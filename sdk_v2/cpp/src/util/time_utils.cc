// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/time_utils.h"

#include <iomanip>
#include <sstream>

namespace fl {

std::string FormatUtcTimestamp(std::time_t unix_time) {
  std::tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &unix_time) != 0) {
    return {};
  }
#else
  if (!gmtime_r(&unix_time, &utc)) {
    return {};
  }
#endif

  std::ostringstream stream;
  stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

}  // namespace fl