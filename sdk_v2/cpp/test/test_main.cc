// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <gtest/gtest.h>
#if __has_include(<ort_genai.h>)
#include <ort_genai.h>
#define FOUNDRY_LOCAL_TEST_HAS_OGA 1
#endif

#include <cstdlib>

int main(int argc, char** argv) {
#ifdef _WIN32
  _putenv_s("ORT_TELEMETRY_DISABLED", "1");
#else
  setenv("ORT_TELEMETRY_DISABLED", "1", 1);
#endif

  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
#ifdef FOUNDRY_LOCAL_TEST_HAS_OGA
  OgaShutdown();
#endif
  return result;
}
