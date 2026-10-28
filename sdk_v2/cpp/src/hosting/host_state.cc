// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/host_state.h"

#include "exception.h"
#include "hosting/bootstrap_descriptor.h"
#include "util/scope_guard.h"
#include "util/sha256.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "hosting/windows_user.h"
#include <aclapi.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#endif
#endif

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

bool IsHex(const std::string& value, size_t length) {
  return value.size() == length && std::all_of(value.begin(), value.end(),
                                               [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); });
}

ProcessIdentity DecodeProcess(const Json& value) {
  if (!value.at("pid").is_number_unsigned() || !value.at("generation").is_number_unsigned()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid native host process record");
  }

  ProcessIdentity process{value.at("pid").get<uint64_t>(), value.at("generation").get<uint64_t>()};
  if (!process.pid || !process.generation) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host process record has an empty identity");
  }

  return process;
}

std::filesystem::path ProcessPath(const std::filesystem::path& bootstrap) {
  auto path = bootstrap;
  path += ".process";
  return path;
}

std::filesystem::path ReadyPath(const std::filesystem::path& bootstrap) {
  auto path = bootstrap;
  path += ".ready";
  return path;
}

#ifdef _WIN32

std::string UserKey() {
  std::string text;
  for (const auto c : windows::UserSidText()) {
    text += static_cast<char>(c);
  }

  return Sha256String(text);
}

std::filesystem::path DefaultRoot() {
  const auto key = UserKey().substr(0, 16);
  PWSTR raw = nullptr;
  const auto error = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
  if (FAILED(error)) {
    throw std::system_error(static_cast<int>(error), std::system_category(), "resolve native host user directory");
  }

  ScopeGuard release([&]() noexcept { CoTaskMemFree(raw); });
  return std::filesystem::path(raw) / ("foundry-native-host-" + key);
}

void VerifyDirectory(const std::filesystem::path& path) {
  const std::unique_ptr<void, decltype(&CloseHandle)> handle(
      CreateFileW(path.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr),
      &CloseHandle);
  windows::Check(handle.get() != INVALID_HANDLE_VALUE, "open native host state directory");
  BY_HANDLE_FILE_INFORMATION info{};
  windows::Check(GetFileInformationByHandle(handle.get(), &info) != FALSE, "inspect native host state directory");
  if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host state must be a non-reparse directory");
  }

  PSECURITY_DESCRIPTOR raw = nullptr;
  PSID owner = nullptr;
  PACL acl = nullptr;
  const auto error =
      GetSecurityInfo(handle.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                      nullptr, &acl, nullptr, &raw);
  if (error != ERROR_SUCCESS) {
    throw std::system_error(static_cast<int>(error), std::system_category(), "inspect native host state permissions");
  }

  const windows::LocalMemory security(raw);
  const auto user = windows::ProcessUser(GetCurrentProcess());
  if (!owner || !EqualSid(owner, windows::UserSid(user)) || !acl) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host state is not owned and restricted to the current user");
  }
  for (DWORD i = 0; i < acl->AceCount; ++i) {
    void* raw_ace = nullptr;
    windows::Check(GetAce(acl, i, &raw_ace) != FALSE, "inspect native host state ACL");
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host state has an unsupported ACL");
    }

    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    if (ace->Mask && !EqualSid(const_cast<DWORD*>(&ace->SidStart), windows::UserSid(user))) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host state grants access to another user");
    }
  }
}

void EnsureDirectory(const std::filesystem::path& path) {
  const auto descriptor = windows::OwnerSecurity(true);
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
  if (!CreateDirectoryW(path.c_str(), &security) && GetLastError() != ERROR_ALREADY_EXISTS) {
    windows::Check(false, "create native host state directory");
  }

  VerifyDirectory(path);
}

#else

[[noreturn]] void FileFailure(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

std::filesystem::path DefaultRoot() {
  return std::filesystem::canonical("/tmp") / ("foundry-host-" + std::to_string(geteuid()));
}

void VerifyDirectory(const std::filesystem::path& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    FileFailure("open native host state directory");
  }

  ScopeGuard close_fd([&]() noexcept { close(fd); });
  struct stat info{};
  if (fstat(fd, &info) != 0) {
    FileFailure("inspect native host state directory");
  }
  if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0077) != 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host state must be an owner-only directory");
  }
#ifdef __APPLE__
  const auto acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
  if (acl) {
    ScopeGuard release([&]() noexcept { acl_free(acl); });
    acl_entry_t entry{};
    const auto result = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
    if (result == 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host state must not have an extended ACL");
    }
    if (errno != EINVAL) {
      FileFailure("inspect native host state ACL entry");
    }
  } else if (errno != ENOENT) {
    FileFailure("inspect native host state ACL");
  }
#endif
}

void EnsureDirectory(const std::filesystem::path& path) {
  if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    FileFailure("create native host state directory");
  }

  VerifyDirectory(path);
}

#endif

}  // namespace

struct HostStartupLock::State {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~State() {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
#else
  int fd = -1;
  ~State() {
    if (fd >= 0) {
      close(fd);
    }
  }
#endif
};

HostStartupLock::HostStartupLock(const std::filesystem::path& directory, std::chrono::steady_clock::time_point deadline)
    : state_(std::make_unique<State>()) {
  VerifyDirectory(directory);
  const auto path = directory / "startup.lock";
#ifdef _WIN32
  const auto descriptor = windows::OwnerSecurity();
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
  state_->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &security, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  windows::Check(state_->handle != INVALID_HANDLE_VALUE, "open native host startup lock");
  BY_HANDLE_FILE_INFORMATION info{};
  windows::Check(GetFileInformationByHandle(state_->handle, &info) != FALSE, "inspect native host startup lock");
  if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host startup lock must be a regular file");
  }
#else
  state_->fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (state_->fd < 0) {
    FileFailure("open native host startup lock");
  }

  struct stat info{};
  if (fstat(state_->fd, &info) != 0) {
    FileFailure("inspect native host startup lock");
  }
  if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0077) != 0 || info.st_nlink != 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host startup lock is not an owner-only regular file");
  }
#endif
  for (;;) {
#ifdef _WIN32
    OVERLAPPED operation{};
    if (LockFileEx(state_->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &operation)) {
      return;
    }
    if (GetLastError() != ERROR_LOCK_VIOLATION) {
      windows::Check(false, "lock native host startup");
    }
#else
    if (flock(state_->fd, LOCK_EX | LOCK_NB) == 0) {
      return;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
      FileFailure("lock native host startup");
    }
#endif
    if (std::chrono::steady_clock::now() >= deadline) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "timed out waiting for native host startup serialization");
    }

    std::this_thread::sleep_for(5ms);
  }
}

HostStartupLock::~HostStartupLock() = default;

HostStateStore::HostStateStore(std::string profile_key, std::filesystem::path root)
    : profile_key_(std::move(profile_key)), root_(root.empty() ? DefaultRoot() : std::move(root)) {
  if (!IsHex(profile_key_, 64) || !root_.is_absolute()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid native host state location/profile");
  }

#ifdef _WIN32
  const auto native = root_.lexically_normal().native();
  if (!native.starts_with(LR"(\\?\)")) {
    root_ = native.starts_with(LR"(\\)") ? LR"(\\?\UNC\)" + native.substr(2) : LR"(\\?\)" + native;
  }
#endif
  directory_ = root_ / profile_key_;
}

bool HostStateStore::Exists() const {
  if (!std::filesystem::exists(root_)) {
    return false;
  }

  VerifyDirectory(root_);
  if (!std::filesystem::exists(directory_)) {
    return false;
  }

  VerifyDirectory(directory_);
  return true;
}

std::unique_ptr<HostStartupLock> HostStateStore::Lock(std::chrono::steady_clock::time_point deadline) const {
  EnsureDirectory(root_);
  EnsureDirectory(directory_);
  return std::make_unique<HostStartupLock>(directory_, deadline);
}

std::filesystem::path HostStateStore::Endpoint() const {
#ifdef _WIN32
  const auto user_key = UserKey();
  return LR"(\\.\pipe\foundry-native-)" + std::wstring(profile_key_.begin(), profile_key_.end()) + L"-" +
         std::wstring(user_key.begin(), user_key.end());
#else
  return directory_ / "s";
#endif
}

void HostStateStore::Validate(const HostLaunchRecord& record) const {
  static_cast<void>(DecodeChildHostRole(EncodeChildHostRole(record.child)));
  if (record.child.profile_key != profile_key_ || record.child.endpoint != Endpoint() ||
      !IsHex(record.child.incarnation, 32) ||
      (record.process && (!record.process->pid || !record.process->generation))) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host launch record does not match its private profile");
  }
}

std::optional<HostLaunchRecord> HostStateStore::Read() const {
  const auto path = directory_ / "launch.json";
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }

  const auto value = ReadBootstrapJson(path);
  if (value.at("version") != 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported native host launch record");
  }

  HostLaunchRecord record{DecodeChildHostRole(value.at("child").dump()), std::nullopt};
  if (!value.at("process").is_null()) {
    record.process = DecodeProcess(value.at("process"));
  }

  Validate(record);
  return record;
}

void HostStateStore::Write(const HostLaunchRecord& record) const {
  Validate(record);
  Json value{{"version", 1}, {"child", Json::parse(EncodeChildHostRole(record.child))}, {"process", nullptr}};
  if (record.process) {
    value["process"] = {{"pid", record.process->pid}, {"generation", record.process->generation}};
  }

  WriteHostJson(directory_ / "launch.json", value);
}

std::filesystem::path HostStateStore::BootstrapPath(const HostLaunchRecord& record) const {
  Validate(record);
  return directory_ / ("bootstrap-" + record.child.incarnation + ".json");
}

std::optional<ProcessIdentity> HostStateStore::ReadPublishedProcess(const HostLaunchRecord& record) const {
  const auto path = ProcessPath(BootstrapPath(record));
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }

  const auto value = ReadBootstrapJson(path);
  if (value.at("version") != 1 || value.at("profile") != profile_key_ ||
      value.at("incarnation") != record.child.incarnation) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host process publication does not match its launch");
  }

  return DecodeProcess(value.at("process"));
}

std::string NewHostIncarnation() {
  std::random_device random;
  constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  for (size_t i = 0; i < 4; ++i) {
    const auto value = random();
    for (int shift = 28; shift >= 0; shift -= 4) {
      result += digits[(value >> shift) & 15];
    }
  }

  return result;
}

void HostStateStore::ForgetExited(const HostLaunchRecord& record) const {
  Validate(record);
  if (!record.process) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot retire a launch with an unknown child process");
  }
#ifndef _WIN32
  const auto endpoint = Endpoint();
  struct stat socket{};
  if (lstat(endpoint.c_str(), &socket) == 0) {
    const auto ready = ReadBootstrapJson(ReadyPath(BootstrapPath(record)));
    if (ready.at("version") != 1 || ready.at("profile") != profile_key_ ||
        ready.at("incarnation") != record.child.incarnation || DecodeProcess(ready.at("process")) != *record.process ||
        !ready.at("device").is_number_unsigned() || !ready.at("inode").is_number_unsigned() ||
        ready.at("device").get<uint64_t>() != static_cast<uint64_t>(socket.st_dev) ||
        ready.at("inode").get<uint64_t>() != static_cast<uint64_t>(socket.st_ino) || !S_ISSOCK(socket.st_mode) ||
        socket.st_uid != geteuid() || (socket.st_mode & 0777) != 0600) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "stale native host socket does not match the exited incarnation");
    }

    if (unlink(endpoint.c_str()) != 0) {
      FileFailure("retire native host socket");
    }
  } else if (errno != ENOENT) {
    FileFailure("inspect stale native host socket");
  }
#endif
  const auto bootstrap = BootstrapPath(record);
  std::filesystem::remove(ProcessPath(bootstrap));
  std::filesystem::remove(ReadyPath(bootstrap));
  std::filesystem::remove(bootstrap);
  std::filesystem::remove(directory_ / "launch.json");
}

void HostStateStore::ForgetRejected(const HostLaunchRecord& record) const {
  Validate(record);
  if (record.child.starter != CurrentProcessIdentity() || record.process || ReadPublishedProcess(record)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot discard a native host launch with a possible child");
  }

  std::filesystem::remove(BootstrapPath(record));
  std::filesystem::remove(directory_ / "launch.json");
}

void WriteHostJson(const std::filesystem::path& path, const Json& value) {
  const auto bytes = value.dump();
  if (bytes.size() > 1024 * 1024) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host state exceeds its size limit");
  }

  auto temporary = path;
  temporary += "." + NewHostIncarnation() + ".tmp";
  bool owns_temporary = false;
  ScopeGuard cleanup([&]() noexcept {
    if (owns_temporary) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
    }
  });
#ifdef _WIN32
  const auto descriptor = windows::OwnerSecurity();
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
  std::unique_ptr<void, decltype(&CloseHandle)> file(
      CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security, CREATE_NEW,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr),
      &CloseHandle);
  windows::Check(file.get() != INVALID_HANDLE_VALUE, "create native host state");
  owns_temporary = true;
  DWORD written{};
  windows::Check(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
                     written == bytes.size(),
                 "write native host state");
  windows::Check(FlushFileBuffers(file.get()) != FALSE, "flush native host state");
  CloseHandle(file.release());
  windows::Check(
      MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
      "publish native host state");
#else
  const int fd = open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    FileFailure("create native host state");
  }

  owns_temporary = true;
  ScopeGuard close_fd([&]() noexcept { close(fd); });
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      FileFailure("write native host state");
    }

    offset += static_cast<size_t>(count);
  }
  if (fsync(fd) != 0 || rename(temporary.c_str(), path.c_str()) != 0) {
    FileFailure("publish native host state");
  }

  const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) {
    FileFailure("open native host publication directory");
  }

  ScopeGuard close_directory([&]() noexcept { close(directory); });
  if (fsync(directory) != 0) {
    FileFailure("flush native host publication directory");
  }
#endif
}

void PublishHostProcess(const std::filesystem::path& bootstrap_path, const ChildHostRole& child,
                        ProcessIdentity process) {
  if (!process.pid || !process.generation) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "cannot publish an empty native host process identity");
  }

  WriteHostJson(ProcessPath(bootstrap_path), {{"version", 1},
                                              {"profile", child.profile_key},
                                              {"incarnation", child.incarnation},
                                              {"process", {{"pid", process.pid}, {"generation", process.generation}}}});
}

void PublishHostReady(const std::filesystem::path& bootstrap_path, const ChildHostRole& child,
                      ProcessIdentity process) {
  Json value{{"version", 1},
             {"profile", child.profile_key},
             {"incarnation", child.incarnation},
             {"process", {{"pid", process.pid}, {"generation", process.generation}}}};
#ifndef _WIN32
  struct stat socket{};
  if (lstat(child.endpoint.c_str(), &socket) != 0) {
    FileFailure("inspect native host listening socket");
  }
  if (!S_ISSOCK(socket.st_mode) || socket.st_uid != geteuid() || (socket.st_mode & 0777) != 0600) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host listening socket is not owner-restricted");
  }

  value["device"] = static_cast<uint64_t>(socket.st_dev);
  value["inode"] = static_cast<uint64_t>(socket.st_ino);
#endif
  WriteHostJson(ReadyPath(bootstrap_path), value);
}

}  // namespace fl::hosting
