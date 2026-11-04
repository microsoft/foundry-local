// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace fl {

/// Whether `text` is well-formed UTF-8. Embedded NUL is valid UTF-8 and must be
/// rejected separately by contracts that use NUL-terminated ABI strings.
inline bool IsValidUtf8(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    if (lead <= 0x7F) {
      ++i;
      continue;
    }

    size_t width = 0;
    uint32_t code_point = 0;
    if ((lead & 0xE0) == 0xC0) {
      width = 2;
      code_point = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
      width = 3;
      code_point = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      width = 4;
      code_point = lead & 0x07;
    } else {
      return false;
    }

    if (i + width > text.size()) {
      return false;
    }

    for (size_t j = 1; j < width; ++j) {
      const auto continuation = static_cast<unsigned char>(text[i + j]);
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }

      code_point = (code_point << 6) | (continuation & 0x3F);
    }

    if ((width == 2 && code_point < 0x80) || (width == 3 && code_point < 0x800) ||
        (width == 4 && code_point < 0x10000) || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point <= 0xDFFF)) {
      return false;
    }

    i += width;
  }

  return true;
}

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
