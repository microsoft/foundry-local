// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
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

/// Case-insensitive (ASCII) three-way comparison.
/// Returns: < 0 if lhs < rhs, 0 if equal, > 0 if lhs > rhs (all case-insensitive).
inline int CompareCaseInsensitive(const std::string& lhs, const std::string& rhs) {
  const size_t common = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < common; ++i) {
    const auto l = static_cast<unsigned char>(lhs[i]);
    const auto r = static_cast<unsigned char>(rhs[i]);
    const char l_lower = static_cast<char>(std::tolower(l));
    const char r_lower = static_cast<char>(std::tolower(r));
    if (l_lower < r_lower) {
      return -1;
    }
    if (l_lower > r_lower) {
      return 1;
    }
  }

  if (lhs.size() < rhs.size()) {
    return -1;
  }
  if (lhs.size() > rhs.size()) {
    return 1;
  }
  return 0;
}

}  // namespace fl
