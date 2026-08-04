// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/webgpu_ep_bootstrapper.h"

#include "logger.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fl {

namespace {

constexpr const char* kOverrideEnv = "FOUNDRY_LOCAL_WEBGPU_EP_LIBRARY";
constexpr const char* kScopedEnvironmentVariableTestEnv = "FOUNDRY_LOCAL_SCOPED_ENVIRONMENT_VARIABLE_TEST";

std::optional<std::string> GetEnvValue(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  size_t length = 0;
  const auto error = _dupenv_s(&value, &length, name);
  const std::unique_ptr<char, decltype(&std::free)> buffer(value, &std::free);
  if (error != 0) {
    throw std::system_error(error, std::generic_category(), "_dupenv_s failed");
  }

  if (buffer == nullptr) {
    return std::nullopt;
  }

  return std::string(buffer.get());
#else
  const auto* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }

  return std::string(value);
#endif
}

void SetEnvValue(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
  _putenv_s(name, value.value_or("").c_str());
#else
  if (value.has_value()) {
    setenv(name, value->c_str(), 1);
  } else {
    unsetenv(name);
  }
#endif
}

class ScopedEnvironmentVariable {
 public:
  ScopedEnvironmentVariable(const char* name, std::string value)
      : name_(name),
        previous_(GetEnvValue(name)) {
    SetEnvValue(name_, value);
  }

  ~ScopedEnvironmentVariable() {
    SetEnvValue(name_, previous_);
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

 private:
  const char* name_;
  std::optional<std::string> previous_;
};

}  // namespace

TEST(WebGpuEpBootstrapperTest, PlatformSupportMatchesPublishedBundles) {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || \
    (defined(__APPLE__) && defined(__aarch64__))
  EXPECT_TRUE(WebGpuEpBootstrapper::IsSupportedPlatform());
#else
  EXPECT_FALSE(WebGpuEpBootstrapper::IsSupportedPlatform());
#endif
}

TEST(WebGpuEpBootstrapperTest, ScopedEnvironmentVariableRestoresExistingValue) {
  ScopedEnvironmentVariable restore_original(kScopedEnvironmentVariableTestEnv, "before");

  {
    ScopedEnvironmentVariable environment(kScopedEnvironmentVariableTestEnv, "during");
    EXPECT_EQ(GetEnvValue(kScopedEnvironmentVariableTestEnv), "during");
  }

  EXPECT_EQ(GetEnvValue(kScopedEnvironmentVariableTestEnv), "before");
}

TEST(WebGpuEpBootstrapperTest, ScopedEnvironmentVariableRestoresUnsetValue) {
  ScopedEnvironmentVariable restore_original(kScopedEnvironmentVariableTestEnv, "before");
  SetEnvValue(kScopedEnvironmentVariableTestEnv, std::nullopt);

  {
    ScopedEnvironmentVariable environment(kScopedEnvironmentVariableTestEnv, "during");
    EXPECT_EQ(GetEnvValue(kScopedEnvironmentVariableTestEnv), "during");
  }

  EXPECT_EQ(GetEnvValue(kScopedEnvironmentVariableTestEnv), std::nullopt);
}

TEST(WebGpuEpBootstrapperTest, OverrideRegistersUsingExistingProviderConvention) {
  auto root = test::TempPath::CreateTempDir("fl_webgpu_bootstrapper_");
  const auto provider_path = root.path() / "custom_webgpu_provider";
  std::ofstream(provider_path, std::ios::binary) << "test provider";
  ScopedEnvironmentVariable override(kOverrideEnv, provider_path.string());

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

}  // namespace fl
