// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contracts/non_generative.h"
#include "inferencing/predictive/non_generative_runtime.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fl {
namespace {

TEST(NonGenerativeRuntimeTest, ExportedPackagesRunWhenConfigured) {
  const char* root_value = std::getenv("FOUNDRY_LOCAL_NON_GENERATIVE_TEST_ROOT");
  if (!root_value || !*root_value) {
    GTEST_SKIP() << "set FOUNDRY_LOCAL_NON_GENERATIVE_TEST_ROOT to run exported packages";
  }
  const std::filesystem::path root(root_value);
  const auto clm = root / "clm-v0.1-8b-fp32";
  const auto kev = root / "kev-4b-fp32";
  if (!std::filesystem::is_directory(clm) || !std::filesystem::is_directory(kev)) {
    GTEST_SKIP() << "configured root does not contain CLM and KEV FP32 packages";
  }

  RankingRequest ranking;
  ranking.context = {{"weather", "heavy rain"}};
  ranking.question = "Which activity is more suitable?";
  ranking.answers = {"Have a picnic outdoors", "Visit an indoor museum"};
  auto ranked = RankingRuntime(clm.string()).Rank(ranking);
  ASSERT_EQ(ranked.at("ranked").size(), 2u);
  EXPECT_EQ(ranked.at("ranked").at(0).at("rank"), 1);

  auto decision = nlohmann::ordered_json{
      {"state", {{"weather", "heavy rain"}}},
      {"questions",
       {{"umbrella",
         {{"type", "noul"}, {"instructions", "Should I take an umbrella?"}}}}},
  }
                      .get<NonGenerativeRequest>();
  auto answers = DecisionRuntime(kev.string()).Decide(decision);
  ASSERT_TRUE(answers.contains("umbrella"));
  EXPECT_EQ(answers.at("umbrella").at("type"), "noul");
  EXPECT_GE(answers.at("umbrella").at("noul").get<double>(), 0.0);
  EXPECT_LE(answers.at("umbrella").at("noul").get<double>(), 1.0);
}

}  // namespace
}  // namespace fl
