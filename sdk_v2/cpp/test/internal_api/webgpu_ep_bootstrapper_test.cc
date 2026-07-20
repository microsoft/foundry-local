// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Unit tests for WebGpuEpBootstrapper.
// These tests cover behavior that is safe to exercise without network access:
//   - Name and initial registration state
//   - DownloadAndRegister returns false when the override env points to a
//     non-existent file (early-return path before any I/O).
//
// The linux-arm64 "no WebGPU payload" path relies on the compile-time
// kPlatformKey being "linux-arm64", which causes GetPackageMetadata() to
// throw at the start of DownloadAndRegister — caught and returned as false.
// That path can only be exercised on an ARM64 Linux host; the contract is
// documented by the header comment and the static preprocessor block.
//
#include "ep_detection/webgpu_ep_bootstrapper.h"
#include "logger.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

using namespace fl;

namespace {

// RAII helper that sets an environment variable and restores it on destruction.
struct ScopedEnv {
  ScopedEnv(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
    char* prev = nullptr;
    size_t len = 0;
    if (_dupenv_s(&prev, &len, name) == 0 && prev != nullptr) {
      prev_ = prev;
      free(prev);
    }
    _putenv_s(name, value);
#else
    const char* prev = std::getenv(name);
    if (prev) {
      prev_ = prev;
    }
    ::setenv(name, value, /*overwrite=*/1);
#endif
  }

  ~ScopedEnv() {
#ifdef _WIN32
    _putenv_s(name_.c_str(), prev_.value_or("").c_str());
#else
    if (prev_.has_value()) {
      ::setenv(name_.c_str(), prev_->c_str(), /*overwrite=*/1);
    } else {
      ::unsetenv(name_.c_str());
    }
#endif
  }

  std::string name_;
  std::optional<std::string> prev_;
};

// A no-op registration callback — never actually reached in these tests.
EpRegistrationCallback MakeNoopCallback() {
  return [](const std::string& /*name*/, const std::filesystem::path& /*path*/) -> bool {
    return false;
  };
}

}  // namespace

// ========================================================================
// WebGpuEpBootstrapper — contract tests (no network I/O)
// ========================================================================

TEST(WebGpuEpBootstrapperTest, Name_IsWebGpuExecutionProvider) {
  WebGpuEpBootstrapper bootstrapper("/tmp/foundry-ep-test", MakeNoopCallback());
  EXPECT_EQ(bootstrapper.Name(), "WebGpuExecutionProvider");
}

TEST(WebGpuEpBootstrapperTest, IsRegistered_InitiallyFalse) {
  WebGpuEpBootstrapper bootstrapper("/tmp/foundry-ep-test", MakeNoopCallback());
  EXPECT_FALSE(bootstrapper.IsRegistered());
}

// When FOUNDRY_LOCAL_WEBGPU_EP_LIBRARY points to a non-existent file the
// bootstrapper returns false at the override-path check — before any download
// attempt or platform-specific metadata lookup.
TEST(WebGpuEpBootstrapperTest, DownloadAndRegister_ReturnsFalse_WhenOverrideFileIsMissing) {
  StderrLogger logger;

  ScopedEnv env("FOUNDRY_LOCAL_WEBGPU_EP_LIBRARY", "/nonexistent/path/to/webgpu-ep.so");

  WebGpuEpBootstrapper bootstrapper("/tmp/foundry-ep-test", MakeNoopCallback());
  bool result = bootstrapper.DownloadAndRegister(/*force=*/false, nullptr, logger);

  EXPECT_FALSE(result);
  EXPECT_FALSE(bootstrapper.IsRegistered());
}

// A second call after a failed attempt also returns false and leaves the
// bootstrapper unregistered.
TEST(WebGpuEpBootstrapperTest, DownloadAndRegister_Unregistered_AfterRepeatedFailure) {
  StderrLogger logger;

  ScopedEnv env("FOUNDRY_LOCAL_WEBGPU_EP_LIBRARY", "/nonexistent/path/to/webgpu-ep.so");

  WebGpuEpBootstrapper bootstrapper("/tmp/foundry-ep-test", MakeNoopCallback());

  bootstrapper.DownloadAndRegister(/*force=*/false, nullptr, logger);
  bootstrapper.DownloadAndRegister(/*force=*/false, nullptr, logger);

  EXPECT_FALSE(bootstrapper.IsRegistered());
}

