// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <utility>
#include <vector>

namespace fl {

class ILogger;

bool HasQualifyingComputeCapability(const std::vector<std::pair<int, int>>& capabilities,
                                    int min_major = 5,
                                    int min_minor = 0);

class NvmlGpuDetector {
 public:
  static bool HasNvidiaGpu(ILogger& logger);
};

}  // namespace fl
