// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Unit tests for the pure compute-capability logic in ep_detection/nvml_gpu_detector. The NVML
// dynamic-loading path itself requires a real NVIDIA driver and is exercised only by
// EpDetectionApiTest integration tests, not here.
#include "ep_detection/nvml_gpu_detector.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace fl {

TEST(HasQualifyingComputeCapabilityTest, EmptyListReturnsFalse) {
  EXPECT_FALSE(HasQualifyingComputeCapability({}));
}

TEST(HasQualifyingComputeCapabilityTest, ExactlyAtDefaultThresholdQualifies) {
  EXPECT_TRUE(HasQualifyingComputeCapability({{5, 0}}));
}

TEST(HasQualifyingComputeCapabilityTest, BelowDefaultThresholdDoesNotQualify) {
  EXPECT_FALSE(HasQualifyingComputeCapability({{4, 9}}));
  EXPECT_FALSE(HasQualifyingComputeCapability({{3, 5}}));
}

TEST(HasQualifyingComputeCapabilityTest, AboveDefaultThresholdQualifies) {
  EXPECT_TRUE(HasQualifyingComputeCapability({{7, 5}}));
  EXPECT_TRUE(HasQualifyingComputeCapability({{9, 0}}));
}

TEST(HasQualifyingComputeCapabilityTest, HigherMajorWithLowerMinorStillQualifies) {
  // (6, 0) beats (5, 9) because major dominates minor.
  EXPECT_TRUE(HasQualifyingComputeCapability({{6, 0}}));
}

TEST(HasQualifyingComputeCapabilityTest, OneQualifyingDeviceAmongManyIsEnough) {
  std::vector<std::pair<int, int>> capabilities = {{3, 0}, {4, 5}, {8, 6}};
  EXPECT_TRUE(HasQualifyingComputeCapability(capabilities));
}

TEST(HasQualifyingComputeCapabilityTest, NoDeviceQualifiesReturnsFalse) {
  std::vector<std::pair<int, int>> capabilities = {{3, 0}, {4, 5}, {4, 9}};
  EXPECT_FALSE(HasQualifyingComputeCapability(capabilities));
}

TEST(HasQualifyingComputeCapabilityTest, CustomThresholdIsRespected) {
  EXPECT_TRUE(HasQualifyingComputeCapability({{7, 0}}, /*min_major=*/7, /*min_minor=*/0));
  EXPECT_FALSE(HasQualifyingComputeCapability({{6, 9}}, /*min_major=*/7, /*min_minor=*/0));
}

TEST(HasQualifyingComputeCapabilityTest, CustomThresholdMinorBoundary) {
  EXPECT_TRUE(HasQualifyingComputeCapability({{8, 6}}, /*min_major=*/8, /*min_minor=*/6));
  EXPECT_FALSE(HasQualifyingComputeCapability({{8, 5}}, /*min_major=*/8, /*min_minor=*/6));
}

}  // namespace fl
