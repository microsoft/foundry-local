// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contracts/non_generative.h"
#include "inferencing/predictive/non_generative_runtime.h"
#include "service/non_generative_handlers.h"
#include "utils/safe_getenv.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace fl {
namespace {

TEST(NonGenerativeRuntimeStateTest, SameKeyColdLoadsAreCoalesced) {
  auto state = CreateNonGenerativeRuntimeState();
  std::atomic<int> constructions{0};
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto factory = [&]() -> std::shared_ptr<void> {
    ++constructions;
    entered.set_value();
    release_future.wait();
    return std::make_shared<int>(42);
  };

  auto first = std::async(std::launch::async, [&] {
    return state->Acquire("same", "model:1", nullptr, factory);
  });
  entered.get_future().wait();
  auto second = std::async(std::launch::async, [&] {
    return state->Acquire("same", "model:1", nullptr, factory);
  });

  EXPECT_EQ(first.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  EXPECT_EQ(second.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(constructions.load(), 1);
}

TEST(NonGenerativeRuntimeStateTest, FailedColdLoadIsRemovedAndCanBeRetried) {
  auto state = CreateNonGenerativeRuntimeState();
  std::atomic<int> constructions{0};
  auto factory = [&]() -> std::shared_ptr<void> {
    if (++constructions == 1) throw std::runtime_error("synthetic failure");
    return std::make_shared<int>(42);
  };

  EXPECT_THROW(state->Acquire("retry", "model:1", nullptr, factory),
               std::runtime_error);
  EXPECT_NE(state->Acquire("retry", "model:1", nullptr, factory), nullptr);
  EXPECT_EQ(constructions.load(), 2);
}

TEST(NonGenerativeRuntimeTest, ExportedPackagesRunWhenConfigured) {
  const auto root_value = test::SafeGetEnv("FOUNDRY_LOCAL_NON_GENERATIVE_TEST_ROOT");
  if (root_value.empty()) {
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
