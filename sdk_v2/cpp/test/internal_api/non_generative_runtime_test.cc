// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contracts/non_generative.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/predictive/non_generative_runtime.h"
#include "internal_api/test_helpers.h"
#include "model.h"
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

class ModelLoadManagerTestAccess {
 public:
  static std::shared_ptr<void> Acquire(
      ModelLoadManager& manager, const std::string& identity,
      uint64_t bytes, const std::string& readiness_group,
      const std::function<std::shared_ptr<void>()>& factory) {
    return manager.AcquireManagedRuntime(
        identity, identity, "/" + identity, bytes, readiness_group, factory);
  }

  static void Clear(ModelLoadManager& manager) {
    std::map<std::string, std::shared_ptr<ManagedModelRuntime>> removed;
    {
      std::lock_guard lock(manager.mutex_);
      removed.swap(manager.loaded_runtimes_);
    }
  }
};

namespace {

TEST(ManagedModelRuntimeTest, SameKeyColdLoadsAreCoalesced) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog());
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
    return ModelLoadManagerTestAccess::Acquire(
        manager, "same", 1, "cpu", factory);
  });
  entered.get_future().wait();
  auto second = std::async(std::launch::async, [&] {
    return ModelLoadManagerTestAccess::Acquire(
        manager, "same", 1, "cpu", factory);
  });

  EXPECT_EQ(first.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  EXPECT_EQ(second.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(constructions.load(), 1);
}

TEST(ManagedModelRuntimeTest, FailedColdLoadIsRemovedAndCanBeRetried) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog());
  std::atomic<int> constructions{0};
  auto factory = [&]() -> std::shared_ptr<void> {
    if (++constructions == 1) throw std::runtime_error("synthetic failure");
    return std::make_shared<int>(42);
  };

  EXPECT_THROW(ModelLoadManagerTestAccess::Acquire(
                   manager, "retry", 1, "cpu", factory),
               std::runtime_error);
  EXPECT_NE(ModelLoadManagerTestAccess::Acquire(
                manager, "retry", 1, "cpu", factory),
            nullptr);
  EXPECT_EQ(constructions.load(), 2);
}

TEST(ManagedModelRuntimeTest, RejectsLoadsBeyondMemoryBudget) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog(), 100, 1);
  auto factory = []() -> std::shared_ptr<void> {
    return std::make_shared<int>(42);
  };

  EXPECT_NE(ModelLoadManagerTestAccess::Acquire(
                manager, "first", 80, "cpu", factory),
            nullptr);
  EXPECT_THROW(ModelLoadManagerTestAccess::Acquire(
                   manager, "second", 21, "cpu", factory),
               std::runtime_error);
}

TEST(ManagedModelRuntimeTest, ReleasesAdmissionAfterRuntimeDestruction) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog(), 100, 1);
  auto factory = []() -> std::shared_ptr<void> {
    return std::make_shared<int>(42);
  };

  auto lease = ModelLoadManagerTestAccess::Acquire(
      manager, "first", 80, "cpu", factory);
  ModelLoadManagerTestAccess::Clear(manager);
  EXPECT_THROW(ModelLoadManagerTestAccess::Acquire(
                   manager, "second", 21, "cpu", factory),
               std::runtime_error);
  lease.reset();
  EXPECT_NE(ModelLoadManagerTestAccess::Acquire(
                manager, "second", 21, "cpu", factory),
            nullptr);
}

TEST(ManagedModelRuntimeTest, ActiveLeaseBlocksExplicitUnload) {
  test::FakeServiceBindings service;
  ModelInfo info;
  info.model_id = "typed:1";
  info.name = "typed";
  info.alias = "typed";
  info.task = "text-ranking";
  auto model = Model::FromLocalRegistration(
      std::move(info), "/typed:1", service.download_manager,
      service.model_load_manager);
  auto lease = ModelLoadManagerTestAccess::Acquire(
      service.model_load_manager, "typed:1", 1, "cpu",
      []() -> std::shared_ptr<void> { return std::make_shared<int>(42); });

  EXPECT_THROW(service.model_load_manager.UnloadNonGenerativeRuntime(model),
               std::exception);
  lease.reset();
  EXPECT_TRUE(
      service.model_load_manager.UnloadNonGenerativeRuntime(model));
}

TEST(ManagedModelRuntimeTest, ShutdownRejectsNewAcquisitions) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog());
  manager.RejectNewLoads();

  EXPECT_THROW(
      ModelLoadManagerTestAccess::Acquire(
          manager, "shutdown", 1, "cpu",
          []() -> std::shared_ptr<void> {
            return std::make_shared<int>(42);
          }),
      std::exception);
}

TEST(ManagedModelRuntimeTest, SerializesReadinessWithinProviderGroup) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog(), 1000, 1);
  std::promise<void> first_entered;
  std::promise<void> release_first;
  auto release_future = release_first.get_future().share();
  auto first = std::async(std::launch::async, [&] {
    return ModelLoadManagerTestAccess::Acquire(
        manager, "first", 1, "cuda", [&]() -> std::shared_ptr<void> {
          first_entered.set_value();
          release_future.wait();
          return std::make_shared<int>(1);
        });
  });
  first_entered.get_future().wait();
  std::atomic<bool> second_entered{false};
  auto second = std::async(std::launch::async, [&] {
    return ModelLoadManagerTestAccess::Acquire(
        manager, "second", 1, "cuda", [&]() -> std::shared_ptr<void> {
          second_entered = true;
          return std::make_shared<int>(2);
        });
  });
  EXPECT_EQ(second.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  EXPECT_FALSE(second_entered.load());

  auto cpu = ModelLoadManagerTestAccess::Acquire(
      manager, "cpu", 1, "cpu",
      []() -> std::shared_ptr<void> { return std::make_shared<int>(3); });
  EXPECT_NE(cpu, nullptr);
  release_first.set_value();
  EXPECT_NE(first.get(), nullptr);
  EXPECT_NE(second.get(), nullptr);
  EXPECT_TRUE(second_entered.load());
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
  for (size_t index = 0; index < ranked.at("ranked").size(); ++index) {
    const auto& item = ranked.at("ranked").at(index);
    EXPECT_TRUE(item.contains("id"));
    EXPECT_TRUE(item.contains("index"));
    EXPECT_EQ(item.at("candidate"),
              ranking.answers.at(item.at("index").get<size_t>()));
  }

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
