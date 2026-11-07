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