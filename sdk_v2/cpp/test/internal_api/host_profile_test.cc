// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "configuration.h"
#include "exception.h"
#include "hosting/host_profile.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

fl::Configuration Configuration() {
  const auto root = std::filesystem::current_path() / "TestResults" / "host-profile";
  fl::Configuration config;
  config.app_name = "cpp-client";
  config.app_data_dir = (root / "data").string();
  config.model_cache_dir = (root / "models").string();
  config.logs_dir = (root / "logs").string();
  return config;
}

fl::hosting::RuntimeIdentity Runtime() {
  return {"native-build", "win-arm64", 1};
}

TEST(HostProfileTest, SharesAcrossLanguageWrappersWithTheSameEffectiveDirectories) {
  auto cpp = Configuration();
  cpp.additional_options["UserAgent"] = "foundry-cpp/2.2";
  auto python = cpp;
  python.app_name = "python-client";
  python.additional_options["UserAgent"] = "foundry-python/2.3";

  const auto first = fl::hosting::BuildHostProfile(cpp, Runtime());
  const auto second = fl::hosting::BuildHostProfile(python, Runtime());
  EXPECT_EQ(first.key, second.key);
  EXPECT_EQ(first.compatibility, second.compatibility);
  EXPECT_EQ(first.configuration.app_name, cpp.app_name);
  EXPECT_EQ(second.configuration.additional_options.at("UserAgent"), "foundry-python/2.3");
}

TEST(HostProfileTest, ResolvesNativeDefaultsBeforeComparingProfiles) {
  const auto implicit = fl::hosting::BuildHostProfile(Configuration(), Runtime());
  auto explicit_config = implicit.configuration;
  const auto explicit_profile = fl::hosting::BuildHostProfile(explicit_config, Runtime());
  EXPECT_EQ(implicit.key, explicit_profile.key);
  EXPECT_EQ(implicit.configuration.catalog_region, "auto");
  ASSERT_EQ(implicit.configuration.web_service_endpoints.size(), 1);
  EXPECT_EQ(implicit.configuration.web_service_endpoints.front(), "http://127.0.0.1:0");
  ASSERT_EQ(implicit.configuration.catalog_urls.size(), 1);
  EXPECT_EQ(implicit.configuration.catalog_urls.front().second, "");

  explicit_config.app_data_dir =
      (*explicit_config.app_data_dir + std::string(1, std::filesystem::path::preferred_separator) + ".");
  EXPECT_EQ(implicit.key, fl::hosting::BuildHostProfile(explicit_config, Runtime()).key);
}

TEST(HostProfileTest, PreservesDistinctCatalogOverridesAndOrdering) {
  auto config = Configuration();
  config.catalog_urls = {{"https://example.test/models", std::nullopt},
                         {"https://example.test/other", std::string("task=coding")}};
  const auto original = fl::hosting::BuildHostProfile(config, Runtime());
  config.catalog_urls.front().second = "";
  EXPECT_NE(original.key, fl::hosting::BuildHostProfile(config, Runtime()).key);
  config.catalog_urls.front().second = std::nullopt;
  std::swap(config.catalog_urls.front(), config.catalog_urls.back());
  EXPECT_NE(original.key, fl::hosting::BuildHostProfile(config, Runtime()).key);
}

TEST(HostProfileTest, SeparatesRuntimeContractAndExecutionChanges) {
  const auto config = Configuration();
  const auto runtime = Runtime();
  const auto original = fl::hosting::BuildHostProfile(config, runtime);
  auto changed_runtime = runtime;
  changed_runtime.native_build = "other-native-build";
  EXPECT_NE(original.key, fl::hosting::BuildHostProfile(config, changed_runtime).key);
  changed_runtime = runtime;
  ++changed_runtime.ipc_protocol_version;
  EXPECT_NE(original.key, fl::hosting::BuildHostProfile(config, changed_runtime).key);
  changed_runtime = runtime;
  changed_runtime.runtime_identifier = "linux-x64";
  EXPECT_NE(original.key, fl::hosting::BuildHostProfile(config, changed_runtime).key);

  auto changed_config = config;
  changed_config.additional_options["ExecutionProvider"] = "CPU";
  EXPECT_NE(original.key, fl::hosting::BuildHostProfile(changed_config, runtime).key);
  changed_config = config;
  changed_config.model_cache_dir = *config.model_cache_dir + "-other";
  EXPECT_NE(original.key, fl::hosting::BuildHostProfile(changed_config, runtime).key);
}

TEST(HostProfileTest, RejectsMissingOrUnsupportedRuntimeIdentity) {
  const auto config = Configuration();
  auto runtime = Runtime();
  runtime.native_build.clear();
  EXPECT_THROW(fl::hosting::BuildHostProfile(config, runtime), fl::Exception);
  runtime = Runtime();
  runtime.ipc_protocol_version = 0;
  EXPECT_THROW(fl::hosting::BuildHostProfile(config, runtime), fl::Exception);
  runtime = Runtime();
  runtime.runtime_identifier = "unsupported-rid";
  EXPECT_THROW(fl::hosting::BuildHostProfile(config, runtime), fl::Exception);
}

TEST(HostProfileTest, PreservesTelemetryPolicyAndRejectsClientOnlyConfiguration) {
  auto config = Configuration();
  config.disable_nonessential_telemetry = true;
  const auto explicit_policy = fl::hosting::BuildHostProfile(config, Runtime());
  config.disable_nonessential_telemetry = false;
  config.additional_options["DisableNonessentialTelemetry"] = "YES";
  const auto option_policy = fl::hosting::BuildHostProfile(config, Runtime());
  EXPECT_EQ(explicit_policy.key, option_policy.key);
  EXPECT_TRUE(option_policy.compatibility["profile"]["disable_nonessential_telemetry"].get<bool>());

  config.external_service_url = "http://127.0.0.1:1234";
  EXPECT_THROW(fl::hosting::BuildHostProfile(config, Runtime()), fl::Exception);
}

}  // namespace
