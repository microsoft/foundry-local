// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include <nlohmann/json.hpp>

#include <oatpp/web/protocol/http/outgoing/Body.hpp>
#include <oatpp/web/protocol/http/outgoing/Response.hpp>
#include <oatpp/web/server/HttpRequestHandler.hpp>

#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>

namespace fl {

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

/// Generate a random ID with the given prefix (e.g. "chatcmpl").
inline std::string GenerateCompletionId(const std::string& prefix) {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<uint64_t> dist;

  std::ostringstream ss;
  ss << prefix << "-" << std::hex << std::setfill('0') << std::setw(16) << dist(rng);
  return ss.str();
}

/// Extract the User-Agent header from an incoming request ("" if absent), for
/// attribution on the telemetry events the request drives.
inline std::string GetUserAgent(const std::shared_ptr<HttpRequestHandler::IncomingRequest>& request) {
  if (!request) {
    return {};
  }
  auto ua = request->getHeader("User-Agent");
  return ua ? *ua : std::string{};
}

// ========================================================================
// SSE stream body — feeds token-by-token SSE events to oatpp's chunked
// transfer encoding. A producer thread pushes formatted SSE strings into
// a queue; oatpp calls read() to pull chunks out.
// ========================================================================

class SseStreamBody : public oatpp::web::protocol::http::outgoing::Body {
 public:
  SseStreamBody() : done_(false) {}

  /// Push a formatted SSE event (e.g. "data: {...}\n\n") into the queue.
  void Push(std::string chunk) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(chunk));
    cv_.notify_one();
  }

  /// Signal that no more data will be pushed.
  void Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
    cv_.notify_one();
  }

  // -- Body interface --

  oatpp::v_io_size read(void* buffer, v_buff_size count, oatpp::async::Action& /*action*/) override {
    std::unique_lock<std::mutex> lock(mutex_);

    // Wait until data is available or the stream is finished
    cv_.wait(lock, [this] { return !queue_.empty() || done_; });

    if (queue_.empty()) {
      return 0;  // EOF — oatpp ends the chunked response
    }

    // Drain as much queued data as fits in the buffer
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

  void declareHeaders(Headers& headers) override {
    headers.put("Content-Type", "text/event-stream");
    headers.put("Cache-Control", "no-cache");
    headers.put("Connection", "keep-alive");
  }

  p_char8 getKnownData() override { return nullptr; }
  v_int64 getKnownSize() override { return -1; }  // unknown → chunked transfer

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::string> queue_;
  bool done_;
};

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
