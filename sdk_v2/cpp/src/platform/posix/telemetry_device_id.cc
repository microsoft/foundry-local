// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "platform/telemetry_device_id.h"

#include "telemetry/invocation_context.h"
#include "telemetry/telemetry_environment.h"
#include "util/file_lock.h"

#include <cerrno>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace fl::TelemetryDeviceIdPlatform {

namespace {

constexpr size_t kMaxDeviceIdSize = 256;
constexpr const char* kDeviceIdFileName = "deviceid";

std::filesystem::path HomeDirectory() {
  const auto home = TelemetryEnvironment::GetEnv("HOME");
  return home.empty() ? std::filesystem::path{} : std::filesystem::path(home);
}

std::string TrimDeviceId(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
    value.pop_back();
  }
  return value;
}

std::optional<std::string> ReadValidDeviceId(const std::filesystem::path& file) {
  std::error_code error;
  const auto file_size = std::filesystem::file_size(file, error);
  if (error || file_size > kMaxDeviceIdSize) {
    return std::nullopt;
  }

  std::ifstream input(file);
  std::string content;
  if (!std::getline(input, content)) {
    return std::nullopt;
  }

  content = TrimDeviceId(std::move(content));
  return TelemetryDeviceId::IsValidGuid(content) ? std::optional<std::string>{std::move(content)} : std::nullopt;
}

bool CreateDirectoryTreeOwnerOnly(const std::filesystem::path& directory, bool leaf = true) {
  if (directory.empty()) {
    return false;
  }

  std::error_code error;
  if (leaf && std::filesystem::is_symlink(directory, error)) {
    return false;
  }

  error.clear();
  if (std::filesystem::exists(directory, error)) {
    if (!std::filesystem::is_directory(directory, error)) {
      return false;
    }
    if (leaf) {
      std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace, error);
      if (error) {
        return false;
      }
    }
    return true;
  }

  const auto parent = directory.parent_path();
  if (!parent.empty() && parent != directory && !CreateDirectoryTreeOwnerOnly(parent, false)) {
    return false;
  }

  if (::mkdir(directory.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
    return false;
  }

  error.clear();
  if ((leaf && std::filesystem::is_symlink(directory, error)) ||
      !std::filesystem::is_directory(directory, error)) {
    return false;
  }
  if (leaf) {
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
      return false;
    }
  }
  return true;
}

enum class PublishResult {
  kCreated,
  kAlreadyExists,
  kFailed,
};

PublishResult PublishDeviceId(const std::filesystem::path& file, std::string_view value, bool replace_existing) {
  std::filesystem::path temporary = file;
  temporary += ".tmp." + GenerateGuidV4();

  int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  const int descriptor = ::open(temporary.c_str(), flags, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    return PublishResult::kFailed;
  }

  bool wrote = true;
  const char* data = value.data();
  size_t remaining = value.size();
  while (remaining > 0) {
    const ssize_t count = ::write(descriptor, data, remaining);
    if (count <= 0) {
      wrote = false;
      break;
    }
    data += count;
    remaining -= static_cast<size_t>(count);
  }
  if (::close(descriptor) != 0) {
    wrote = false;
  }

  std::error_code error;
  if (!wrote) {
    std::filesystem::remove(temporary, error);
    return PublishResult::kFailed;
  }

  if (replace_existing) {
    std::filesystem::rename(temporary, file, error);
  } else {
    std::filesystem::create_hard_link(temporary, file, error);
  }
  if (error) {
    const bool already_exists = !replace_existing && error == std::errc::file_exists;
    std::filesystem::remove(temporary, error);
    return already_exists ? PublishResult::kAlreadyExists : PublishResult::kFailed;
  }
  std::filesystem::remove(temporary, error);

  error.clear();
  std::filesystem::permissions(file,
                               std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, error);
  return PublishResult::kCreated;
}

}  // namespace

bool UsesPlatformProvidedId() {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  return true;
#else
  return false;
#endif
}

std::filesystem::path GetStorageDirectory() {
#ifdef __APPLE__
  const auto home = HomeDirectory();
  return home.empty()
             ? std::filesystem::path{}
             : home / "Library" / "Application Support" / "Microsoft" / "DeveloperTools" / ".onnxruntime";
#else
  auto cache_base = TelemetryEnvironment::GetEnv("XDG_CACHE_HOME");
  std::filesystem::path base;
  if (!cache_base.empty()) {
    base = cache_base;
  } else {
    const auto home = HomeDirectory();
    if (home.empty()) {
      return {};
    }
    base = home / ".cache";
  }
  return base / "Microsoft" / "DeveloperTools" / ".onnxruntime";
#endif
}

std::filesystem::path EnsureStorageDirectory() {
  const auto directory = GetStorageDirectory();
  return !directory.empty() && CreateDirectoryTreeOwnerOnly(directory) ? directory : std::filesystem::path{};
}

std::filesystem::path GetCacheDirectory() {
  return GetStorageDirectory();
}

std::filesystem::path EnsureCacheDirectory() {
  const auto directory = GetCacheDirectory();
  if (directory.empty()) {
    return {};
  }

  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || ::chmod(directory.c_str(), S_IRWXU) != 0) {
    return {};
  }
  return directory;
}

LoadResult LoadOrCreate() {
  const auto directory = GetStorageDirectory();
  if (directory.empty()) {
    return {{}, TelemetryDeviceIdStatus::kFailed};
  }

  const auto file = directory / kDeviceIdFileName;
  std::error_code error;
  if (std::filesystem::is_symlink(file, error)) {
    return {{}, TelemetryDeviceIdStatus::kFailed};
  }

  TelemetryDeviceIdStatus status = TelemetryDeviceIdStatus::kNew;
  error.clear();
  if (std::filesystem::exists(file, error) && !error) {
    if (auto content = ReadValidDeviceId(file)) {
      return {std::move(*content), TelemetryDeviceIdStatus::kExisting};
    }
    status = TelemetryDeviceIdStatus::kCorrupted;
  }

  const bool file_existed = status == TelemetryDeviceIdStatus::kCorrupted;
  if (EnsureStorageDirectory().empty()) {
    return {{}, TelemetryDeviceIdStatus::kFailed};
  }

  error.clear();
  if (std::filesystem::is_symlink(file, error)) {
    return {{}, TelemetryDeviceIdStatus::kFailed};
  }

  std::unique_ptr<FileLock> recovery_lock;
  if (file_existed) {
    try {
      recovery_lock = std::make_unique<FileLock>(directory / "deviceid.lock");
    } catch (...) {
      return {{}, TelemetryDeviceIdStatus::kFailed};
    }

    error.clear();
    if (std::filesystem::is_symlink(file, error)) {
      return {{}, TelemetryDeviceIdStatus::kFailed};
    }
    if (auto winner = ReadValidDeviceId(file)) {
      return {std::move(*winner), TelemetryDeviceIdStatus::kExisting};
    }
  }

  auto value = GenerateGuidV4();
  const auto publish_result = PublishDeviceId(file, value, file_existed);
  if (publish_result == PublishResult::kAlreadyExists) {
    error.clear();
    if (!std::filesystem::is_symlink(file, error)) {
      if (auto winner = ReadValidDeviceId(file)) {
        return {std::move(*winner), TelemetryDeviceIdStatus::kExisting};
      }
    }
    return {{}, TelemetryDeviceIdStatus::kFailed};
  }

  return publish_result == PublishResult::kCreated
             ? LoadResult{std::move(value), file_existed ? TelemetryDeviceIdStatus::kCorrupted
                                                         : TelemetryDeviceIdStatus::kNew}
             : LoadResult{{}, TelemetryDeviceIdStatus::kFailed};
}

}  // namespace fl::TelemetryDeviceIdPlatform
