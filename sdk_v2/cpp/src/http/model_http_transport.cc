// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "http/model_http_transport.h"

#include "exception.h"
#include "util/string_utils.h"

#include <azure/core/context.hpp>
#include <azure/core/http/transport.hpp>
#include <azure/core/io/body_stream.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace fl::http {
namespace {

constexpr size_t kMetadataLimit = 16 * 1024 * 1024;
using Headers = std::vector<std::pair<std::string, std::string>>;

class CallbackFailure final : public Exception {
 public:
  using Exception::Exception;
};

void CheckStatus(flStatus* status, const char* phase) {
  if (!status) {
    return;
  }

  const auto code = ConsumeModelHttpStatus(*status);
  // Adapter diagnostics can contain signed URLs. Never propagate them into retry logs or telemetry.
  if (code == FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED) {
    throw Azure::Core::OperationCancelledException("Custom model HTTP operation cancelled");
  }

  throw CallbackFailure(FL_WHERE_WITH_STACK, std::string("Custom model HTTP transport failed during ") + phase,
                        code == FOUNDRY_LOCAL_OK ? FOUNDRY_LOCAL_ERROR_NETWORK : code);
}

uint64_t TimeoutMilliseconds(const Azure::Core::Context& context) {
  const auto deadline = context.GetDeadline();
  if (deadline == (Azure::DateTime::max)()) {
    return 0;
  }

  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                             deadline - Azure::DateTime(std::chrono::system_clock::now()))
                             .count();
  return remaining > 0 ? static_cast<uint64_t>(remaining) : 1;
}

Headers MetadataHeaders(const std::string& body, const HttpRequestOptions& options) {
  std::map<std::string, std::string> headers{{"user-agent", options.user_agent}};
  for (const auto& [name, value] : options.headers) {
    headers[ToLower(name)] = value;
  }

  if (!body.empty()) {
    headers["content-type"] = "application/json";
  }

  if (options.close_connection) {
    headers["connection"] = "close";
  }

  return {headers.begin(), headers.end()};
}

/// Owns every request pointer and joins the only concurrent callback caller before releasing the operation.
class Operation final {
 public:
  Operation(std::shared_ptr<ModelHttpTransport> transport, std::string method, std::string url,
            Headers headers, std::vector<uint8_t> body, const Azure::Core::Context& context, uint64_t timeout_ms)
      : transport_(std::move(transport)),
        method_(std::move(method)),
        url_(std::move(url)),
        headers_(std::move(headers)),
        body_(std::move(body)),
        context_(context) {
    for (const auto& [name, value] : headers_) {
      request_headers_.push_back({name.c_str(), value.c_str()});
    }

    request_ = {FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION, method_.c_str(), url_.c_str(), request_headers_.data(),
                request_headers_.size(), body_.data(), body_.size(), timeout_ms};
    auto* status = Callbacks().CreateOperation(Callbacks().user_data, &request_, &handle_);
    if (status && handle_) {
      // An adapter that returned both an operation and an error violated the contract, but must not leak its operation.
      Callbacks().ReleaseOperation(handle_);
      handle_ = nullptr;
    }

    CheckStatus(status, "CreateOperation");
    if (!handle_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Custom model HTTP CreateOperation returned a null operation");
    }
  }

  ~Operation() {
    {
      std::lock_guard lock(mutex_);
      stopped_ = true;
    }

    wake_.notify_one();
    if (monitor_.joinable()) {
      monitor_.join();
    }

    Callbacks().ReleaseOperation(handle_);
  }

  void Send() {
    // The total timeout begins here, not during prompt, allocation-only CreateOperation.
    const auto start = std::chrono::steady_clock::now();
    if (request_.timeout_ms != 0) {
      const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
                               (std::chrono::steady_clock::time_point::max)() - start)
                               .count();
      deadline_ = start + std::chrono::milliseconds(
                              static_cast<int64_t>((std::min)(request_.timeout_ms, static_cast<uint64_t>(maximum))));
    }

    CheckCancelled();
    monitor_ = std::thread([this] { Monitor(); });
    flHttpResponseData response{FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION, 0, nullptr, 0};
    CheckStatus(Callbacks().Send(handle_, &response), "Send");
    CheckCancelled();
    CopyResponse(response);
  }

  size_t Read(std::span<uint8_t> buffer, const Azure::Core::Context& context) {
    if (buffer.empty()) {
      return 0;
    }

    {
      std::lock_guard lock(mutex_);
      read_context_ = context;
    }

    CheckCancelled();
    size_t read = 0;
    CheckStatus(Callbacks().Read(handle_, buffer.data(), buffer.size(), &read), "Read");
    CheckCancelled();
    if (read > buffer.size()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Custom model HTTP Read exceeded buffer capacity");
    }

    return read;
  }

  uint32_t Status() const noexcept { return status_; }
  const Headers& ResponseHeaders() const noexcept { return response_headers_; }

 private:
  const flHttpTransport& Callbacks() const noexcept { return transport_->Callbacks(); }

  bool IsCancelled() {
    std::lock_guard lock(mutex_);
    return context_.IsCancelled() || read_context_.IsCancelled() ||
           std::chrono::steady_clock::now() >= deadline_;
  }

  void Cancel() noexcept {
    if (!cancelled_.exchange(true)) {
      Callbacks().Cancel(handle_);
    }
  }

  void CheckCancelled() {
    if (IsCancelled()) {
      Cancel();
    }

    if (cancelled_.load()) {
      throw Azure::Core::OperationCancelledException("Custom model HTTP operation cancelled or timed out");
    }
  }

  void Monitor() noexcept {
    std::unique_lock lock(mutex_);
    while (!stopped_) {
      if (wake_.wait_for(lock, std::chrono::milliseconds(5), [this] { return stopped_; })) {
        break;
      }

      lock.unlock();
      if (IsCancelled()) {
        Cancel();
        return;
      }

      lock.lock();
    }
  }

  void CopyResponse(const flHttpResponseData& response) {
    if (response.version != FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION ||
        response.status_code < 100 || response.status_code > 599 ||
        (response.header_count != 0 && !response.headers)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Custom model HTTP Send returned invalid response metadata");
    }

    status_ = response.status_code;
    for (size_t i = 0; i < response.header_count; ++i) {
      const auto& header = response.headers[i];
      if (!header.name || !header.value || header.name[0] == '\0') {
        FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Custom model HTTP Send returned an invalid header");
      }

      response_headers_.emplace_back(header.name, header.value);
    }
  }

  std::shared_ptr<ModelHttpTransport> transport_;
  std::string method_;
  std::string url_;
  Headers headers_;
  std::vector<uint8_t> body_;
  std::vector<flHttpHeader> request_headers_;
  flHttpRequestData request_{};
  void* handle_ = nullptr;
  uint32_t status_ = 0;
  Headers response_headers_;
  Azure::Core::Context context_;
  Azure::Core::Context read_context_;
  std::chrono::steady_clock::time_point deadline_ = (std::chrono::steady_clock::time_point::max)();
  std::atomic<bool> cancelled_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopped_ = false;
  std::thread monitor_;
};

int64_t ContentLength(const Headers& headers) {
  for (const auto& [name, value] : headers) {
    if (ToLower(name) == "content-length") {
      int64_t length = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), length);
      if (error != std::errc{} || end != value.data() + value.size() || length < 0) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Custom model HTTP returned invalid content-length");
      }

      return length;
    }
  }

  return -1;
}

class BodyStream final : public Azure::Core::IO::BodyStream {
 public:
  explicit BodyStream(std::shared_ptr<Operation> operation)
      : operation_(std::move(operation)), length_(ContentLength(operation_->ResponseHeaders())) {}
  int64_t Length() const override { return length_; }

 private:
  size_t OnRead(uint8_t* buffer, size_t count, const Azure::Core::Context& context) override {
    try {
      return operation_->Read({buffer, count}, context);
    } catch (const CallbackFailure& error) {
      throw Azure::Core::Http::TransportException(error.what());
    }
  }

  std::shared_ptr<Operation> operation_;
  int64_t length_;
};

class AzureTransport final : public Azure::Core::Http::HttpTransport {
 public:
  explicit AzureTransport(std::shared_ptr<ModelHttpTransport> transport) : transport_(std::move(transport)) {}

  std::unique_ptr<Azure::Core::Http::RawResponse> Send(
      Azure::Core::Http::Request& request, const Azure::Core::Context& context) override {
    try {
      return SendRequest(request, context);
    } catch (const CallbackFailure& error) {
      // Azure's retry policy recognizes only TransportException, not arbitrary std::exception types.
      throw Azure::Core::Http::TransportException(error.what());
    }
  }

 private:
  std::unique_ptr<Azure::Core::Http::RawResponse> SendRequest(
      Azure::Core::Http::Request& request, const Azure::Core::Context& context) {
    Headers headers;
    for (const auto& [name, value] : request.GetHeaders()) {
      headers.emplace_back(name, value);
    }

    auto operation = std::make_shared<Operation>(
        transport_, request.GetMethod().ToString(), request.GetUrl().GetAbsoluteUrl(), std::move(headers),
        CopyRequestBody(request, context), context, TimeoutMilliseconds(context));
    operation->Send();
    auto response = std::make_unique<Azure::Core::Http::RawResponse>(
        1, 1, static_cast<Azure::Core::Http::HttpStatusCode>(operation->Status()), "");
    // Azure's map cannot represent duplicate header names. The public callbacks and Operation preserve them;
    // this compatibility boundary necessarily uses Azure's SetHeader behavior.
    for (const auto& [name, value] : operation->ResponseHeaders()) {
      response->SetHeader(name, value);
    }

    response->SetBodyStream(std::make_unique<BodyStream>(std::move(operation)));
    return response;
  }

  static std::vector<uint8_t> CopyRequestBody(
      Azure::Core::Http::Request& request, const Azure::Core::Context& context) {
    auto* stream = request.GetBodyStream();
    if (!stream) {
      return {};
    }

    const auto length = stream->Length();
    if (length < 0 || static_cast<uint64_t>(length) > kMetadataLimit) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Custom model HTTP request body exceeds metadata limit");
    }

    if (length == 0) {
      return {};
    }

    std::vector<uint8_t> body(static_cast<size_t>(length));
    if (stream->ReadToCount(body.data(), body.size(), context) != body.size()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Custom model HTTP request body ended prematurely");
    }

    return body;
  }

  std::shared_ptr<ModelHttpTransport> transport_;
};

}  // namespace

ModelHttpTransport::ModelHttpTransport(const flHttpTransport& callbacks) : callbacks_{} {
  if (callbacks.version != FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Custom model HTTP transport version is unsupported");
  }

  if (!callbacks.Retain || !callbacks.Release || !callbacks.CreateOperation || !callbacks.Send ||
      !callbacks.Read || !callbacks.Cancel || !callbacks.ReleaseOperation) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Custom model HTTP transport requires every callback");
  }

  callbacks_ = callbacks;
  callbacks_.Retain(callbacks_.user_data);
}

ModelHttpTransport::~ModelHttpTransport() { callbacks_.Release(callbacks_.user_data); }

HttpResponse ModelHttpTransport::Request(const std::string& method, const std::string& url, const std::string& body,
                                         const HttpRequestOptions& options) try {
  auto operation = std::make_shared<Operation>(
      shared_from_this(), method, url, MetadataHeaders(body, options), std::vector<uint8_t>(body.begin(), body.end()),
      Azure::Core::Context{}, options.timeout.count() > 0 ? static_cast<uint64_t>(options.timeout.count()) : 0);
  operation->Send();
  HttpResponse response;
  response.status = static_cast<int>(operation->Status());
  for (const auto& [name, value] : operation->ResponseHeaders()) {
    response.headers[ToLower(name)] = value;
  }

  std::array<uint8_t, 64 * 1024> scratch{};
  while (const auto read = operation->Read(scratch, Azure::Core::Context{})) {
    if (read > kMetadataLimit - response.body.size()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "Custom model HTTP metadata response exceeds 16 MiB");
    }

    response.body.append(reinterpret_cast<const char*>(scratch.data()), read);
  }

  return response;
} catch (const CallbackFailure& error) {
  // Match HttpGet/PostWithResponse so the existing catalog retry and regional fallback policies still apply.
  return HttpResponse{0, {}, error.what()};
} catch (const Azure::Core::OperationCancelledException&) {
  return HttpResponse{0, {}, "Custom model HTTP operation cancelled or timed out"};
}

std::shared_ptr<Azure::Core::Http::HttpTransport> ModelHttpTransport::AzureTransport() {
  return std::make_shared<http::AzureTransport>(shared_from_this());
}

}  // namespace fl::http
