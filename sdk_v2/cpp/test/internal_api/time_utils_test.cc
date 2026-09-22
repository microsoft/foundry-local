// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/time_utils.h"

#include <gtest/gtest.h>

#include <ctime>

namespace fl {
namespace {

TEST(TimeUtilsTest, FormatsUnixEpochInUtc) {
  EXPECT_EQ(FormatUtcTimestamp(std::time_t{0}), "1970-01-01T00:00:00Z");
}

TEST(TimeUtilsTest, FormatsNonzeroTimestampInUtc) {
  EXPECT_EQ(FormatUtcTimestamp(static_cast<std::time_t>(946684800)), "2000-01-01T00:00:00Z");
}

}  // namespace
}  // namespace fl