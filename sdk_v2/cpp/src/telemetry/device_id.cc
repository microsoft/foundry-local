// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/device_id.h"

#include "telemetry/invocation_context.h"
#include "telemetry/telemetry_environment.h"
#include "util/sha256.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace fl {

namespace {

constexpr size_t kMaxDeviceIdFileSize = 256;
constexpr const char* kDeviceIdFileName = "deviceid";
#ifdef _WIN32
constexpr const char* kRegistryPath = "SOFTWARE\\Microsoft\\DeveloperTools\\.onnxruntime";
constexpr const char* kRegistryValueName = "deviceid";
#endif

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
constexpr bool kUsePlatformDeviceId = true;
#else
constexpr bool kUsePlatformDeviceId = false;
#endif

std::filesystem::path HomeDirectory() {
  auto home = TelemetryEnvironment::GetEnv("HOME");
  return home.empty() ? std::filesystem::path{} : std::filesystem::path(home);
}

std::string TrimDeviceId(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
    value.pop_back();
  }
  return value;
}

#ifdef _WIN32
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
#else
bool CreateDirectoryTreeOwnerOnly(const std::filesystem::path& dir, bool leaf = true) {
  if (dir.empty()) {
    return false;
  }

  std::error_code ec;
  if (leaf && std::filesystem::is_symlink(dir, ec)) {
    return false;
  }

  ec.clear();
  if (std::filesystem::exists(dir, ec)) {
    if (!std::filesystem::is_directory(dir, ec)) {
      return false;
    }
    if (leaf) {
      ec.clear();
      std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace, ec);
      if (ec) {
        return false;
      }
    }
    return true;
  }

  const auto parent = dir.parent_path();
  if (!parent.empty() && parent != dir && !CreateDirectoryTreeOwnerOnly(parent, false)) {
    return false;
  }

  const auto dir_path = dir.string();
  if (::mkdir(dir_path.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
    return false;
  }

  ec.clear();
  if (leaf && std::filesystem::is_symlink(dir, ec)) {
    return false;
  }
  ec.clear();
  if (!std::filesystem::is_directory(dir, ec)) {
    return false;
  }
  if (leaf) {
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
      return false;
    }
  }
  return true;
}

enum class DeviceIdPublishResult {
  kCreated,
  kAlreadyExists,
  kFailed,
};

DeviceIdPublishResult PublishDeviceIdFileNoFollow(const std::filesystem::path& file,
                                                  std::string_view value,
                                                  bool replace_existing) {
  std::filesystem::path temp = file;
  temp += ".tmp." + GenerateGuidV4();

  int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  const auto temp_path = temp.string();
  const int fd = ::open(temp_path.c_str(), flags, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return DeviceIdPublishResult::kFailed;
  }

  bool wrote = true;
  const char* data = value.data();
  size_t remaining = value.size();
  while (remaining > 0) {
    const ssize_t n = ::write(fd, data, remaining);
    if (n <= 0) {
      wrote = false;
      break;
    }
    data += n;
    remaining -= static_cast<size_t>(n);
  }
  if (::close(fd) != 0) {
    wrote = false;
  }

  std::error_code ec;
  if (!wrote) {
    std::filesystem::remove(temp, ec);
    return DeviceIdPublishResult::kFailed;
  }

  if (replace_existing) {
    std::filesystem::rename(temp, file, ec);
  } else {
    std::filesystem::create_hard_link(temp, file, ec);
  }
  if (ec) {
    const bool already_exists = !replace_existing && ec == std::errc::file_exists;
    std::filesystem::remove(temp, ec);
    return already_exists ? DeviceIdPublishResult::kAlreadyExists : DeviceIdPublishResult::kFailed;
  }
  std::filesystem::remove(temp, ec);

  ec.clear();
  std::filesystem::permissions(file,
                               std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, ec);
  return DeviceIdPublishResult::kCreated;
}
#endif

}  // namespace

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
#ifdef _WIN32
  return {};
#elif defined(__APPLE__)
  auto home = HomeDirectory();
  return home.empty() ? std::filesystem::path{} :
                        home / "Library" / "Application Support" / "Microsoft" / "DeveloperTools" / ".onnxruntime";
#else
  auto cache_base = TelemetryEnvironment::GetEnv("XDG_CACHE_HOME");
  std::filesystem::path base;
  if (!cache_base.empty()) {
    base = cache_base;
  } else {
    auto home = HomeDirectory();
    if (home.empty()) {
      return {};
    }
    base = home / ".cache";
  }
  return base / "Microsoft" / "DeveloperTools" / ".onnxruntime";
#endif
}

std::filesystem::path TelemetryDeviceId::EnsureStorageDirectory() {
#ifdef _WIN32
  return {};
#else
  auto dir = GetStorageDirectory();
  if (dir.empty()) {
    return {};
  }

  if (!CreateDirectoryTreeOwnerOnly(dir)) {
    return {};
  }
  return dir;
#endif
}

std::filesystem::path TelemetryDeviceId::GetCacheDirectory() {
#ifdef _WIN32
  auto base = TelemetryEnvironment::GetEnv("LOCALAPPDATA");
  if (base.empty()) {
    auto user_profile = TelemetryEnvironment::GetEnv("USERPROFILE");
    if (!user_profile.empty()) {
      base = (std::filesystem::path(user_profile) / "AppData" / "Local").string();
    }
  }
  return base.empty() ? std::filesystem::path{} :
                        std::filesystem::path(base) / "Microsoft" / "DeveloperTools" / ".onnxruntime";
#else
  return GetStorageDirectory();
#endif
}

std::filesystem::path TelemetryDeviceId::EnsureCacheDirectory() {
  auto dir = GetCacheDirectory();
  if (dir.empty()) {
    return {};
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return {};
  }
#ifndef _WIN32
  ::chmod(dir.string().c_str(), S_IRWXU);
#endif
  return dir;
}

std::string TelemetryDeviceId::HashForTelemetry(std::string_view raw_device_id) {
  if (raw_device_id.empty()) {
    return {};
  }
  return "c:" + Sha256String(raw_device_id);
}

bool TelemetryDeviceId::IsValidGuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (ch != '-') {
        return false;
      }
      continue;
    }
    const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (!hex) {
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

  if constexpr (kUsePlatformDeviceId) {
    status_ = TelemetryDeviceIdStatus::kPlatform;
    device_id_.clear();
    return;
  }

#ifdef _WIN32
  std::string registry_value;
  bool found = false;
  const auto read_existing = [&]() -> bool {
    if (!ReadWindowsRegistryDeviceId(registry_value, found)) {
      status_ = TelemetryDeviceIdStatus::kFailed;
      return false;
    }

    if (!found) {
      return false;
    }

    registry_value = TrimDeviceId(std::move(registry_value));
    if (registry_value.size() <= kMaxDeviceIdFileSize && IsValidGuid(registry_value)) {
      device_id_ = std::move(registry_value);
      status_ = TelemetryDeviceIdStatus::kExisting;
      return true;
    }
    status_ = TelemetryDeviceIdStatus::kCorrupted;
    return false;
  };

  if (read_existing()) {
    return;
  }

  const bool saw_corruption_before_mutex = status_ == TelemetryDeviceIdStatus::kCorrupted;
  ScopedDeviceIdMutex mutex;
  if (!mutex) {
    if (read_existing()) {
      return;
    }
    status_ = TelemetryDeviceIdStatus::kFailed;
    return;
  }

  status_ = TelemetryDeviceIdStatus::kNew;
  registry_value.clear();
  found = false;
  if (read_existing()) {
    return;
  }
  if (status_ == TelemetryDeviceIdStatus::kFailed) {
    return;
  }

  const bool regenerated_from_corruption =
      saw_corruption_before_mutex || status_ == TelemetryDeviceIdStatus::kCorrupted;
  device_id_ = GenerateGuidV4();
  if (WriteWindowsRegistryDeviceId(device_id_)) {
    status_ = regenerated_from_corruption ? TelemetryDeviceIdStatus::kCorrupted : TelemetryDeviceIdStatus::kNew;
  } else {
    status_ = TelemetryDeviceIdStatus::kFailed;
  }
  return;
#else
  auto dir = GetStorageDirectory();
  if (dir.empty()) {
    status_ = TelemetryDeviceIdStatus::kFailed;
    return;
  }

  const auto file_path = dir / kDeviceIdFileName;
  std::error_code ec;
  if (std::filesystem::is_symlink(file_path, ec)) {
    status_ = TelemetryDeviceIdStatus::kFailed;
    return;
  }
  ec.clear();
  if (std::filesystem::exists(file_path, ec) && !ec) {
    const auto file_size = std::filesystem::file_size(file_path, ec);
    if (!ec && file_size <= kMaxDeviceIdFileSize) {
      std::ifstream input(file_path);
      std::string content;
      std::getline(input, content);
      content = TrimDeviceId(std::move(content));
      if (IsValidGuid(content)) {
        device_id_ = std::move(content);
        status_ = TelemetryDeviceIdStatus::kExisting;
        return;
      }
    }
    status_ = TelemetryDeviceIdStatus::kCorrupted;
  }

  const bool file_existed = status_ == TelemetryDeviceIdStatus::kCorrupted;
  auto storage_dir = EnsureStorageDirectory();
  if (storage_dir.empty()) {
    status_ = TelemetryDeviceIdStatus::kFailed;
    return;
  }

  ec.clear();
  if (std::filesystem::is_symlink(file_path, ec)) {
    status_ = TelemetryDeviceIdStatus::kFailed;
    return;
  }

  device_id_ = GenerateGuidV4();
  const auto publish_result = PublishDeviceIdFileNoFollow(file_path, device_id_, file_existed);
  if (publish_result == DeviceIdPublishResult::kAlreadyExists) {
    ec.clear();
    if (!std::filesystem::is_symlink(file_path, ec)) {
      std::ifstream winner(file_path);
      std::string winner_id;
      if (std::getline(winner, winner_id)) {
        winner_id = TrimDeviceId(std::move(winner_id));
        if (IsValidGuid(winner_id)) {
          device_id_ = std::move(winner_id);
          status_ = TelemetryDeviceIdStatus::kExisting;
          return;
        }
      }
    }
    status_ = TelemetryDeviceIdStatus::kFailed;
    return;
  }

  if (publish_result == DeviceIdPublishResult::kCreated) {
    status_ = file_existed ? TelemetryDeviceIdStatus::kCorrupted : TelemetryDeviceIdStatus::kNew;
  } else {
    status_ = TelemetryDeviceIdStatus::kFailed;
  }
#endif
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

bool TelemetryDeviceId::WriteDeviceIdFile(const std::filesystem::path& path, std::string_view value) {
#ifdef _WIN32
  (void)path;
  (void)value;
  return false;
#else
  const int fd = ::open(path.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return false;
  }
  ::fchmod(fd, S_IRUSR | S_IWUSR);
  const ssize_t written = ::write(fd, value.data(), value.size());
  ::close(fd);
  return written == static_cast<ssize_t>(value.size());
#endif
}

#ifdef _WIN32
bool TelemetryDeviceId::ReadWindowsRegistryDeviceId(std::string& value, bool& found) {
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

bool TelemetryDeviceId::WriteWindowsRegistryDeviceId(std::string_view value) {
  HKEY key = nullptr;
  LSTATUS status = ::RegCreateKeyExA(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0,
                                     KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) {
    return false;
  }

  const auto close_key = [&]() { ::RegCloseKey(key); };
  std::string value_z(value);
  status = ::RegSetValueExA(key, kRegistryValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value_z.c_str()),
                            static_cast<DWORD>(value_z.size() + 1));
  close_key();
  return status == ERROR_SUCCESS;
}
#endif

}  // namespace fl
