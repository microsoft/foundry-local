// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace fl {

inline constexpr size_t kMaxTelemetryStringLength = 1024;
inline constexpr size_t kMaxTelemetryInspectionLength = 2 * kMaxTelemetryStringLength;
inline constexpr size_t kMaxTelemetryProbeLength = 16 * 1024;
inline constexpr size_t kMaxTelemetryEnvironmentLength = 32 * 1024;

// Never use an unbounded strlen for externally supplied exception or platform strings.
inline std::string_view BoundedTelemetryCString(const char* value,
                                                size_t limit = kMaxTelemetryInspectionLength + 1) {
  if (value == nullptr) {
    return {};
  }

  size_t length = 0;
  while (length < limit && value[length] != '\0') {
    ++length;
  }
  return {value, length};
}

inline std::string_view TelemetryIdentifierView(std::string_view value) {
  // Reject only the diagnostic projection; never manufacture a truncated functional identifier.
  return value.size() > kMaxTelemetryStringLength ? std::string_view{"[oversized]"} : value;
}

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

inline std::string CopyUtf8Prefix(std::string_view value, size_t max_length) {
  std::string result;
  result.reserve((std::min)(value.size(), max_length));
  size_t offset = 0;
  while (offset < value.size() && result.size() < max_length) {
    const auto lead = static_cast<unsigned char>(value[offset]);
    const size_t width = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2
                                       : lead >= 0xE0 && lead <= 0xEF   ? 3
                                       : lead >= 0xF0 && lead <= 0xF4   ? 4
                                                                        : 0;
    bool valid = width != 0 && width <= value.size() - offset;
    for (size_t i = 1; valid && i < width; ++i) {
      const auto byte = static_cast<unsigned char>(value[offset + i]);
      valid = (byte & 0xC0) == 0x80;
      if (i == 1) {
        valid = valid && !(lead == 0xE0 && byte < 0xA0) && !(lead == 0xED && byte >= 0xA0) &&
                !(lead == 0xF0 && byte < 0x90) && !(lead == 0xF4 && byte >= 0x90);
      }
    }

    if (!valid) {
      result += '?';
      ++offset;
    } else {
      if (width > max_length - result.size()) {
        break;
      }
      result.append(value.data() + offset, width);
      offset += width;
    }
  }
  return result;
}

}  // namespace telemetry_detail

inline std::string ScrubStringForTelemetry(std::string_view message) {
  // A distant separator can turn an apparently harmless prefix into a private path. Fail closed rather than scan
  // arbitrary input or leak a partial path/credential after truncation. The lookahead preserves boundary redaction.
  if (message.size() > kMaxTelemetryInspectionLength) {
    return "[oversized]";
  }

  const size_t anchor = telemetry_detail::FindPathAnchor(message);
  std::string result;
  if (anchor == std::string_view::npos) {
    return telemetry_detail::CopyUtf8Prefix(message, kMaxTelemetryStringLength);
  } else {
    result = telemetry_detail::CopyUtf8Prefix(message.substr(0, anchor), kMaxTelemetryStringLength);
    result += "[path]";
  }
  if (result.size() > kMaxTelemetryStringLength) {
    telemetry_detail::TruncateUtf8AtBoundary(result, kMaxTelemetryStringLength);
  }
  return result;
}

namespace TelemetryInternal {

namespace detail {

inline bool IsSecretProperty(std::string_view name) {
  if (name.size() > kMaxTelemetryStringLength) {
    return true;
  }

  std::string normalized(name);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

  constexpr std::string_view secret_names[] = {
      "access-key",
      "access_key",
      "accesskey",
      "access-token",
      "access_token",
      "account-key",
      "account_key",
      "accountkey",
      "api-key",
      "api_key",
      "apikey",
      "auth",
      "authorization",
      "client-secret",
      "client_secret",
      "connection-string",
      "connection_string",
      "connectionstring",
      "credential",
      "credentials",
      "password",
      "passwd",
      "private-key",
      "private_key",
      "privatekey",
      "pwd",
      "secret",
      "sig",
      "signature",
      "token",
  };
  for (const auto secret_name : secret_names) {
    if (normalized == secret_name ||
        (normalized.size() > secret_name.size() && normalized.ends_with(secret_name))) {
      return true;
    }
  }
  return false;
}

inline size_t FindUrlAnchor(std::string_view value) {
  size_t separator = value.find("://");
  while (separator != std::string_view::npos) {
    size_t start = separator;
    while (start > 0) {
      const unsigned char character = static_cast<unsigned char>(value[start - 1]);
      if (!std::isalnum(character) && value[start - 1] != '+' && value[start - 1] != '-' &&
          value[start - 1] != '.') {
        break;
      }
      --start;
    }
    if (start < separator && std::isalpha(static_cast<unsigned char>(value[start]))) {
      return start;
    }
    separator = value.find("://", separator + 3);
  }
  return std::string_view::npos;
}

inline size_t FindAbsolutePathAnchor(std::string_view value) {
  for (size_t i = 0; i + 1 < value.size(); ++i) {
    if (value[i] != '/' || value[i + 1] == '/' ||
        std::isspace(static_cast<unsigned char>(value[i + 1]))) {
      continue;
    }

    if (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1])) ||
        value[i - 1] == ':' || value[i - 1] == '=' || value[i - 1] == '"' ||
        value[i - 1] == '\'' || value[i - 1] == '(') {
      return i;
    }
  }
  return std::string_view::npos;
}

inline std::string SanitizeString(std::string_view value, std::string_view suffix = {}) {
  if (value.size() > kMaxTelemetryInspectionLength) {
    return "[oversized]";
  }

  const size_t url_anchor = FindUrlAnchor(value);
  const size_t path_anchor = FindAbsolutePathAnchor(value);
  // Redact URL/absolute-path suffixes before looking for drive-letter and relative-path forms, so metadata keys
  // such as "cache_dir:/private" are not mistaken for the drive-letter path "r:/private".
  const auto primary_anchor = (std::min)(url_anchor, path_anchor);
  const size_t relative_path_anchor = telemetry_detail::FindPathAnchor(value.substr(0, primary_anchor));
  const size_t anchor = (std::min)({url_anchor, path_anchor, relative_path_anchor});
  auto result = telemetry_detail::CopyUtf8Prefix(value.substr(0, anchor), kMaxTelemetryStringLength);
  if (anchor != std::string_view::npos) {
    result += anchor == url_anchor ? "[url]" : "[path]";
  } else {
    result += suffix;
  }
  telemetry_detail::TruncateUtf8AtBoundary(result, kMaxTelemetryStringLength);
  return result;
}

inline std::string SanitizeMetadataValue(std::string_view value) {
  if (value.size() > kMaxTelemetryInspectionLength) {
    return "[oversized]";
  }

  for (size_t separator = value.find_first_of(":="); separator != std::string_view::npos;
       separator = value.find_first_of(":=", separator + 1)) {
    size_t end = separator;
    while (end > 0 && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
      --end;
    }
    size_t start = end;
    while (start > 0) {
      const unsigned char character = static_cast<unsigned char>(value[start - 1]);
      if (!std::isalnum(character) && character != '_' && character != '-' && character != '.') {
        break;
      }
      --start;
    }

    if (start < end && IsSecretProperty(value.substr(start, end - start))) {
      return SanitizeString(value.substr(0, separator + 1), "[secret]");
    }
  }
  return SanitizeString(value);
}

}  // namespace detail

inline std::string SanitizeTelemetryValue(std::string_view value) {
  return detail::SanitizeMetadataValue(value);
}

inline std::string SanitizeTelemetryCatalogFormat(std::string_view value) {
  return value == "asset-gallery/v1.0/models" ? std::string(value) : SanitizeTelemetryValue(value);
}

inline std::string SanitizeTelemetryIdentifier(std::string_view value) {
  return SanitizeTelemetryValue(TelemetryIdentifierView(value));
}

}  // namespace TelemetryInternal

}  // namespace fl
