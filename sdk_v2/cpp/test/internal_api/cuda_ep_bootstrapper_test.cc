// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/cuda_ep_bootstrapper.h"

#include <gtest/gtest.h>

namespace fl {

TEST(CudaEpBootstrapperTest, PlatformSupportMatchesPublishedBundles) {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || \
    (defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__))
  EXPECT_TRUE(CudaEpBootstrapper::IsSupportedPlatform());
#else
  EXPECT_FALSE(CudaEpBootstrapper::IsSupportedPlatform());
#endif
}

}  // namespace fl
