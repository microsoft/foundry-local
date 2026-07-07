// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "http/http_client.h"
#include "exception.h"
#include "utils.h"

#include <azure/core/context.hpp>
#include <azure/core/datetime.hpp>
#include <azure/core/http/http.hpp>
#include <azure/core/http/raw_response.hpp>
#include <azure/core/io/body_stream.hpp>

// Desktop Windows uses the WinHTTP transport (OS SChannel TLS, no background threads)
// and drops libcurl entirely to shrink the binary and avoid curl's process-lifetime
// connection-pool cleanup thread, which races with host-runtime teardown (Node/V8).
// UWP and non-Windows builds keep the libcurl transport to match the vcpkg feature selection.
// FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT is set by CMake for non-UWP Windows builds.
#if defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)
#include <azure/core/http/win_http_transport.hpp>
#else
#include <azure/core/http/curl_transport.hpp>
#endif

#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fl {
namespace http {

namespace {

struct HttpRawResult {
  int status = 0;                              // 0 indicates a transport-level failure (no HTTP response)
  std::string body;                            // response body or transport error message when status==0
  std::map<std::string, std::string> headers;  // response headers, keys lowercased
};

HttpRawResult HttpRequestRaw(const Azure::Core::Http::HttpMethod& method,
                             const std::string& url,
                             const std::string& body,
                             const HttpRequestOptions& options) {
  using namespace Azure::Core;
  using namespace Azure::Core::Http;

#if defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)
  WinHttpTransport transport;
#else
  CurlTransport transport;
#endif

  // Build the request. For methods with a body (POST), attach a MemoryBodyStream.
  std::vector<uint8_t> body_bytes(body.begin(), body.end());
  IO::MemoryBodyStream body_stream(body_bytes);

  Request request = body.empty()
                        ? Request(method, Url(url))
                        : Request(method, Url(url), &body_stream);

  request.SetHeader("User-Agent", options.user_agent);

  if (options.close_connection) {
    request.SetHeader("Connection", "close");
  }

  if (!body.empty()) {
    request.SetHeader("Content-Type", "application/json");
  }

  Context context = Context{}.WithDeadline(
      Azure::DateTime(std::chrono::system_clock::now() + options.timeout));

  std::unique_ptr<RawResponse> response;
  try {
    response = transport.Send(request, context);
  } catch (const std::exception& e) {
    // Transport-level failure (DNS, connect, timeout, TLS, etc.). Surface as status==0
    // with the message so the retry layer can classify this as a transient error.
    return HttpRawResult{0, std::string("transport error: ") + e.what(), {}};
  }

  HttpRawResult result;
  result.status = static_cast<int>(response->GetStatusCode());

  // Copy response headers with lowercased keys for case-insensitive lookup.
  for (const auto& [key, value] : response->GetHeaders()) {
    result.headers[ToLower(key)] = value;
  }

  // Read the response body — prefer the stream if available, otherwise use the buffered body.
  auto response_stream = response->ExtractBodyStream();

  if (response_stream) {
    auto bytes = response_stream->ReadToEnd(context);
    result.body.assign(bytes.begin(), bytes.end());
  } else {
    auto& bytes = response->GetBody();
    result.body.assign(bytes.begin(), bytes.end());
  }

  return result;
}

std::string HttpRequest(const Azure::Core::Http::HttpMethod& method,
                        const std::string& url,
                        const std::string& body,
                        const HttpRequestOptions& options) {
  auto raw = HttpRequestRaw(method, url, body, options);

  if (raw.status == 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "HTTP request failed for " + url + " (" + raw.body + ")");
  }

  if (raw.status < 200 || raw.status >= 300) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "HTTP " + std::to_string(raw.status) + " from " + url +
                                              ": " + raw.body);
  }

  return raw.body;
}

bool IsTransientStatus(int status) {
  // 0 = transport-level failure (DNS, connect, timeout). Treat as transient.
  // 408 (Request Timeout) and 429 (Too Many Requests) are also retryable on the
  // server side, but the spec for this fix calls out only 5xx — keep the rule narrow.
  return status == 0 || status == 500 || status == 502 || status == 503 || status == 504;
}

}  // anonymous namespace

std::string HttpGet(const std::string& url, const HttpRequestOptions& options) {
  return HttpRequest(Azure::Core::Http::HttpMethod::Get, url, "", options);
}

HttpResponse HttpPostWithResponse(const std::string& url,
                                  const std::string& json_body,
                                  const HttpRequestOptions& options) {
  auto raw = HttpRequestRaw(Azure::Core::Http::HttpMethod::Post, url, json_body, options);
  return HttpResponse{raw.status, std::move(raw.headers), std::move(raw.body)};
}

HttpResponse HttpGetWithResponse(const std::string& url, const HttpRequestOptions& options) {
  auto raw = HttpRequestRaw(Azure::Core::Http::HttpMethod::Get, url, "", options);
  return HttpResponse{raw.status, std::move(raw.headers), std::move(raw.body)};
}

std::string DescribeFailure(const HttpResponse& response, std::size_t max_body_chars) {
  std::string detail = response.status == 0 ? "transport failure" : "HTTP " + std::to_string(response.status);

  if (!response.body.empty()) {
    if (response.body.size() > max_body_chars) {
      detail += ": " + response.body.substr(0, max_body_chars) + "... (truncated)";
    } else {
      detail += ": " + response.body;
    }
  }

  return detail;
}

std::string HttpPost(const std::string& url, const std::string& json_body,
                     const HttpRequestOptions& options) {
  return HttpRequest(Azure::Core::Http::HttpMethod::Post, url, json_body, options);
}

std::string HttpDelete(const std::string& url, const HttpRequestOptions& options) {
  return HttpRequest(Azure::Core::Http::HttpMethod::Delete, url, "", options);
}

std::string RetryWithBackoff(const std::function<RetryAttempt()>& op,
                             const RetryConfig& config,
                             ILogger& logger,
                             const std::function<void(std::chrono::milliseconds)>& sleep_fn) {
  const auto start = std::chrono::steady_clock::now();
  const int total_attempts = config.max_retries + 1;

  // Per-call RNG seeded from a clock — jitter only needs to be unpredictable,
  // not cryptographically strong, and we don't want global state.
  std::mt19937_64 rng{static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count())};

  std::string last_error;

  for (int attempt = 0; attempt < total_attempts; ++attempt) {
    RetryAttempt result = op();

    if (result.decision == RetryDecision::Success) {
      if (attempt > 0) {
        logger.Log(LogLevel::Information,
                   "Operation succeeded after " + std::to_string(attempt) + " retr" +
                       (attempt == 1 ? "y" : "ies"));
      }
      return std::move(result.body);
    }

    last_error = std::move(result.error_message);

    if (result.decision == RetryDecision::FailPermanent) {
      logger.Log(LogLevel::Warning,
                 "Operation failed permanently (no retry): " + last_error);
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, last_error);
    }

    // Transient. Decide whether we have budget for another attempt.
    if (attempt + 1 >= total_attempts) {
      break;
    }

    // Exponential backoff with jitter. base * 2^attempt + rand(0..base).
    auto base_ms = config.base_delay.count();
    auto exp_ms = base_ms * (1LL << attempt);
    auto jitter_ms = base_ms > 0 ? static_cast<int64_t>(rng() % static_cast<uint64_t>(base_ms)) : 0;
    auto sleep_ms = std::chrono::milliseconds(exp_ms + jitter_ms);

    // Cap so the total elapsed time stays inside max_total.
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    auto remaining = config.max_total - elapsed;

    if (remaining <= std::chrono::milliseconds::zero()) {
      break;
    }

    if (sleep_ms > remaining) {
      sleep_ms = remaining;
    }

    logger.Log(LogLevel::Information,
               "Transient failure on attempt " + std::to_string(attempt + 1) + " of " +
                   std::to_string(total_attempts) + " (" + last_error + "); retrying in " +
                   std::to_string(sleep_ms.count()) + "ms");

    if (sleep_fn) {
      sleep_fn(sleep_ms);
    } else {
      std::this_thread::sleep_for(sleep_ms);
    }
  }

  logger.Log(LogLevel::Warning,
             "Operation failed after " + std::to_string(total_attempts) +
                 " attempts: " + last_error);
  FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK,
           "operation failed after " + std::to_string(total_attempts) + " attempts: " + last_error);
}

std::string HttpGetWithRetry(const std::string& url,
                             ILogger& logger,
                             const HttpRequestOptions& options,
                             const RetryConfig& config) {
  auto op = [&]() -> RetryAttempt {
    auto raw = HttpRequestRaw(Azure::Core::Http::HttpMethod::Get, url, "", options);

    RetryAttempt attempt;

    if (raw.status >= 200 && raw.status < 300) {
      attempt.decision = RetryDecision::Success;
      attempt.body = std::move(raw.body);
      return attempt;
    }

    std::string description = raw.status == 0
                                  ? ("transport failure for " + url + ": " + raw.body)
                                  : ("HTTP " + std::to_string(raw.status) + " from " + url + ": " + raw.body);

    attempt.decision = IsTransientStatus(raw.status) ? RetryDecision::RetryTransient
                                                     : RetryDecision::FailPermanent;
    attempt.error_message = std::move(description);
    return attempt;
  };

  return RetryWithBackoff(op, config, logger);
}

}  // namespace http
}  // namespace fl
