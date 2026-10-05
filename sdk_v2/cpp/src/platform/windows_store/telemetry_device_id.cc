// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "platform/telemetry_device_id.h"

namespace fl::TelemetryDeviceIdPlatform {

bool UsesPlatformProvidedId() {
  return true;
}

std::filesystem::path GetStorageDirectory() {
  return {};
}

std::filesystem::path EnsureStorageDirectory() {
  return {};
}

std::filesystem::path GetCacheDirectory() {
  return {};
}

std::filesystem::path EnsureCacheDirectory() {
  return {};
}

LoadResult LoadOrCreate() {
  return {{}, TelemetryDeviceIdStatus::kPlatform};
}

}  // namespace fl::TelemetryDeviceIdPlatform
