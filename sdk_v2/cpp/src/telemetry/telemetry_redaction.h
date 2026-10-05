// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace fl {

// Keep this implementation aligned with ONNX Runtime's core/platform/telemetry_redaction.h.
inline constexpr size_t kMaxTelemetryStringLength = 40'960;

namespace telemetry_detail {

// Returns the first filesystem-path anchor. Redaction runs from that anchor to the end because paths may contain
// spaces, including user names.
inline size_t FindPathAnchor(std::string_view value) {
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '\\' && i + 1 < value.size() && value[i + 1] == '\\') {
      return i;
    }
    if (c == '~' && i + 1 < value.size() && (value[i + 1] == '/' || value[i + 1] == '\\')) {
      return i;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) && i + 2 < value.size() && value[i + 1] == ':' &&
        (value[i + 2] == '\\' || value[i + 2] == '/')) {
      return i;
    }
    if (c == '\\') {
      size_t start = i;
      while (start > 0) {
        const unsigned char previous = static_cast<unsigned char>(value[start - 1]);
        if (std::isspace(previous) || value[start - 1] == '"' || value[start - 1] == '\'') {
          break;
        }
        --start;
      }

      size_t separators = 0;
      for (size_t j = i; j < value.size() && value[j] != '\r' && value[j] != '\n'; ++j) {
        if (value[j] == '\\' && ++separators >= 2) {
          return start;
        }
      }
    }
    if (c == '/') {
      size_t segments = 0;
      size_t j = i;
      while (j < value.size() && value[j] == '/') {
        const size_t segment_start = ++j;
        while (j < value.size() && value[j] != '/' && value[j] != '\r' && value[j] != '\n' &&
               value[j] != ' ' && value[j] != '\t') {
          ++j;
        }
        if (j > segment_start) {
          ++segments;
        } else {
          break;
        }
      }
      if (segments >= 2) {
        size_t start = i;
        while (start > 0) {
          const unsigned char previous = static_cast<unsigned char>(value[start - 1]);
          if (std::isspace(previous) || value[start - 1] == '"' || value[start - 1] == '\'') {
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

inline void TruncateUtf8AtBoundary(std::string& value, size_t max_length) {
  if (value.size() <= max_length) {
    return;
  }

  size_t end = max_length;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) {
    --end;
  }
  value.resize(end);
}

}  // namespace telemetry_detail

inline std::string ScrubStringForTelemetry(std::string_view message) {
  const size_t anchor = telemetry_detail::FindPathAnchor(message);
  std::string result;
  if (anchor == std::string_view::npos) {
    result.assign(message);
  } else {
    result.assign(message.substr(0, anchor));
    result += "[path]";
  }
  if (result.size() > kMaxTelemetryStringLength) {
    telemetry_detail::TruncateUtf8AtBoundary(result, kMaxTelemetryStringLength);
  }
  return result;
}

}  // namespace fl
