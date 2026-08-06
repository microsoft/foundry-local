// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <gtest/gtest.h>

#include <cstdlib>

int main(int argc, char** argv) {
#ifdef _WIN32
  _putenv_s("ORT_TELEMETRY_DISABLED", "1");
#else
  setenv("ORT_TELEMETRY_DISABLED", "1", 1);
#endif

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
