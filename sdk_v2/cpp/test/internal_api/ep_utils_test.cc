// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_utils.h"

#ifdef _WIN32
#include "logger.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <string_view>

namespace fl {

namespace {

class NullLogger : public ILogger {
 public:
  void Log(LogLevel /*level*/, std::string_view /*message*/) override {}
};

}  // namespace

TEST(EpUtilsTest, SearchPathOwnerAcceptsExistingDirectoryAndDuplicateAdd) {
  auto directory = test::TempPath::CreateTempDir("fl_ep_search_path_");
  NullLogger logger;
  EpBundleSearchPathOwner owner;

  EXPECT_TRUE(owner.Add(directory.path(), "Test EP", logger));
  EXPECT_TRUE(owner.Add(directory.path(), "Test EP", logger));
}

}  // namespace fl
#endif
