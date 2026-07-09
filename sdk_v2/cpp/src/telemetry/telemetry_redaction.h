// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace fl {
namespace telemetry_detail {

inline bool LooksLikePath(std::string_view token) {
  if (token.find("://") != std::string_view::npos) {
    return true;
  }
  if (token.find('\\') != std::string_view::npos) {
    return true;
  }
  if (token.size() >= 3 && std::isalpha(static_cast<unsigned char>(token[0])) && token[1] == ':' &&
      (token[2] == '\\' || token[2] == '/')) {
    return true;
  }
  if (token.size() >= 2 && token[0] == '~' && (token[1] == '/' || token[1] == '\\')) {
    return true;
  }

  int segments = 0;
  for (size_t i = 0; i + 1 < token.size(); ++i) {
    if (token[i] == '/' && token[i + 1] != '/') {
      ++segments;
    }
  }
  return segments >= 2;
}

inline std::string RedactPathToken(std::string_view token) {
  const auto is_sep = [](char c) { return c == '/' || c == '\\'; };
  const auto query_start = token.find_first_of("?#");
  if (query_start != std::string_view::npos) {
    token = token.substr(0, query_start);
  }

  std::string normalized(token);
  for (char& c : normalized) {
    c = (c == '\\') ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  size_t safe_start = 0;
  const auto guard = [&](std::string_view marker, bool has_user) {
    for (size_t pos = normalized.find(marker); pos != std::string::npos;
         pos = normalized.find(marker, pos + 1)) {
      size_t end = pos + marker.size();
      if (has_user) {
        while (end < token.size() &&
               (is_sep(token[end]) ||
                (token[end] == '.' && (end + 1 == token.size() || is_sep(token[end + 1]))))) {
          ++end;
        }
        while (end < token.size() && !is_sep(token[end])) {
          ++end;
        }
      } else {
        --end;
      }
      if (end > safe_start) {
        safe_start = end;
      }
    }
  };
  guard("/home/", true);
  guard("/users/", true);
  guard("/root/", false);
  guard("~/", false);

  size_t scan_end = token.size();
  while (scan_end > 0 && is_sep(token[scan_end - 1])) {
    --scan_end;
  }

  size_t tail_start = token.size();
  if (scan_end > 0) {
    const size_t last_sep = token.find_last_of("/\\", scan_end - 1);
    if (last_sep != std::string_view::npos) {
      const size_t prev =
          (last_sep == 0) ? std::string_view::npos : token.find_last_of("/\\", last_sep - 1);
      tail_start = (prev == std::string_view::npos) ? last_sep : prev;
    }
  }

  const size_t keep_from = (tail_start > safe_start) ? tail_start : safe_start;
  std::string out = "[path]";
  if (keep_from < token.size()) {
    out.append(token.data() + keep_from, token.size() - keep_from);
  }
  return out;
}

}  // namespace telemetry_detail

inline constexpr size_t kMaxTelemetryErrorMessageLength = 256;

inline std::string ScrubTelemetryErrorMessage(std::string_view message) {
  std::string out;
  out.reserve(message.size());

  size_t i = 0;
  while (i < message.size()) {
    if (std::isspace(static_cast<unsigned char>(message[i]))) {
      out.push_back(message[i]);
      ++i;
      continue;
    }

    const size_t start = i;
    while (i < message.size() && !std::isspace(static_cast<unsigned char>(message[i]))) {
      ++i;
    }

    const std::string_view token = message.substr(start, i - start);
    if (telemetry_detail::LooksLikePath(token)) {
      out += telemetry_detail::RedactPathToken(token);
    } else {
      out.append(token.data(), token.size());
    }
  }

  if (out.size() > kMaxTelemetryErrorMessageLength) {
    out.resize(kMaxTelemetryErrorMessageLength);
  }
  return out;
}

}  // namespace fl
