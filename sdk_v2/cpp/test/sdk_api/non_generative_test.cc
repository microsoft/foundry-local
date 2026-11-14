// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "foundry_local/foundry_local_cpp.h"
#include "utils/safe_getenv.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

TEST(NonGenerativeSdkTest, RejectsEmbeddedNullBeforeNativeDispatch) {
  const std::string request{"{}\0trailing", 11};
  EXPECT_THROW(
      foundry_local::detail::ValidateNonGenerativeJsonInput(request),
      std::invalid_argument);
}

TEST(NonGenerativeSdkTest, ExportedPackagesRunWhenConfigured) {
  const auto root_value =
      fl::test::SafeGetEnv("FOUNDRY_LOCAL_NON_GENERATIVE_TEST_ROOT");
  if (root_value.empty()) {
    if (fl::test::SafeGetEnv(
            "FOUNDRY_LOCAL_REQUIRE_NON_GENERATIVE_TESTS") == "1")
      FAIL() << "required non-generative package root is not configured";
    GTEST_SKIP() << "set FOUNDRY_LOCAL_NON_GENERATIVE_TEST_ROOT";
  }

  const auto root = std::filesystem::path(root_value);
  const auto clm = root / "clm-v0.1-8b-fp32";
  const auto kev = root / "kev-4b-fp32";
  const auto app_data =
      std::filesystem::temp_directory_path() /
      ("fl_non_generative_sdk_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(app_data);

  {
    foundry_local::Configuration config("non-generative-sdk-test");
    config.SetAppDataDir(app_data.string());
    foundry_local::Manager manager(std::move(config));
    auto& catalog =
        manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_LOCAL);

    foundry_local::ModelInfo clm_metadata;
    clm_metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR,
                                   "ranking");
    auto clm_model = catalog.RegisterModel(
        clm.string(), "clm-v0.1-8b-generic-cpu:1", clm_metadata);
    clm_model->Load();
    EXPECT_TRUE(clm_model->IsLoaded());
    EXPECT_THROW(
        {
          foundry_local::DecisionSession wrong_task(*clm_model);
        },
        std::invalid_argument);
    {
      foundry_local::RankingSession ranking(*clm_model);
      foundry_local::RequestOptions defaults;
      defaults.search.temperature = 0.5f;
      ranking.SetOptions(defaults);
      const auto ranked = nlohmann::json::parse(ranking.RankJson(
          nlohmann::json{
              {"context", {{"weather", "heavy rain"}}},
              {"question", "Which activity is more suitable?"},
              {"answers",
               {"Have a picnic outdoors", "Visit an indoor museum"}}}
              .dump()));
      ASSERT_EQ(ranked.at("ranked").size(), 2u);
      for (const auto& candidate : ranked.at("ranked")) {
        EXPECT_EQ(candidate.at("id").get<std::string>(),
                  std::to_string(candidate.at("index").get<size_t>()));
        EXPECT_LT(candidate.at("index").get<size_t>(), 2u);
      }
      EXPECT_THROW(clm_model->Unload(), foundry_local::Error);
      try {
        const std::string answer{"a\0private", 9};
        ranking.RankJson(
            nlohmann::json{{"answers", {"valid", answer}}}.dump());
        FAIL() << "embedded NUL ranking answer was accepted";
      } catch (const foundry_local::Error& error) {
        EXPECT_EQ(error.Code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
      }

      foundry_local::RequestOptions request_options;
      request_options.search.temperature = 0.75f;
      foundry_local::Request request(foundry_local::Item::Text(
          nlohmann::json{{"answers", {"a", "b"}}}.dump()));
      request.SetOptions(request_options);
      EXPECT_NO_THROW(ranking.ProcessRequest(request));
    }
    EXPECT_TRUE(clm_model->IsLoaded());
    clm_model->Unload();
    EXPECT_FALSE(clm_model->IsLoaded());

    foundry_local::ModelInfo kev_metadata;
    kev_metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR,
                                   "decision");
    auto kev_model = catalog.RegisterModel(
        kev.string(), "kev-4b-generic-cpu:1", kev_metadata);
    kev_model->Load();
    EXPECT_THROW(
        {
          foundry_local::RankingSession wrong_task(*kev_model);
        },
        std::invalid_argument);
    {
      foundry_local::DecisionSession decision(*kev_model);
      const auto answers = nlohmann::json::parse(decision.DecideJson(
          nlohmann::json{
              {"state", {{"weather", "heavy rain"}}},
              {"questions",
               {{"umbrella",
                 {{"type", "noul"},
                  {"instructions", "Should I take an umbrella?"}}}}}}
              .dump()));
      const auto value = answers.at("answers").at("umbrella").at("noul").get<double>();
      EXPECT_GE(value, 0.0);
      EXPECT_LE(value, 1.0);
      try {
        const std::string id{"q\0private", 9};
        nlohmann::json questions = nlohmann::json::object();
        questions[id] = {{"type", "noul"}};
        decision.DecideJson(nlohmann::json{{"questions", questions}}.dump());
        FAIL() << "embedded NUL question ID was accepted";
      } catch (const foundry_local::Error& error) {
        EXPECT_EQ(error.Code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
      }
      EXPECT_THROW(kev_model->Unload(), foundry_local::Error);
    }
    EXPECT_TRUE(kev_model->IsLoaded());
    kev_model->Unload();
    EXPECT_FALSE(kev_model->IsLoaded());
  }

  std::error_code ec;
  std::filesystem::remove_all(app_data, ec);
}

TEST(NonGenerativeSdkTest, RequiredKevPackageRunsFromSharedCache) {
  const auto test_data = fl::test::SafeGetEnv("FOUNDRY_TEST_DATA_DIR");
  ASSERT_FALSE(test_data.empty())
      << "FOUNDRY_TEST_DATA_DIR is required for this real-model test";
  const auto package =
      std::filesystem::path(test_data) / "Microsoft" /
      "kev-0.8b-mixed-generic-cpu-2" / "v2";
  if (!std::filesystem::is_directory(package)) {
    FAIL() << "required KEV package is missing: " << package;
  }

  const auto app_data =
      std::filesystem::temp_directory_path() /
      ("fl_required_kev_sdk_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(app_data);
  {
    foundry_local::Configuration config("required-kev-sdk-test");
    config.SetAppDataDir(app_data.string());
    foundry_local::Manager manager(std::move(config));
    auto& catalog = manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_LOCAL);
    foundry_local::ModelInfo metadata;
    metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR,
                               "decision");
    auto model = catalog.RegisterModel(
        package.string(), "kev-0.8b-mixed-generic-cpu:2", metadata);
    model->Load();
    EXPECT_TRUE(model->IsLoaded());
    {
      foundry_local::DecisionSession session(*model);
      const auto result = nlohmann::json::parse(session.DecideJson(
          nlohmann::json{
              {"state", {{"weather", "heavy rain"}}},
              {"questions",
               {{"umbrella",
                 {{"type", "noul"},
                  {"instructions", "Should I take an umbrella?"}}}}}}
              .dump()));
      const auto value =
          result.at("answers").at("umbrella").at("noul").get<double>();
      EXPECT_GE(value, 0.0);
      EXPECT_LE(value, 1.0);
      EXPECT_THROW(model->Unload(), foundry_local::Error);
    }
    EXPECT_TRUE(model->IsLoaded());
    model->Unload();
    EXPECT_FALSE(model->IsLoaded());
  }
  std::error_code ec;
  std::filesystem::remove_all(app_data, ec);
}

TEST(NonGenerativeSdkTest, RequiredClmPackageRunsFromSharedCache) {
  const auto test_data = fl::test::SafeGetEnv("FOUNDRY_TEST_DATA_DIR");
  ASSERT_FALSE(test_data.empty())
      << "FOUNDRY_TEST_DATA_DIR is required for this real-model test";
  const auto package =
      std::filesystem::path(test_data) / "Microsoft" /
      "clm-v0.1-8b-mixed-generic-cpu-2" / "v2";
  if (!std::filesystem::is_directory(package))
    FAIL() << "required CLM package is missing: " << package;

  const auto app_data =
      std::filesystem::temp_directory_path() /
      ("fl_required_clm_sdk_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(app_data);
  {
    foundry_local::Configuration config("required-clm-sdk-test");
    config.SetAppDataDir(app_data.string());
    foundry_local::Manager manager(std::move(config));
    auto& catalog = manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_LOCAL);
    foundry_local::ModelInfo metadata;
    metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR,
                               "ranking");
    auto model = catalog.RegisterModel(
        package.string(), "clm-v0.1-8b-mixed-generic-cpu:2", metadata);
    model->Load();
    EXPECT_TRUE(model->IsLoaded());
    {
      foundry_local::RankingSession session(*model);
      const auto result = nlohmann::json::parse(session.RankJson(
          nlohmann::json{
              {"context", {{"weather", "heavy rain"}}},
              {"question", "Which activity is more suitable?"},
              {"answers",
               {"Have a picnic outdoors", "Visit an indoor museum"}}}
              .dump()));
      ASSERT_EQ(result.at("ranked").size(), 2u);
      EXPECT_EQ(result.at("ranked").at(0).at("index"), 0u);
      EXPECT_THROW(model->Unload(), foundry_local::Error);
    }
    EXPECT_TRUE(model->IsLoaded());
    model->Unload();
    EXPECT_FALSE(model->IsLoaded());
  }
  std::error_code ec;
  std::filesystem::remove_all(app_data, ec);
}

}  // namespace
