// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace fl {

inline constexpr size_t kMaxTelemetryStringLength = 40'960;

namespace telemetry_detail {

inline size_t FindPathAnchor(std::string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '\\' && i + 1 < s.size() && s[i + 1] == '\\') {
      return i;
    }
    if (c == '~' && i + 1 < s.size() && (s[i + 1] == '/' || s[i + 1] == '\\')) {
      return i;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) && i + 2 < s.size() && s[i + 1] == ':' &&
        (s[i + 2] == '\\' || s[i + 2] == '/')) {
      return i;
    }
    if (c == '\\') {
      size_t start = i;
      while (start > 0) {
        const unsigned char prev = static_cast<unsigned char>(s[start - 1]);
        if (std::isspace(prev) || s[start - 1] == '"' || s[start - 1] == '\'') {
          break;
        }
        --start;
      }

      size_t separators = 0;
      for (size_t j = i; j < s.size() && s[j] != '\r' && s[j] != '\n'; ++j) {
        if (s[j] == '\\' && ++separators >= 2) {
          return start;
        }
      }
    }
    if (c == '/') {
      if (i == 0) {
        return i;
      }

      const unsigned char prev_anchor = static_cast<unsigned char>(s[i - 1]);
      if ((std::isspace(prev_anchor) || s[i - 1] == '"' || s[i - 1] == '\'') && i + 1 < s.size() &&
          !std::isspace(static_cast<unsigned char>(s[i + 1]))) {
        return i;
      }

      size_t segments = 0;
      bool segment_has_dot = false;
      size_t j = i;
      while (j < s.size() && s[j] == '/') {
        const size_t seg_start = ++j;
        while (j < s.size() && s[j] != '/' && s[j] != '\r' && s[j] != '\n' && s[j] != ' ' &&
               s[j] != '\t') {
          segment_has_dot = segment_has_dot || s[j] == '.';
          ++j;
        }
        if (j > seg_start) {
          ++segments;
        } else {
          break;
        }
      }

      if (segments >= 2 || (segments == 1 && segment_has_dot)) {
        size_t start = i;
        while (start > 0) {
          const unsigned char prev = static_cast<unsigned char>(s[start - 1]);
          if (std::isspace(prev) || s[start - 1] == '"' || s[start - 1] == '\'') {
            break;
          }
          --start;
        }
        return start;
      }
    }
  }
  return std::string_view::npos;
}

inline void TruncateUtf8AtBoundary(std::string& s, size_t max_length) {
  if (s.size() <= max_length) {
    return;
  }

  size_t end = max_length;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
    --end;
  }
  s.resize(end);
}

}  // namespace telemetry_detail

inline std::string ScrubStringForTelemetry(std::string_view msg) {
  const size_t anchor = telemetry_detail::FindPathAnchor(msg);
  std::string out;
  if (anchor == std::string_view::npos) {
    out.assign(msg);
  } else {
    out.assign(msg.substr(0, anchor));
    out += "[path]";
  }
  if (out.size() > kMaxTelemetryStringLength) {
    telemetry_detail::TruncateUtf8AtBoundary(out, kMaxTelemetryStringLength);
  }
  return out;
}

}  // namespace fl
