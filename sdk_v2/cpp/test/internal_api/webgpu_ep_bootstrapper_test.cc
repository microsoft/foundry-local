// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/webgpu_ep_bootstrapper.h"

#include "logger.h"
#include "utils/scoped_environment_variable.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fl {

namespace {

constexpr const char* kOverrideEnv = "FOUNDRY_LOCAL_WEBGPU_EP_LIBRARY";
constexpr const char* kScopedEnvironmentVariableTestEnv = "FOUNDRY_LOCAL_SCOPED_ENVIRONMENT_VARIABLE_TEST";

}  // namespace

TEST(WebGpuEpBootstrapperTest, PlatformSupportMatchesPublishedBundles) {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || (defined(__APPLE__) && defined(__aarch64__))
  EXPECT_TRUE(WebGpuEpBootstrapper::IsSupportedPlatform());
#else
  EXPECT_FALSE(WebGpuEpBootstrapper::IsSupportedPlatform());
#endif
}

TEST(WebGpuEpBootstrapperTest, ScopedEnvironmentVariableRestoresExistingValue) {
  test::ScopedEnvironmentVariable restore_original(kScopedEnvironmentVariableTestEnv, "before");

  {
    test::ScopedEnvironmentVariable environment(kScopedEnvironmentVariableTestEnv, "during");
    EXPECT_EQ(test::GetEnvironmentVariable(kScopedEnvironmentVariableTestEnv), "during");
  }

  EXPECT_EQ(test::GetEnvironmentVariable(kScopedEnvironmentVariableTestEnv), "before");
}

TEST(WebGpuEpBootstrapperTest, ScopedEnvironmentVariableRestoresUnsetValue) {
  test::ScopedEnvironmentVariable restore_original(kScopedEnvironmentVariableTestEnv, "before");
  test::SetEnvironmentVariable(kScopedEnvironmentVariableTestEnv, std::nullopt);

  {
    test::ScopedEnvironmentVariable environment(kScopedEnvironmentVariableTestEnv, "during");
    EXPECT_EQ(test::GetEnvironmentVariable(kScopedEnvironmentVariableTestEnv), "during");
  }

  EXPECT_EQ(test::GetEnvironmentVariable(kScopedEnvironmentVariableTestEnv), std::nullopt);
}

TEST(WebGpuEpBootstrapperTest, OverrideRegistersUsingExistingProviderConvention) {
  auto root = test::TempPath::CreateTempDir("fl_webgpu_bootstrapper_");
  const auto provider_path = root.path() / "custom_webgpu_provider";
  std::ofstream(provider_path, std::ios::binary) << "test provider";
  test::ScopedEnvironmentVariable override(kOverrideEnv, provider_path.string());

  std::string registered_name;
  std::filesystem::path registered_path;
  int registration_count = 0;
  auto register_ep = [&](const std::string& name, const std::filesystem::path& path) {
    registered_name = name;
    registered_path = path;
    ++registration_count;
    return true;
  };
  WebGpuEpBootstrapper bootstrapper(root.string(), register_ep);
  StderrLogger logger;
  std::vector<std::pair<std::string, float>> progress;

  EXPECT_TRUE(bootstrapper.DownloadAndRegister(
      false,
      [&](const std::string& name, float percent) {
        progress.emplace_back(name, percent);
        return true;
      },
      logger));

  EXPECT_TRUE(bootstrapper.IsRegistered());
  EXPECT_EQ(registered_name, "Foundry.WebGPU");
  EXPECT_EQ(registered_path, std::filesystem::absolute(provider_path));
  EXPECT_EQ(registration_count, 1);
  ASSERT_EQ(progress.size(), 2u);
  EXPECT_EQ(progress[0], std::make_pair(std::string("WebGpuExecutionProvider"), 90.0f));
  EXPECT_EQ(progress[1], std::make_pair(std::string("WebGpuExecutionProvider"), 100.0f));

  progress.clear();
  EXPECT_TRUE(bootstrapper.DownloadAndRegister(
      false,
      [&](const std::string& name, float percent) {
        progress.emplace_back(name, percent);
        return true;
      },
      logger));

  EXPECT_EQ(registration_count, 1);
  ASSERT_EQ(progress.size(), 1u);
  EXPECT_EQ(progress[0], std::make_pair(std::string("WebGpuExecutionProvider"), 100.0f));
}

TEST(WebGpuEpBootstrapperTest, OverrideCancellationBeforeRegistrationReturnsFalse) {
  auto root = test::TempPath::CreateTempDir("fl_webgpu_bootstrapper_");
  const auto provider_path = root.path() / "custom_webgpu_provider";
  std::ofstream(provider_path, std::ios::binary) << "test provider";
  test::ScopedEnvironmentVariable override(kOverrideEnv, provider_path.string());

  int registration_count = 0;
  WebGpuEpBootstrapper bootstrapper(root.string(), [&](const std::string&, const std::filesystem::path&) {
    ++registration_count;
    return true;
  });
  StderrLogger logger;

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(
      false, [](const std::string&, float percent) { return percent != 90.0f; }, logger));
  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_EQ(registration_count, 0);
}

}  // namespace fl
