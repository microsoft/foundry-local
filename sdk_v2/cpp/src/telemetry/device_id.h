// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace fl {

enum class TelemetryDeviceIdStatus {
  kNew,
  kExisting,
  kCorrupted,
  kFailed,
  kPlatform,
};

class TelemetryDeviceId {
 public:
  static TelemetryDeviceId& Instance();

  std::string GetValue();
  TelemetryDeviceIdStatus GetStatus();
  std::string GetStatusString();

  static std::filesystem::path GetStorageDirectory();
  static std::filesystem::path EnsureStorageDirectory();
  static std::filesystem::path GetCacheDirectory();
  static std::filesystem::path EnsureCacheDirectory();
  static std::string HashForTelemetry(std::string_view raw_device_id);
  static bool IsValidGuid(std::string_view value);

 private:
  TelemetryDeviceId() = default;

  void InitializeLocked();
  static std::string StatusToString(TelemetryDeviceIdStatus status);

  std::mutex mutex_;
  std::string device_id_;
  TelemetryDeviceIdStatus status_ = TelemetryDeviceIdStatus::kNew;
  bool initialized_ = false;
};

}  // namespace fl
