#include "catalog/capacity_compatibility.h"

#include <gtest/gtest.h>

#include <array>

using namespace fl;

TEST(CapacityCompatibilityTest, TotalVramBoundaryAndUnknowns) {
  const std::array<DeviceCapacity, 1> small{{{8}}}, exact{{{16}}}, unknown{{{std::nullopt}}}, zero{{{0}}};
  EXPECT_FALSE(MeetsCapacityRequirements(true, {16}, small));
  EXPECT_TRUE(MeetsCapacityRequirements(true, {16}, exact));
  EXPECT_TRUE(MeetsCapacityRequirements(true, {16}, unknown));
  EXPECT_TRUE(MeetsCapacityRequirements(true, {}, zero));
  EXPECT_FALSE(MeetsCapacityRequirements(true, {16}, zero));
  EXPECT_TRUE(MeetsCapacityRequirements(true, {0}, zero));
  EXPECT_TRUE(MeetsCapacityRequirements(true, {16}, {}));
  EXPECT_TRUE(MeetsCapacityRequirements(false, {16}, small));
}

TEST(CapacityCompatibilityTest, RequiresOneFittingDeviceNotAggregatedMemory) {
  const std::array<DeviceCapacity, 2> small{{{8}, {8}}}, fitting{{{8}, {16}}}, unknown{{{8}, {std::nullopt}}};
  EXPECT_FALSE(MeetsCapacityRequirements(true, {16}, small));
  EXPECT_TRUE(MeetsCapacityRequirements(true, {16}, fitting));
  EXPECT_TRUE(MeetsCapacityRequirements(true, {16}, unknown));
}

TEST(CapacityCompatibilityTest, DistinguishesUnknownAndRetainsFittingDeviceIdentity) {
  const std::array<ExecutionTarget, 3> targets{{
      {"CUDAExecutionProvider", 0, "GPU", {8}},
      {"CUDAExecutionProvider", 1, "GPU", {24}},
      {"CUDAExecutionProvider", 2, "GPU", {}},
  }};
  const auto result = EvaluateCapacity(true, {16}, targets);
  EXPECT_EQ(result.state, CompatibilityState::kFits);
  EXPECT_EQ(result.reason, CompatibilityReason::kSufficientCapacity);
  ASSERT_EQ(result.eligible_targets.size(), 2u);
  EXPECT_EQ(result.eligible_targets.front().device_id, 1u);
  EXPECT_EQ(EvaluateCapacity(true, {32}, targets).state, CompatibilityState::kUnknown);
  EXPECT_EQ(EvaluateCapacity(true, {32}, std::span(targets).first(2)).state, CompatibilityState::kDoesNotFit);
  EXPECT_EQ(EvaluateCapacity(true, {}, targets).reason, CompatibilityReason::kMissingRequirement);
}

TEST(CapacityCompatibilityTest, GenericGpuEvaluatesProvidersTogetherAndPrefersKnownFit) {
  const std::array<ExecutionTarget, 2> targets{{
      {"CUDAExecutionProvider", 0, "GPU", {8}},
      {"WebGpuExecutionProvider", 7, "GPU", {24}},
  }};
  const auto result = EvaluateProviderCapacity(true, {16}, targets, {}, true);
  ASSERT_EQ(result.state, CompatibilityState::kFits);
  ASSERT_EQ(result.eligible_targets.size(), 1u);
  EXPECT_EQ(result.eligible_targets.front().provider, "WebGpuExecutionProvider");
  EXPECT_EQ(EvaluateProviderCapacity(true, {16}, targets, "cuda").state, CompatibilityState::kDoesNotFit);
}

TEST(CapacityCompatibilityTest, MultimodalGenericGpuDoesNotUseCudaCapacityForDefaultWebGpuRoute) {
  const std::array<ExecutionTarget, 2> targets{{
      {"CUDAExecutionProvider", 0, "GPU", {24}},
      {"WebGpuExecutionProvider", 1, "GPU", {8}},
  }};
  EXPECT_EQ(EvaluateProviderCapacity(true, {16}, targets, "webgpu", true, true).state,
            CompatibilityState::kDoesNotFit);
  EXPECT_EQ(EvaluateProviderCapacity(true, {16}, targets, "cuda").state, CompatibilityState::kFits);
  const auto fallback = EvaluateProviderCapacity(true, {16}, std::span(targets).first(1), "cuda", true, true);
  EXPECT_EQ(fallback.state, CompatibilityState::kFits);
}