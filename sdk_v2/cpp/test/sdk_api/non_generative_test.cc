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
                                   "text-ranking");
    auto clm_model = catalog.RegisterModel(
        clm.string(), "clm-v0.1-8b-generic-cpu:1", clm_metadata);
    EXPECT_THROW(
        {
          foundry_local::DecisionSession wrong_task(*clm_model);
        },
        std::invalid_argument);
    foundry_local::RankingSession ranking(*clm_model);
    foundry_local::RequestOptions defaults;
    defaults.search.temperature = 0.5;
    ranking.SetOptions(defaults);
    const auto ranked = nlohmann::json::parse(ranking.RankJson(
        nlohmann::json{
            {"context", {{"weather", "heavy rain"}}},
            {"question", "Which activity is more suitable?"},
            {"answers",
             {"Have a picnic outdoors", "Visit an indoor museum"}}}
            .dump()));
    ASSERT_EQ(ranked.at("ranked").size(), 2u);
    EXPECT_THROW(
        ranking.RankJson(
            nlohmann::json{{"model", "different-model"},
                           {"answers", {"a", "b"}}}
                .dump()),
        foundry_local::Error);

    foundry_local::RequestOptions request_options;
    request_options.search.temperature = 0.75;
    foundry_local::Request request(foundry_local::Item::Text(
        nlohmann::json{{"answers", {"a", "b"}}}.dump()));
    request.SetOptions(request_options);
    EXPECT_NO_THROW(ranking.ProcessRequest(request));

    foundry_local::RequestOptions unsupported;
    unsupported.additional_options.Set("unsupported_option", "true");
    ranking.SetOptions(unsupported);
    EXPECT_THROW(ranking.RankJson(
                     nlohmann::json{{"answers", {"a", "b"}}}.dump()),
                 foundry_local::Error);

    foundry_local::ModelInfo kev_metadata;
    kev_metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR,
                                   "typed-decision");
    auto kev_model = catalog.RegisterModel(
        kev.string(), "kev-4b-generic-cpu:1", kev_metadata);
    EXPECT_THROW(
        {
          foundry_local::RankingSession wrong_task(*kev_model);
        },
        std::invalid_argument);
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
    EXPECT_THROW(
        decision.DecideJson(
            nlohmann::json{
                {"model", "different-model"},
                {"questions", {{"q", {{"type", "noul"}}}}}}
                .dump()),
        foundry_local::Error);
  }

  std::error_code ec;
  std::filesystem::remove_all(app_data, ec);
}

}  // namespace
