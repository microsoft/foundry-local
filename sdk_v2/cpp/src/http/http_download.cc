// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "http/http_download.h"

#include "http/http_client.h"
#include "logger.h"
#include "util/string_utils.h"

#include <azure/core/context.hpp>
#include <azure/core/http/http.hpp>
#include <azure/core/io/body_stream.hpp>

// See http_client.cc: desktop Windows uses WinHTTP; UWP and non-Windows builds use libcurl.
// FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT is set by CMake for non-UWP Windows builds.
#if defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)
#include <azure/core/http/win_http_transport.hpp>
#else
#include "http/curl_transport.h"

#include <azure/core/http/curl_transport.hpp>
#endif

#include <charconv>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fl {

namespace {

std::string RedactUrlForLog(const std::string& url) {
  const auto query = url.find('?');
  return query == std::string::npos ? url : url.substr(0, query) + "?<redacted>";
}

}  // namespace

std::optional<int64_t> ParseContentLengthHeader(std::string_view value) {
  constexpr auto is_http_whitespace = [](char ch) { return ch == ' ' || ch == '\t'; };
  while (!value.empty() && is_http_whitespace(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_http_whitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.empty()) {
    return std::nullopt;
  }

  int64_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed < 0) {
    return std::nullopt;
  }

  return parsed;
}

bool ContentLengthMatchesBody(int64_t content_length, int64_t bytes_downloaded) {
  return content_length < 0 || content_length == bytes_downloaded;
}

bool HttpDownloadFile(const std::string& url, const std::filesystem::path& destination, const std::string& user_agent,
                      std::atomic<bool>* cancel_flag, std::function<void(float percent)> progress_cb, ILogger& logger,
                      int64_t max_bytes) {
  using namespace Azure::Core;
  using namespace Azure::Core::Http;

  // Ensure parent directory exists
  auto parent = destination.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

#if defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)
  WinHttpTransport transport;
#else
  // On Android, libcurl has no valid default CA path, so MakeCurlTransportOptions() supplies the CA
  // bundle via CAInfo; on other curl platforms it returns empty options and libcurl uses the system
  // default trust store (see http/curl_transport.h).
  CurlTransport transport(http::MakeCurlTransportOptions());
#endif
  Request request(HttpMethod::Get, Url(url));
  request.SetHeader("User-Agent", user_agent);

  // Long timeout for large downloads (30 minutes)
  Context context =
      Context{}.WithDeadline(Azure::DateTime(std::chrono::system_clock::now() + std::chrono::minutes(30)));

  std::unique_ptr<RawResponse> response;
  const auto log_url = RedactUrlForLog(url);

  try {
    response = transport.Send(request, context);
  } catch (const std::exception& ex) {
    logger.Log(LogLevel::Warning, MakeString("HTTP download failed for ", log_url, ": ", ex.what()));
    return false;
  } catch (...) {
    logger.Log(LogLevel::Warning, MakeString("HTTP download failed for ", log_url, ": unknown exception"));
    return false;
  }

  auto status = static_cast<int>(response->GetStatusCode());
  if (status < 200 || status >= 300) {
    logger.Log(LogLevel::Warning, MakeString("HTTP download failed for ", log_url, ": HTTP status ", status));
    return false;
  }

  auto body_stream = response->ExtractBodyStream();
  if (!body_stream) {
    logger.Log(LogLevel::Warning, MakeString("HTTP download failed for ", log_url, ": no body stream in response"));
    return false;
  }

  // Get content length for progress reporting and truncation detection.
  // A missing Content-Length is legitimate (e.g. chunked transfer encoding), but a *malformed* one
  // is a strong signal of server misbehavior. Fail loudly rather than silently disabling
  // truncation validation for this download.
  int64_t content_length = -1;
  auto cl_header = response->GetHeaders().find("content-length");
  if (cl_header != response->GetHeaders().end()) {
    const auto parsed_content_length = ParseContentLengthHeader(cl_header->second);
    if (!parsed_content_length.has_value()) {
      logger.Log(LogLevel::Warning, MakeString("HTTP download: invalid Content-Length header for ", log_url, " (\"",
                                               cl_header->second, "\")"));
      return false;
    }

    content_length = *parsed_content_length;

    if (max_bytes >= 0 && content_length > max_bytes) {
      logger.Log(LogLevel::Warning,
                 MakeString("HTTP download: Content-Length ", content_length, " for ", log_url, " exceeds the ",
                            max_bytes, "-byte cap; refusing before reading any body"));
      return false;
    }
  }

  std::ofstream out(destination, std::ios::binary);
  if (!out) {
    logger.Log(LogLevel::Warning, MakeString("HTTP download: failed to open output file ", destination.string()));
    return false;
  }

  constexpr size_t kBufferSize = 65536;
  uint8_t buffer[kBufferSize];
  int64_t bytes_downloaded = 0;
  int chunks_since_progress = 0;
  auto remove_destination = [&]() {
    std::error_code ec;
    std::filesystem::remove(destination, ec);
  };

  try {
    while (true) {
      if (cancel_flag && cancel_flag->load()) {
        out.close();
        remove_destination();
        return false;
      }

      size_t bytes_read = body_stream->Read(buffer, kBufferSize, context);
      if (bytes_read == 0) {
        break;
      }

      out.write(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(bytes_read));
      if (!out) {
        logger.Log(LogLevel::Warning, MakeString("HTTP download: failed writing ", destination.string()));
        out.close();
        remove_destination();
        return false;
      }
      bytes_downloaded += static_cast<int64_t>(bytes_read);

      if (max_bytes >= 0 && bytes_downloaded > max_bytes) {
        logger.Log(LogLevel::Warning,
                   MakeString("HTTP download: body for ", log_url, " exceeded the ", max_bytes, "-byte cap"));
        out.close();
        remove_destination();
        return false;
      }

      chunks_since_progress++;
      if (progress_cb && content_length > 0 && chunks_since_progress >= 32) {
        float percent = static_cast<float>(bytes_downloaded * 100.0 / content_length);
        progress_cb(percent);
        chunks_since_progress = 0;
      }
    }
  } catch (const std::exception& ex) {
    logger.Log(LogLevel::Warning, MakeString("HTTP download failed while reading ", log_url, ": ", ex.what()));
    out.close();
    remove_destination();
    return false;
  }

  out.close();
  if (!out) {
    logger.Log(LogLevel::Warning, MakeString("HTTP download: failed finalizing ", destination.string()));
    remove_destination();
    return false;
  }

  if (!ContentLengthMatchesBody(content_length, bytes_downloaded)) {
    logger.Log(LogLevel::Warning, MakeString("HTTP download length mismatch for ", log_url, ": got ",
                                             bytes_downloaded, " bytes, expected ", content_length));
    remove_destination();
    return false;
  }

  if (progress_cb) {
    progress_cb(100.0f);
  }

  return true;
}

}  // namespace fl
