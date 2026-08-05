// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_bundle_manifest.h"

#include <optional>

namespace fl {

enum class CudaEpPlatform {
  WindowsX64,
  WindowsArm64,
  LinuxX64,
  LinuxArm64,
  Unsupported,
};

std::optional<EpBundleManifest> BuildCudaEpManifest(CudaEpPlatform platform);

}  // namespace fl
