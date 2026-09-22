// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <ctime>
#include <string>

namespace fl {

/// Formats a Unix timestamp as an ISO 8601 UTC timestamp. Returns an empty string if conversion fails.
std::string FormatUtcTimestamp(std::time_t unix_time);

}  // namespace fl