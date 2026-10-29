// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "foundry_local/foundry_local_cpp.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

TEST(NonGenerativeSdkTest, ExportedPackagesRunWhenConfigured) {
  const char* root_value =
      std::getenv("FOUNDRY_LOCAL_NON_GENERATIVE_TEST_ROOT");
  if (!root_value || !*root_value)
    GTEST_SKIP() << "set FOUNDRY_LOCAL_NON_GENERATIVE_TEST_ROOT";

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
                                   "text-ranking");
    auto clm_model = catalog.RegisterModel(
        clm.string(), "clm-v0.1-8b-generic-cpu:1", clm_metadata);
    foundry_local::RankingSession ranking(*clm_model);
    const auto ranked = nlohmann::json::parse(ranking.RankJson(
        nlohmann::json{
            {"context", {{"weather", "heavy rain"}}},
            {"question", "Which activity is more suitable?"},
            {"answers",
             {"Have a picnic outdoors", "Visit an indoor museum"}}}
            .dump()));
    ASSERT_EQ(ranked.at("ranked").size(), 2u);

    foundry_local::ModelInfo kev_metadata;
    kev_metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR,
                                   "typed-decision");
    auto kev_model = catalog.RegisterModel(
        kev.string(), "kev-4b-generic-cpu:1", kev_metadata);
    foundry_local::DecisionSession decision(*kev_model);
    const auto answers = nlohmann::json::parse(decision.DecideJson(
        nlohmann::json{
            {"state", {{"weather", "heavy rain"}}},
            {"questions",
             {{"umbrella",
               {{"type", "noul"},
                {"instructions", "Should I take an umbrella?"}}}}}}
            .dump()));
    EXPECT_TRUE(answers.at("answers").contains("umbrella"));
  }

  std::error_code ec;
  std::filesystem::remove_all(app_data, ec);
}

}  // namespace
