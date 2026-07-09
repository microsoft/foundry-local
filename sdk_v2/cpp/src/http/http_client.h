// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "logger.h"

#include <chrono>
#include <functional>
#include <map>
#include <string>

namespace fl {
namespace http {

/// Full HTTP response. `status == 0` indicates a transport-level failure (no HTTP
/// response received), in which case `body` carries the transport error message.
/// Header keys are lowercased for case-insensitive lookup.
struct HttpResponse {
  int status = 0;
  std::map<std::string, std::string> headers;
  std::string body;
};

struct HttpRequestOptions {
  std::string user_agent;
  std::chrono::milliseconds timeout = std::chrono::seconds(30);
  bool close_connection = false;
};

/// Perform an HTTP POST and return status, headers, and body without throwing on non-2xx responses.
/// Transport failures are returned as `status == 0` with the error message in `body`.
HttpResponse HttpPostWithResponse(const std::string& url,
                                  const std::string& json_body,
                                  const HttpRequestOptions& options = {});

/// Perform an HTTP GET and return status, headers, and body without throwing on non-2xx responses.
/// Transport failures are returned as `status == 0` with the error message in `body`.
HttpResponse HttpGetWithResponse(const std::string& url, const HttpRequestOptions& options = {});

/// Human-readable, length-bounded description of a failed response for error messages.
/// `status == 0` becomes "transport failure"; otherwise "HTTP <status>". When the body is
/// non-empty it is appended (truncated to `max_body_chars`) so server-side or transport
/// diagnostics are preserved without bloating logs.
std::string DescribeFailure(const HttpResponse& response, std::size_t max_body_chars = 512);

/// Perform an HTTP GET request. Returns the response body.
/// Throws fl::Exception on HTTP errors or connection failures.
std::string HttpGet(const std::string& url, const HttpRequestOptions& options = {});

/// Perform an HTTP POST request with a JSON body. Returns the response body.
/// Throws fl::Exception on HTTP errors or connection failures.
std::string HttpPost(const std::string& url,
                     const std::string& json_body,
                     const HttpRequestOptions& options = {});

/// Perform an HTTP DELETE request. Returns the response body.
/// Throws fl::Exception on HTTP errors or connection failures.
std::string HttpDelete(const std::string& url, const HttpRequestOptions& options = {});

// ------------------------------------------------------------------
// Retry primitive — used by registry resolution to ride out transient
// 5xx / network blips without giving up after a single attempt.
// ------------------------------------------------------------------

/// Outcome of one attempt of a retryable operation.
enum class RetryDecision {
  Success,         ///< op succeeded; `body` is the result
  RetryTransient,  ///< transient failure (5xx, transport error); retry if budget remains
  FailPermanent    ///< permanent failure (4xx, validation); do not retry
};

struct RetryAttempt {
  RetryDecision decision = RetryDecision::FailPermanent;
  std::string body;           // populated on Success
  std::string error_message;  // populated on RetryTransient / FailPermanent
};

struct RetryConfig {
  int max_retries = 3;                         // 4 attempts total
  std::chrono::milliseconds base_delay{500};   // 500ms, 1s, 2s, ...
  std::chrono::milliseconds max_total{10000};  // overall budget
};

/// Execute `op` up to (config.max_retries + 1) times. Between attempts, sleep for
/// `base_delay * 2^attempt + rand(0..base_delay)`, capped so that total elapsed time stays
/// inside `max_total`. On Success returns `body`. On exhausted retries or FailPermanent
/// throws fl::Exception with FOUNDRY_LOCAL_ERROR_NETWORK.
///
/// `sleep_fn` is injected for tests so they don't block on real wall-clock delays.
std::string RetryWithBackoff(const std::function<RetryAttempt()>& op,
                             const RetryConfig& config,
                             ILogger& logger,
                             const std::function<void(std::chrono::milliseconds)>& sleep_fn = {});

/// Convenience wrapper: HTTP GET with retry on transient 5xx / network errors.
/// 4xx responses are treated as permanent failures (no retry).
std::string HttpGetWithRetry(const std::string& url,
                             ILogger& logger,
                             const HttpRequestOptions& options = {},
                             const RetryConfig& config = {});

}  // namespace http
}  // namespace fl
