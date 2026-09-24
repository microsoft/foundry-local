// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "exception.h"
#include "inferencing/session/request.h"

#include <nlohmann/json.hpp>

#include <oatpp/web/protocol/http/outgoing/Body.hpp>
#include <oatpp/web/protocol/http/outgoing/Response.hpp>
#include <oatpp/web/server/HttpRequestHandler.hpp>

#include "model.h"
#include "service/web_service.h"
#include "telemetry/telemetry_action_tracker.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace fl {

class GenAIModelInstance;

using oatpp::web::protocol::http::Status;
using oatpp::web::server::HttpRequestHandler;

// ========================================================================
// JSON helper — build an oatpp response from nlohmann::json
// ========================================================================

inline std::shared_ptr<HttpRequestHandler::OutgoingResponse> JsonResponse(const Status& status,
                                                                          const nlohmann::json& body) {
  auto response = HttpRequestHandler::ResponseFactory::createResponse(
      status, body.dump());
  response->putHeader("Content-Type", "application/json");
  return response;
}

inline std::shared_ptr<HttpRequestHandler::OutgoingResponse> ErrorResponse(const Status& status,
                                                                           const std::string& message,
                                                                           const std::string& detail = "") {
  std::string full_message = detail.empty() ? message : message + ": " + detail;

  nlohmann::json error_obj = {
      {"message", full_message},
      {"type", status.code >= 500 ? "server_error" : "invalid_request_error"},
      {"param", nullptr},
      {"code", nullptr},
  };

  nlohmann::json body = {{"error", error_obj}};
  return JsonResponse(status, body);
}

inline ActionStatus ResponseToActionStatus(const std::shared_ptr<HttpRequestHandler::OutgoingResponse>& response) {
  if (!response) {
    return ActionStatus::kFailure;
  }

  const auto code = response->getStatus().code;
  if (code == 408 || code == 504) {
    return ActionStatus::kTimeout;
  }

  if (code >= 500) {
    return ActionStatus::kFailure;
  }

  if (code >= 400) {
    return ActionStatus::kClientError;
  }

  return ActionStatus::kSuccess;
}

inline std::string SafeHttpUserAgent(std::string_view value) {
  constexpr std::string_view products[] = {
      "foundry-local-core/", "foundry-local-cpp/", "foundry-local-csharp/",
      "foundry-local-python/", "foundry-local-js/", "foundry-local-rust/"};
  for (const auto product : products) {
    if (!value.starts_with(product)) {
      continue;
    }

    const auto version = value.substr(product.size());
    const auto release = version.substr(0, std::min(version.find('-'), version.find(".dev")));
    const auto suffix = version.substr(release.size());
    if (release.empty() || release.size() > 32 || release.find("..") != std::string_view::npos ||
        release.front() < '0' || release.front() > '9' ||
        release.back() < '0' || release.back() > '9' ||
        !std::all_of(release.begin(), release.end(), [](unsigned char ch) {
          return (ch >= '0' && ch <= '9') || ch == '.';
        })) {
      return "unknown-http-client";
    }

    constexpr std::string_view prerelease_prefixes[] = {
        "-dev.local.", "-dev.", ".dev", "-rc.", "-rc", "-beta.", "-alpha.", "-preview."};
    bool supported_suffix = suffix.empty();
    for (const auto prefix : prerelease_prefixes) {
      if (suffix.starts_with(prefix)) {
        const auto number = suffix.substr(prefix.size());
        supported_suffix = !number.empty() && number.size() <= 14 &&
                           std::all_of(number.begin(), number.end(),
                                       [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
        break;
      }
    }

    if (!supported_suffix) {
      return "unknown-http-client";
    }

    return std::string(product) + std::string(release);
  }

  return "unknown-http-client";
}

inline std::string GetUserAgent(const std::shared_ptr<HttpRequestHandler::IncomingRequest>& request) {
  if (!request) {
    return "unknown-http-client";
  }

  const auto user_agent = request->getHeader("User-Agent");
  return user_agent ? SafeHttpUserAgent(*user_agent) : "unknown-http-client";
}

/// Track construction separately from processing, with the route's indirect context for both.
template <typename SessionType>
std::unique_ptr<SessionType> CreateSessionWithTelemetry(const Model& model, GenAIModelInstance& loaded,
                                                        ServiceContext& ctx, const InvocationContext& context) {
  ActionTracker tracker(Action::kSessionCreate, ctx.telemetry, context);
  tracker.SetModelId(model.Id());
  try {
    auto session = std::make_unique<SessionType>(model, loaded, ctx.logger, ctx.telemetry);
    session->SetInvocationContext(context);
    tracker.SetStatus(ActionStatus::kSuccess);
    return session;
  } catch (const std::exception& ex) {
    tracker.RecordException(ex);
    throw;
  }
}

/// Map a failure raised during request handling to an HTTP status.
///
/// The inference path validates what the caller sent: tool results must reference an outstanding call, call IDs must
/// be unique, and supplied tool-call arguments must be a JSON object. Those rejections are client mistakes, so they
/// surface as 400 invalid_request_error. Every other failure is a service failure and stays a 500.
inline Status StatusForException(const fl::Exception& ex) {
  return ex.code() == FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT ? Status::CODE_400 : Status::CODE_500;
}

/// The OpenAI error `type` matching an HTTP status.
inline const char* ErrorTypeForStatus(const Status& status) {
  return status.code >= 500 ? "server_error" : "invalid_request_error";
}

/// Generate a random ID with the given prefix (e.g. "chatcmpl").
inline std::string GenerateCompletionId(const std::string& prefix) {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<uint64_t> dist;

  std::ostringstream ss;
  ss << prefix << "-" << std::hex << std::setfill('0') << std::setw(16) << dist(rng);
  return ss.str();
}

// ========================================================================
// SSE stream body — the producer owns the state, not the oatpp Body. This lets
// a failed socket write destroy the Body and cancel inference while the producer is still running.
// ========================================================================

class SseStreamState {
 public:
  void Push(std::string chunk) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disconnected_) {
      return;
    }

    queue_.push(std::move(chunk));
    cv_.notify_one();
  }

  void Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
    cv_.notify_one();
  }

  void BindRequest(const std::shared_ptr<Request>& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    request_ = request;
  }

  bool IsDisconnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return disconnected_;
  }

  void BodyClosed() {
    std::shared_ptr<Request> request;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (eof_observed_) {
        return;
      }

      disconnected_ = true;
      request = request_.lock();
      std::queue<std::string> empty;
      queue_.swap(empty);
    }

    if (request) {
      request->CancelCurrentOrNext();
    }
  }

 private:
  friend class SseStreamBody;

  oatpp::v_io_size Read(void* buffer, v_buff_size count) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(250),
                      [this] { return !queue_.empty() || done_; })) {
      queue_.push(": keep-alive\n\n");
    }

    if (queue_.empty()) {
      eof_observed_ = true;
      return 0;
    }

    oatpp::v_io_size total = 0;
    auto* dst = static_cast<char*>(buffer);

    while (!queue_.empty() && total < count) {
      auto& front = queue_.front();
      auto remaining = count - total;
      auto to_copy = static_cast<v_buff_size>(
          std::min(static_cast<v_buff_size>(front.size()), static_cast<v_buff_size>(remaining)));

      std::memcpy(dst + total, front.data(), to_copy);
      total += to_copy;

      if (to_copy < static_cast<v_buff_size>(front.size())) {
        // Partial read — keep the rest for next call
        front = front.substr(to_copy);
        break;
      }

      queue_.pop();
    }

    return total;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::string> queue_;
  std::weak_ptr<Request> request_;
  bool done_ = false;
  bool disconnected_ = false;
  bool eof_observed_ = false;
};

class SseStreamBody : public oatpp::web::protocol::http::outgoing::Body {
 public:
  SseStreamBody() : state_(std::make_shared<SseStreamState>()) {}
  ~SseStreamBody() override { state_->BodyClosed(); }

  std::shared_ptr<SseStreamState> Stream() const { return state_; }

  void Push(std::string chunk) { state_->Push(std::move(chunk)); }
  void Finish() { state_->Finish(); }

  oatpp::v_io_size read(void* buffer, v_buff_size count, oatpp::async::Action& /*action*/) override {
    return state_->Read(buffer, count);
  }

  void declareHeaders(Headers& headers) override {
    headers.put("Content-Type", "text/event-stream");
    headers.put("Cache-Control", "no-cache");
    headers.put("Connection", "keep-alive");
  }

  p_char8 getKnownData() override { return nullptr; }
  v_int64 getKnownSize() override { return -1; }  // unknown → chunked transfer

 private:
  std::shared_ptr<SseStreamState> state_;
};

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
