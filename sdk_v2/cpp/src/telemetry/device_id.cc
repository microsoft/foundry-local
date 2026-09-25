// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/device_id.h"

#include "platform/telemetry_device_id.h"
#include "util/sha256.h"

#include <utility>

namespace fl {

TelemetryDeviceId& TelemetryDeviceId::Instance() {
  static TelemetryDeviceId instance;
  return instance;
}

std::string TelemetryDeviceId::GetValue() {
  std::lock_guard<std::mutex> lock(mutex_);
  InitializeLocked();
  return device_id_;
}

TelemetryDeviceIdStatus TelemetryDeviceId::GetStatus() {
  std::lock_guard<std::mutex> lock(mutex_);
  InitializeLocked();
  return status_;
}

std::string TelemetryDeviceId::GetStatusString() {
  return StatusToString(GetStatus());
}

std::filesystem::path TelemetryDeviceId::GetStorageDirectory() {
  return TelemetryDeviceIdPlatform::GetStorageDirectory();
}

std::filesystem::path TelemetryDeviceId::EnsureStorageDirectory() {
  return TelemetryDeviceIdPlatform::EnsureStorageDirectory();
}

std::filesystem::path TelemetryDeviceId::GetCacheDirectory() {
  return TelemetryDeviceIdPlatform::GetCacheDirectory();
}

std::filesystem::path TelemetryDeviceId::EnsureCacheDirectory() {
  return TelemetryDeviceIdPlatform::EnsureCacheDirectory();
}

std::string TelemetryDeviceId::HashForTelemetry(std::string_view raw_device_id) {
  return raw_device_id.empty() ? std::string{} : "c:" + Sha256String(raw_device_id);
}

bool TelemetryDeviceId::IsValidGuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }

  for (size_t i = 0; i < value.size(); ++i) {
    const char character = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }

    const bool is_hex = (character >= '0' && character <= '9') ||
                        (character >= 'a' && character <= 'f') ||
                        (character >= 'A' && character <= 'F');
    if (!is_hex) {
      return false;
    }
  }
  return true;
}

void TelemetryDeviceId::InitializeLocked() {
  if (initialized_) {
    return;
  }
  initialized_ = true;

  if (TelemetryDeviceIdPlatform::UsesPlatformProvidedId()) {
    status_ = TelemetryDeviceIdStatus::kPlatform;
    return;
  }

  auto result = TelemetryDeviceIdPlatform::LoadOrCreate();
  device_id_ = std::move(result.value);
  status_ = result.status;
}

std::string TelemetryDeviceId::StatusToString(TelemetryDeviceIdStatus status) {
  switch (status) {
    case TelemetryDeviceIdStatus::kNew:
      return "New";
    case TelemetryDeviceIdStatus::kExisting:
      return "Existing";
    case TelemetryDeviceIdStatus::kCorrupted:
      return "Corrupted";
    case TelemetryDeviceIdStatus::kFailed:
      return "Failed";
    case TelemetryDeviceIdStatus::kPlatform:
      return "Platform";
    default:
      return "Unknown";
  }
}

}  // namespace fl
