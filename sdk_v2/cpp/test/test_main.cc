// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace {

void MarkTestProcessAsRunningUnitTestsForTelemetry() {
#ifdef _WIN32
  ::SetEnvironmentVariableA("ORT_RUNNING_UNIT_TESTS", "1");
#else
  setenv("ORT_RUNNING_UNIT_TESTS", "1", 1);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  MarkTestProcessAsRunningUnitTestsForTelemetry();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
