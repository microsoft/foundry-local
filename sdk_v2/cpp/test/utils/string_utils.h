// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tiny string helpers shared across all test targets. Mirrors fl::ToLower
// in src/util/string_utils.h, but lives under test/utils/ because sdk_integration_tests
// deliberately walls itself off from internal SDK headers.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace fl::test {

/// Lowercase a string (ASCII). Returns a new string.
inline std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

}  // namespace fl::test
