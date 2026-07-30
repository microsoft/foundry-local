// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "telemetry_test_environment.h"

#include <gtest/gtest.h>

int main(int argc, char** argv) {
  fl::test::SuppressTelemetryForTests();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
