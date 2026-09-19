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

struct RedactionAnchor {
  size_t position;
  std::string_view replacement;
};

inline bool IsAsciiAlpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

inline unsigned char AsciiToLower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

inline bool AsciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (size_t i = 0; i < lhs.size(); ++i) {
    if (AsciiToLower(static_cast<unsigned char>(lhs[i])) !=
        AsciiToLower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }

  return true;
}

inline bool AsciiEndsWithIgnoreCase(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         AsciiEqualsIgnoreCase(value.substr(value.size() - suffix.size()), suffix);
}

inline bool IsAnchorDelimiter(char c) {
  const auto value = static_cast<unsigned char>(c);
  if (std::isspace(value) != 0) {
    return true;
  }

  switch (c) {
    case '/':
    case '\\':
    case '.':
    case '-':
    case '_':
    case '+':
    case '@':
    case '?':
    case '#':
    case '~':
    case ':':
      return false;
    default:
      return std::ispunct(value) != 0;
  }
}

inline bool StartsSegment(std::string_view value, size_t index) {
  return index < value.size() && value[index] != '/' && value[index] != '\\' &&
         !IsAnchorDelimiter(value[index]);
}

inline bool StartsExplicitRelativePath(std::string_view value, size_t index) {
  if (index + 2 < value.size() && value[index] == '.' &&
      (value[index + 1] == '/' || value[index + 1] == '\\')) {
    return StartsSegment(value, index + 2);
  }

  return index + 3 < value.size() && value[index] == '.' && value[index + 1] == '.' &&
         (value[index + 2] == '/' || value[index + 2] == '\\') && StartsSegment(value, index + 3);
}

inline bool IsUrlSchemeChar(char c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '+' || c == '-' || c == '.';
}

inline size_t FindSchemeUrlAnchor(std::string_view value) {
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '/' && i + 2 < value.size() && value[i + 1] == '/' &&
        (i == 0 || IsAnchorDelimiter(value[i - 1])) && StartsSegment(value, i + 2)) {
      return i;
    }

    if (!IsAsciiAlpha(value[i]) || (i > 0 && IsUrlSchemeChar(value[i - 1]))) {
      continue;
    }

    size_t end = i + 1;
    while (end < value.size() && IsUrlSchemeChar(value[end])) {
      ++end;
    }
    if (end + 2 < value.size() && value[end] == ':' && value[end + 1] == '/' &&
        value[end + 2] == '/') {
      return i;
    }
  }

  return std::string_view::npos;
}

inline size_t FindSchemePathAnchor(std::string_view value) {
  for (size_t i = 0; i < value.size(); ++i) {
    if (!IsAsciiAlpha(value[i]) ||
        (i > 0 && (IsUrlSchemeChar(value[i - 1]) || value[i - 1] == '_'))) {
      continue;
    }

    size_t end = i + 1;
    while (end < value.size() && IsUrlSchemeChar(value[end])) {
      ++end;
    }
    if (end < value.size() && value[end] == ':' &&
        ((end + 1 < value.size() && (value[end + 1] == '/' || value[end + 1] == '\\')) ||
         StartsExplicitRelativePath(value, end + 1))) {
      return i;
    }
  }

  return std::string_view::npos;
}

inline bool IsSecretKey(std::string_view key) {
  constexpr std::string_view secret_keys[] = {
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
  for (std::string_view candidate : secret_keys) {
    if (AsciiEqualsIgnoreCase(key, candidate)) {
      return true;
    }
  }

  constexpr std::string_view secret_suffixes[] = {
      "access-key",
      "access_key",
      "accesskey",
      "account-key",
      "account_key",
      "accountkey",
      "api-key",
      "api_key",
      "apikey",
      "authorization",
      "connection-string",
      "connection_string",
      "connectionstring",
      "credential",
      "credentials",
      "passwd",
      "password",
      "private-key",
      "private_key",
      "privatekey",
      "pwd",
      "secret",
      "signature",
      "token",
  };
  for (std::string_view suffix : secret_suffixes) {
    if (key.size() > suffix.size() && AsciiEndsWithIgnoreCase(key, suffix)) {
      return true;
    }
  }

  return false;
}

inline bool IsSecretKeyChar(char c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '_' || c == '-' || c == '.';
}

inline bool IsSecretKeyBoundary(char c) {
  const auto value = static_cast<unsigned char>(c);
  return std::isspace(value) != 0 || c == '?' || c == '&' || c == '#' || c == ';' || c == ',' ||
         c == '"' || c == '\'' || c == '(' || c == '[' || c == '{' || c == '-' || c == '/';
}

inline size_t FindSecretValueAnchor(std::string_view value) {
  for (size_t i = 0; i < value.size(); ++i) {
    if (!IsAsciiAlpha(value[i]) || (i > 0 && !IsSecretKeyBoundary(value[i - 1]))) {
      continue;
    }

    size_t key_end = i + 1;
    while (key_end < value.size() && IsSecretKeyChar(value[key_end])) {
      ++key_end;
    }
    if (!IsSecretKey(value.substr(i, key_end - i))) {
      i = key_end - 1;
      continue;
    }

    const bool cli_option = i > 0 && (value[i - 1] == '-' || value[i - 1] == '/');
    size_t separator = key_end;
    if (separator < value.size() && (value[separator] == '"' || value[separator] == '\'')) {
      ++separator;
    }
    const size_t before_whitespace = separator;
    while (separator < value.size() &&
           std::isspace(static_cast<unsigned char>(value[separator])) != 0) {
      ++separator;
    }

    const bool assignment =
        separator < value.size() && (value[separator] == '=' || value[separator] == ':');
    const bool separated_cli_value =
        cli_option && separator > before_whitespace && separator < value.size() &&
        value[separator] != '-';
    if (!assignment && !separated_cli_value) {
      i = key_end - 1;
      continue;
    }

    size_t secret_value = assignment ? separator + 1 : separator;
    while (secret_value < value.size() &&
           std::isspace(static_cast<unsigned char>(value[secret_value])) != 0) {
      ++secret_value;
    }
    if (secret_value < value.size() && value[secret_value] != '&' && value[secret_value] != ';' &&
        value[secret_value] != '\r' && value[secret_value] != '\n') {
      return secret_value;
    }
  }

  return std::string_view::npos;
}

inline bool IsAuthorityTerminator(char c) {
  const auto value = static_cast<unsigned char>(c);
  return std::isspace(value) != 0 || c == '"' || c == '\'' || c == ')' || c == '}' ||
         c == ',' || c == ';' || c == '/' || c == '?' || c == '#';
}

inline bool IsUserInfoTerminator(char c) {
  const auto value = static_cast<unsigned char>(c);
  return std::isspace(value) != 0 || c == '"' || c == '\\' || c == '/' || c == '?' ||
         c == '#' || c == '[' || c == ']' || c == '{' || c == '}';
}

inline size_t FindCredentialUrlAnchor(std::string_view value) {
  size_t token_start = 0;
  size_t colon = std::string_view::npos;
  for (size_t i = 0; i < value.size(); ++i) {
    if (colon == std::string_view::npos) {
      if (IsUserInfoTerminator(value[i])) {
        token_start = i + 1;
      } else if (value[i] == ':' && i > token_start) {
        colon = i;
      }
      continue;
    }

    if (IsUserInfoTerminator(value[i])) {
      token_start = i + 1;
      colon = std::string_view::npos;
      continue;
    }
    if (value[i] != '@' || colon + 1 == i) {
      continue;
    }

    const size_t host_start = i + 1;
    if (host_start == value.size()) {
      continue;
    }

    if (value[host_start] == '[') {
      const size_t host_end = value.find(']', host_start + 1);
      if (host_end != std::string_view::npos && host_end > host_start + 1) {
        return token_start;
      }
    } else {
      size_t host_end = host_start;
      while (host_end < value.size() && !IsAuthorityTerminator(value[host_end])) {
        ++host_end;
      }
      if (host_end > host_start) {
        return token_start;
      }
    }
  }

  return std::string_view::npos;
}

inline bool IsTruncatedTokenBoundary(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '"';
}

inline size_t FindTruncatedSensitiveTokenAnchor(std::string_view transmitted_prefix, char next_char) {
  if (transmitted_prefix.empty()) {
    return std::string_view::npos;
  }

  size_t token_start = transmitted_prefix.size();
  while (token_start > 0 && !IsTruncatedTokenBoundary(transmitted_prefix[token_start - 1])) {
    --token_start;
  }
  const std::string_view token = transmitted_prefix.substr(token_start);
  const bool continues_path = !token.empty() && (next_char == '/' || next_char == '\\');
  const bool contains_path_separator = token.find_first_of("/\\") != std::string_view::npos;
  if (continues_path && !contains_path_separator) {
    size_t previous_end = token_start;
    while (previous_end > 0) {
      while (previous_end > 0 && IsTruncatedTokenBoundary(transmitted_prefix[previous_end - 1])) {
        --previous_end;
      }
      size_t previous_start = previous_end;
      while (previous_start > 0 &&
             !IsTruncatedTokenBoundary(transmitted_prefix[previous_start - 1])) {
        --previous_start;
      }
      if (transmitted_prefix.substr(previous_start, previous_end - previous_start)
              .find_first_of("/\\") != std::string_view::npos) {
        return previous_start;
      }
      previous_end = previous_start;
    }
  }

  if (continues_path || (!token.empty() && next_char == ':') ||
      token.find(':') != std::string_view::npos || contains_path_separator) {
    return token_start;
  }

  return std::string_view::npos;
}

inline bool StartsCliSecretOption(std::string_view value, size_t index) {
  if (value[index] != '/' || index + 1 >= value.size()) {
    return false;
  }

  size_t key_end = index + 1;
  while (key_end < value.size() && IsSecretKeyChar(value[key_end])) {
    ++key_end;
  }
  if (!IsSecretKey(value.substr(index + 1, key_end - index - 1)) || key_end == value.size()) {
    return false;
  }
  if (value[key_end] == '=' || value[key_end] == ':') {
    return true;
  }
  if (std::isspace(static_cast<unsigned char>(value[key_end])) == 0) {
    return false;
  }

  while (key_end < value.size() && std::isspace(static_cast<unsigned char>(value[key_end])) != 0) {
    ++key_end;
  }
  return key_end < value.size();
}

inline bool StartsNamedValuePath(std::string_view value, size_t index) {
  if (index == 0 || value[index - 1] != ':') {
    return false;
  }

  size_t key_start = index - 1;
  while (key_start > 0 && IsSecretKeyChar(value[key_start - 1])) {
    --key_start;
  }
  const size_t key_length = index - 1 - key_start;
  if (key_length == 0 || (key_length == 1 && IsAsciiAlpha(value[key_start]))) {
    return false;
  }
  return key_start == 0 || IsAnchorDelimiter(value[key_start - 1]);
}

inline size_t FindPathAnchor(std::string_view value) {
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '\\' && i + 1 < value.size() && value[i + 1] == '\\') {
      return i;
    }
    if (c == '~' && i + 1 < value.size() && (value[i + 1] == '/' || value[i + 1] == '\\')) {
      return i;
    }
    if (IsAsciiAlpha(c) && (i == 0 || IsAnchorDelimiter(value[i - 1])) &&
        i + 2 < value.size() && value[i + 1] == ':' &&
        (value[i + 2] == '\\' || value[i + 2] == '/')) {
      return i;
    }
    if (StartsExplicitRelativePath(value, i) &&
        (i == 0 || IsAnchorDelimiter(value[i - 1]))) {
      return i;
    }
    if (c == '/' || c == '\\') {
      if (c == '/' && StartsCliSecretOption(value, i)) {
        continue;
      }
      if (i == 0 || StartsNamedValuePath(value, i) ||
          (IsAnchorDelimiter(value[i - 1]) && StartsSegment(value, i + 1))) {
        return i;
      }
    }
    if (c == '\\') {
      size_t start = i;
      while (start > 0 && !IsAnchorDelimiter(value[start - 1])) {
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
        if (j == segment_start) {
          break;
        }
        ++segments;
      }
      if (segments >= 2) {
        size_t start = i;
        while (start > 0 && !IsAnchorDelimiter(value[start - 1])) {
          --start;
        }
        return start;
      }
    }
  }

  return std::string_view::npos;
}

inline RedactionAnchor FindRedactionAnchor(std::string_view value) {
  RedactionAnchor result{std::string_view::npos, {}};
  const auto consider = [&result](size_t position, std::string_view replacement) {
    if (position < result.position) {
      result = {position, replacement};
    }
  };

  consider(FindSchemeUrlAnchor(value), "[url]");
  consider(FindSchemePathAnchor(value), "[path]");
  consider(FindCredentialUrlAnchor(value), "[credential]");
  consider(FindPathAnchor(value), "[path]");
  consider(FindSecretValueAnchor(value), "[secret]");
  return result;
}

inline size_t Utf8SafePrefixLength(std::string_view value, size_t max_length) {
  if (value.size() <= max_length) {
    return value.size();
  }

  size_t end = max_length;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) {
    --end;
  }
  return end;
}

inline std::string LimitTelemetryString(std::string_view value) {
  return std::string{value.substr(0, Utf8SafePrefixLength(value, kMaxTelemetryStringLength))};
}

}  // namespace telemetry_detail

inline std::string ScrubStringForTelemetry(std::string_view value) {
  const std::string_view scan = value.substr(0, kMaxTelemetryStringLength);
  auto anchor = telemetry_detail::FindRedactionAnchor(scan);
  if (anchor.position == std::string_view::npos && value.size() > scan.size()) {
    const size_t truncated_anchor =
        telemetry_detail::FindTruncatedSensitiveTokenAnchor(scan, value[scan.size()]);
    if (truncated_anchor != std::string_view::npos) {
      anchor = {truncated_anchor, "[redacted]"};
    }
  }
  if (anchor.position == std::string_view::npos) {
    return telemetry_detail::LimitTelemetryString(value);
  }

  const size_t prefix_limit = kMaxTelemetryStringLength - anchor.replacement.size();
  const size_t prefix_length =
      telemetry_detail::Utf8SafePrefixLength(value.substr(0, anchor.position), prefix_limit);
  std::string output{value.substr(0, prefix_length)};
  output.append(anchor.replacement);
  return output;
}

}  // namespace fl
