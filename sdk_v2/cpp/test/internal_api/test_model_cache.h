// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Shared helper for tests that need access to the shared test model cache.
// Requires FOUNDRY_TEST_DATA_DIR to be set to an existing model cache path.
#pragma once

#include "utils/safe_getenv.h"
#include "utils/string_utils.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace fl::test {

/// Resolve the test model cache directory.
/// FOUNDRY_TEST_DATA_DIR is required.
inline fs::path GetTestModelCacheDir() {
  std::string env_value = SafeGetEnv("FOUNDRY_TEST_DATA_DIR");
  if (env_value.empty()) {
    throw std::runtime_error(
        "FOUNDRY_TEST_DATA_DIR is not set. Set it to a local model cache directory before running tests.");
  }

  fs::path env_path(env_value);
  if (!fs::exists(env_path)) {
    throw std::runtime_error("FOUNDRY_TEST_DATA_DIR does not exist: " + env_path.string());
  }

  fs::path publisher_path = env_path / "Microsoft";
  if (!fs::exists(publisher_path)) {
    throw std::runtime_error("FOUNDRY_TEST_DATA_DIR/Microsoft does not exist: " +
                             publisher_path.string());
  }

  return fs::canonical(publisher_path);
}

/// Returns true when running under a known CI provider.
/// Mirrors the C# helper: TF_BUILD=True (Azure DevOps) or
/// GITHUB_ACTIONS=true (GitHub Actions), case-insensitive.
inline bool IsRunningInCI() {
  return ToLower(SafeGetEnv("TF_BUILD")) == "true" ||
         ToLower(SafeGetEnv("GITHUB_ACTIONS")) == "true";
}

/// Get the effective model path — the directory containing genai_config.json.
/// ModelLoadManager::LoadModel expects this resolved path (it no longer searches
/// subdirectories). Some models use a variant layout where genai_config.json is
/// in a subdirectory of the model root.
/// @param model_alias  e.g. "qwen2.5-0.5b-instruct-generic-cpu-4"
inline fs::path GetTestModelPath(const std::string& model_alias) {
  fs::path cache_dir = GetTestModelCacheDir();
  fs::path model_dir = cache_dir / model_alias;

  if (!fs::exists(model_dir)) {
    throw std::runtime_error("Test model not found: " + model_dir.string());
  }

  // Check for genai_config.json at the root level.
  constexpr const char* kConfigFile = "genai_config.json";

  if (fs::exists(model_dir / kConfigFile)) {
    return model_dir;
  }

  // Search immediate subdirectories (variant layout).
  for (const auto& entry : fs::directory_iterator(model_dir)) {
    if (entry.is_directory() && fs::exists(entry.path() / kConfigFile)) {
      return fs::canonical(entry.path());
    }
  }

  throw std::runtime_error(
      "Test model does not contain genai_config.json (checked root and subdirectories): " +
      model_dir.string());
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
