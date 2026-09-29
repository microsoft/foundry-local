// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/device_id.h"

#include <filesystem>
#include <string>

namespace fl::TelemetryDeviceIdPlatform {

struct LoadResult {
  std::string value;
  TelemetryDeviceIdStatus status;
};

bool UsesPlatformProvidedId();
std::filesystem::path GetStorageDirectory();
std::filesystem::path EnsureStorageDirectory();
std::filesystem::path GetCacheDirectory();
std::filesystem::path EnsureCacheDirectory();
LoadResult LoadOrCreate();

}  // namespace fl::TelemetryDeviceIdPlatform
