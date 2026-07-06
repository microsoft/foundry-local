// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace fl {

template <typename... Args>
std::string MakeString(Args&&... args) {
  std::ostringstream out;
  (out << ... << std::forward<Args>(args));
  return out.str();
}

/// Lowercase a string (ASCII). Returns a new string.
inline std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

/// Strip leading/trailing ASCII whitespace (space, tab, CR, LF). Returns a new string.
inline std::string Trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }

  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

/// Case-insensitive (ASCII) suffix test.
inline bool EndsWithIgnoreCase(const std::string& str, const std::string& suffix) {
  if (suffix.size() > str.size()) {
    return false;
  }

  return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
                    [](char a, char b) {
                      return std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b));
                    });
}

/// Case-insensitive (ASCII) string equality.
inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](char x, char y) {
                      return std::tolower(static_cast<unsigned char>(x)) ==
                             std::tolower(static_cast<unsigned char>(y));
                    });
}

}  // namespace fl
