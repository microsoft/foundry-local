// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "configuration.h"
#include "exception.h"
#include "utils.h"
#include <gtest/gtest.h>

using namespace fl;

TEST(ConfigurationTest, ValidateRejectsEmptyAppName) {
  Configuration config;
  config.app_name = "";
  EXPECT_THROW(config.Validate(), fl::Exception);
}

TEST(ConfigurationTest, ValidateAcceptsValidConfig) {
  Configuration config;
  config.app_name = "test_app";
  EXPECT_NO_THROW(config.Validate());
}

TEST(ConfigurationTest, DefaultValues) {
  Configuration config;
  EXPECT_EQ(config.log_level, LogLevel::Warning);
  EXPECT_TRUE(config.catalog_urls.empty());
  EXPECT_TRUE(config.web_service_endpoints.empty());
  EXPECT_FALSE(config.app_data_dir.has_value());
  EXPECT_FALSE(config.model_cache_dir.has_value());
  EXPECT_FALSE(config.logs_dir.has_value());
}

TEST(ConfigurationTest, ValidateRejectsEmptyCatalogUrl) {
  Configuration config;
  config.app_name = "test_app";
  config.catalog_urls.push_back(CatalogSource{"public", "", std::string("")});
  EXPECT_THROW(config.Validate(), fl::Exception);
}

TEST(ConfigurationTest, ValidateRejectsEmptyCatalogName) {
  Configuration config;
  config.app_name = "test_app";
  config.catalog_urls.push_back(CatalogSource{"", "https://example.com/catalog", std::string("")});
  EXPECT_THROW(config.Validate(), fl::Exception);
}

TEST(ConfigurationTest, ValidateRejectsReservedCatalogName) {
  Configuration config;
  config.app_name = "test_app";
  config.catalog_urls.push_back(CatalogSource{"public", "https://example.com/catalog", std::string("")});
  EXPECT_THROW(config.Validate(), fl::Exception);
}

TEST(ConfigurationTest, ValidateRejectsDuplicateCatalogNames) {
  Configuration config;
  config.app_name = "test_app";
  config.catalog_urls.push_back(CatalogSource{"custom", "https://example.com/first", std::string("")});
  config.catalog_urls.push_back(CatalogSource{"custom", "https://example.com/second", std::string("")});
  EXPECT_THROW(config.Validate(), fl::Exception);
}

TEST(ConfigurationTest, ValidateRejectsEmptyEndpoint) {
  Configuration config;
  config.app_name = "test_app";
  config.web_service_endpoints.emplace_back("");
  EXPECT_THROW(config.Validate(), fl::Exception);
}

TEST(ConfigurationTest, ValidateAcceptsCatalogUrlsAndEndpoints) {
  Configuration config;
  config.app_name = "test_app";
  config.catalog_urls.push_back(CatalogSource{"custom", "https://example.com/catalog", std::string("")});
  config.web_service_endpoints.emplace_back("http://127.0.0.1:0");
  EXPECT_NO_THROW(config.Validate());
}

TEST(ConfigurationTest, ValidateExpandsHomeAndAppNamePlaceholdersInAppDataDir) {
  Configuration config;
  config.app_name = "test_app";
  config.app_data_dir = "{home}/apps/{appname}";

  ASSERT_NO_THROW(config.Validate());
  ASSERT_TRUE(config.app_data_dir.has_value());

  EXPECT_EQ(*config.app_data_dir, Utils::GetHomeDir() + "/apps/test_app");
}

TEST(ConfigurationTest, ValidateExpandsAppDataPlaceholderInLogsAndCacheDirs) {
  Configuration config;
  config.app_name = "test_app";
  config.app_data_dir = "{home}/root/{appname}";
  config.logs_dir = "{appdata}/logs";
  config.model_cache_dir = "{appdata}/cache/models/{appname}";

  ASSERT_NO_THROW(config.Validate());
  ASSERT_TRUE(config.app_data_dir.has_value());
  ASSERT_TRUE(config.logs_dir.has_value());
  ASSERT_TRUE(config.model_cache_dir.has_value());

  const std::string expected_app_data = Utils::GetHomeDir() + "/root/test_app";

  EXPECT_EQ(*config.app_data_dir, expected_app_data);
  EXPECT_EQ(*config.logs_dir, expected_app_data + "/logs");
  EXPECT_EQ(*config.model_cache_dir, expected_app_data + "/cache/models/test_app");
}

// Regression: when app_data_dir is an explicit path (no {home} placeholder), Validate() must not
// call GetHomeDir(). This is critical on Android where $HOME may be unset (adb + emulator for tests). The default
// logs_dir and model_cache_dir should derive from the explicit app_data_dir.
TEST(ConfigurationTest, ValidateDefaultsLogAndCacheDirsFromExplicitAppDataDir) {
  Configuration config;
  config.app_name = "test_app";
  config.app_data_dir = "/data/local/tmp/my-app";

  ASSERT_NO_THROW(config.Validate());

  ASSERT_TRUE(config.app_data_dir.has_value());
  ASSERT_TRUE(config.logs_dir.has_value());
  ASSERT_TRUE(config.model_cache_dir.has_value());

  EXPECT_EQ(*config.app_data_dir, "/data/local/tmp/my-app");
  EXPECT_EQ(*config.logs_dir, "/data/local/tmp/my-app/logs");
  EXPECT_EQ(*config.model_cache_dir, "/data/local/tmp/my-app/cache/models");
}

// Verify that {appname} works in app_data_dir without requiring {home} — another case where
// GetHomeDir() should never be called.
TEST(ConfigurationTest, ValidateExpandsAppNameInAppDataDirWithoutHome) {
  Configuration config;
  config.app_name = "test_app";
  config.app_data_dir = "/opt/{appname}/data";

  ASSERT_NO_THROW(config.Validate());

  ASSERT_TRUE(config.app_data_dir.has_value());
  ASSERT_TRUE(config.logs_dir.has_value());
  ASSERT_TRUE(config.model_cache_dir.has_value());

  EXPECT_EQ(*config.app_data_dir, "/opt/test_app/data");
  EXPECT_EQ(*config.logs_dir, "/opt/test_app/data/logs");
  EXPECT_EQ(*config.model_cache_dir, "/opt/test_app/data/cache/models");
}
