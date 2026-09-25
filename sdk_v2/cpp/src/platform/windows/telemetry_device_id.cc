// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "platform/telemetry_device_id.h"

#include "telemetry/invocation_context.h"
#include "telemetry/telemetry_environment.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fl::TelemetryDeviceIdPlatform {

namespace {

constexpr size_t kMaxDeviceIdSize = 256;
constexpr const char* kRegistryPath = "SOFTWARE\\Microsoft\\DeveloperTools\\.onnxruntime";
constexpr const char* kRegistryValueName = "deviceid";

std::string TrimDeviceId(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
    value.pop_back();
  }
  return value;
}

class ScopedWinHandle {
 public:
  explicit ScopedWinHandle(HANDLE handle = nullptr) : handle_(handle) {}
  ~ScopedWinHandle() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
  }

  ScopedWinHandle(const ScopedWinHandle&) = delete;
  ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;

  HANDLE Get() const { return handle_; }

 private:
  HANDLE handle_ = nullptr;
};

class ScopedDeviceIdMutex {
 public:
  ScopedDeviceIdMutex() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return;
    }
    ScopedWinHandle token_handle(token);

    DWORD size = 0;
    ::GetTokenInformation(token_handle.Get(), TokenUser, nullptr, 0, &size);
    if (size == 0) {
      return;
    }

    std::vector<unsigned char> token_info(size);
    if (!::GetTokenInformation(token_handle.Get(), TokenUser, token_info.data(), size, &size)) {
      return;
    }

    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token_info.data());
    if (!::IsValidSid(token_user->User.Sid)) {
      return;
    }

    uint64_t sid_hash = 14695981039346656037ULL;
    const auto* sid_bytes = static_cast<const unsigned char*>(token_user->User.Sid);
    const DWORD sid_size = ::GetLengthSid(token_user->User.Sid);
    for (DWORD i = 0; i < sid_size; ++i) {
      sid_hash ^= sid_bytes[i];
      sid_hash *= 1099511628211ULL;
    }

    std::array<wchar_t, 96> mutex_name{};
    _snwprintf_s(mutex_name.data(), mutex_name.size(), _TRUNCATE,
                 L"Global\\Microsoft.DeveloperTools.OnnxRuntime.DeviceId.%016llx",
                 static_cast<unsigned long long>(sid_hash));

    handle_ = ::CreateMutexW(nullptr, FALSE, mutex_name.data());
    if (handle_ == nullptr) {
      return;
    }

    const DWORD wait_result = ::WaitForSingleObject(handle_, 1000);
    acquired_ = wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED;
  }

  ~ScopedDeviceIdMutex() {
    if (acquired_) {
      ::ReleaseMutex(handle_);
    }
    if (handle_ != nullptr) {
      ::CloseHandle(handle_);
    }
  }

  ScopedDeviceIdMutex(const ScopedDeviceIdMutex&) = delete;
  ScopedDeviceIdMutex& operator=(const ScopedDeviceIdMutex&) = delete;

  explicit operator bool() const { return acquired_; }

 private:
  HANDLE handle_ = nullptr;
  bool acquired_ = false;
};

bool ReadRegistryDeviceId(std::string& value, bool& found) {
  found = false;
  DWORD size = 0;
  LSTATUS status = ::RegGetValueA(HKEY_CURRENT_USER, kRegistryPath, kRegistryValueName,
                                  RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, &size);
  if (status == ERROR_FILE_NOT_FOUND) {
    return true;
  }
  if (status != ERROR_SUCCESS) {
    return false;
  }

  if (size == 0 || size > kMaxDeviceIdSize + 1) {
    found = true;
    value.clear();
    return true;
  }

  std::string buffer(size, '\0');
  status = ::RegGetValueA(HKEY_CURRENT_USER, kRegistryPath, kRegistryValueName,
                          RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer.data(), &size);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  if (!buffer.empty() && buffer.back() == '\0') {
    buffer.pop_back();
  }

  value = std::move(buffer);
  found = true;
  return true;
}

bool WriteRegistryDeviceId(std::string_view value) {
  HKEY key = nullptr;
  LSTATUS status = ::RegCreateKeyExA(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0,
                                     KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) {
    return false;
  }

  std::string terminated(value);
  status = ::RegSetValueExA(key, kRegistryValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(terminated.c_str()),
                            static_cast<DWORD>(terminated.size() + 1));
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

}  // namespace

bool UsesPlatformProvidedId() {
  return false;
}

std::filesystem::path GetStorageDirectory() {
  return {};
}

std::filesystem::path EnsureStorageDirectory() {
  return {};
}

std::filesystem::path GetCacheDirectory() {
  auto base = TelemetryEnvironment::GetEnv("LOCALAPPDATA");
  if (base.empty()) {
    const auto user_profile = TelemetryEnvironment::GetEnv("USERPROFILE");
    if (!user_profile.empty()) {
      base = (std::filesystem::path(user_profile) / "AppData" / "Local").string();
    }
  }
  return base.empty()
             ? std::filesystem::path{}
             : std::filesystem::path(base) / "Microsoft" / "DeveloperTools" / ".onnxruntime";
}

std::filesystem::path EnsureCacheDirectory() {
  const auto directory = GetCacheDirectory();
  if (directory.empty()) {
    return {};
  }

  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return error ? std::filesystem::path{} : directory;
}

LoadResult LoadOrCreate() {
  LoadResult result{{}, TelemetryDeviceIdStatus::kNew};
  bool found = false;
  const auto read_existing = [&]() -> bool {
    if (!ReadRegistryDeviceId(result.value, found)) {
      result.status = TelemetryDeviceIdStatus::kFailed;
      return false;
    }
    if (!found) {
      return false;
    }

    result.value = TrimDeviceId(std::move(result.value));
    if (result.value.size() <= kMaxDeviceIdSize && TelemetryDeviceId::IsValidGuid(result.value)) {
      result.status = TelemetryDeviceIdStatus::kExisting;
      return true;
    }
    result.status = TelemetryDeviceIdStatus::kCorrupted;
    return false;
  };

  if (read_existing()) {
    return result;
  }

  const bool was_corrupted = result.status == TelemetryDeviceIdStatus::kCorrupted;
  ScopedDeviceIdMutex mutex;
  if (!mutex) {
    if (read_existing()) {
      return result;
    }
    return {{}, TelemetryDeviceIdStatus::kFailed};
  }

  result = {{}, TelemetryDeviceIdStatus::kNew};
  found = false;
  if (read_existing()) {
    return result;
  }
  if (result.status == TelemetryDeviceIdStatus::kFailed) {
    return result;
  }

  const bool regenerated = was_corrupted || result.status == TelemetryDeviceIdStatus::kCorrupted;
  result.value = GenerateGuidV4();
  result.status = WriteRegistryDeviceId(result.value)
                      ? regenerated ? TelemetryDeviceIdStatus::kCorrupted : TelemetryDeviceIdStatus::kNew
                      : TelemetryDeviceIdStatus::kFailed;
  return result;
}

}  // namespace fl::TelemetryDeviceIdPlatform
