// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Shared helper for tests that need access to the shared test model cache.
// Requires FOUNDRY_TEST_DATA_DIR to be set to an existing model cache path.
//
// This header is intentionally free of internal SDK headers so it can be included by the
// public-API-only sdk_api test target. Path resolution that relies on the production model
// scanner lives in internal_api/test_model_locator.h (internal_api target only).
#pragma once

#include "utils/safe_getenv.h"
#include "utils/string_utils.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace fl::test {

/// Returns true when running under a known CI provider.
/// Mirrors the C# helper: TF_BUILD=True (Azure DevOps) or
/// GITHUB_ACTIONS=true (GitHub Actions), case-insensitive.
inline bool IsRunningInCI() {
  return ToLower(SafeGetEnv("TF_BUILD")) == "true" ||
         ToLower(SafeGetEnv("GITHUB_ACTIONS")) == "true";
}

/// The standard chat model used in tests.
constexpr const char* kTestChatModelAlias = "qwen2.5-0.5b-instruct-generic-cpu-4";

/// The standard audio (whisper) model used in tests.
constexpr const char* kTestAudioModelAlias = "openai-whisper-tiny-generic-cpu-4";

/// Get the path to a file in the test data directory.
/// Uses the compile-time FOUNDRY_LOCAL_TEST_DATA_DIR macro set by CMake.
/// On Android this is the relative path "testdata" (binary runs from the same directory).
/// On desktop this is the absolute path to the build output testdata directory.
inline fs::path GetTestDataPath(const std::string& relative_path) {
#ifdef FOUNDRY_LOCAL_TEST_DATA_DIR
  return fs::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / relative_path;
#else
  // Fallback: assume cwd is the build output directory (CTest sets WORKING_DIRECTORY).
  return fs::path("testdata") / relative_path;
#endif
}

}  // namespace fl::test
