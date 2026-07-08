// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Windows implementation of CrossProcessFileLock acquisition and the
// platform-specific lock handle. Cross-platform orchestration
// (WaitForDirectoryLock) lives in src/platform/cross_process_file_lock.cc.
#include "platform/cross_process_file_lock.h"
#include "exception.h"
#include "logger.h"

#include <foundry_local/foundry_local_c.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <process.h>
#include <windows.h>

namespace fl {

namespace {

constexpr const char* kLockFileName = ".download.lock";

/// `PID:<pid>,Time:<iso8601-utc>\n`
std::string FormatProcessInfo() {
  auto pid = static_cast<unsigned long>(_getpid());
  auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_s(&tm, &t);
  std::ostringstream oss;
  oss << "PID:" << pid << ",Time:" << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ") << '\n';
  return oss.str();
}

}  // namespace

// Platform-specific resource handle. The destructor here is the only thing
// that releases the lock; CrossProcessFileLock's destructor is defaulted.
struct CrossProcessFileLock::State {
  HANDLE handle;
  ~State() {
    if (handle != INVALID_HANDLE_VALUE) {
      // FILE_FLAG_DELETE_ON_CLOSE removes the file when the last handle closes.
      CloseHandle(handle);
    }
  }
};

CrossProcessFileLock::CrossProcessFileLock(std::filesystem::path path,
                                           std::unique_ptr<State> state,
                                           ILogger& logger)
    : path_(std::move(path)), state_(std::move(state)), logger_(logger) {}

CrossProcessFileLock::~CrossProcessFileLock() {
  // Release the OS handle first so the "released" log message is accurate.
  state_.reset();
  logger_.Log(LogLevel::Debug, "CrossProcessFileLock released: " + path_.string());
}

std::unique_ptr<CrossProcessFileLock> CrossProcessFileLock::TryAcquireForDirectory(
    const std::filesystem::path& directory, ILogger& logger) {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  // Best-effort: if create_directories failed, the platform open below will
  // surface a clearer error message.

  auto lock_path = directory / kLockFileName;
  std::unique_ptr<State> state;

  // dwShareMode=0 blocks any other open (cross- and in-process) until this
  // handle closes. FILE_FLAG_DELETE_ON_CLOSE pairs OPEN_ALWAYS into a
  // self-cleaning lock that doesn't require unlink-then-close races.
  auto wide = lock_path.wstring();
  HANDLE handle = CreateFileW(wide.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION || err == ERROR_ACCESS_DENIED) {
      // SHARING/LOCK_VIOLATION: another handle already holds the share-none
      // lock. ACCESS_DENIED: the holder is mid-release — FILE_FLAG_DELETE_ON_CLOSE
      // puts the file into STATUS_DELETE_PENDING during the close window, and a
      // concurrent open of a delete-pending file is reported as ACCESS_DENIED.
      // All three mean "another process has it"; treat as contention so the
      // caller retries. (A genuine permission error also lands here and would
      // poll until timeout, but the directory was just created successfully so
      // that is improbable.)
      return nullptr;
    }
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "CreateFileW failed for lock '" + lock_path.string() +
                 "' (GetLastError=" + std::to_string(err) + ")");
  }

  auto info = FormatProcessInfo();
  DWORD written = 0;
  WriteFile(handle, info.data(), static_cast<DWORD>(info.size()), &written, nullptr);
  FlushFileBuffers(handle);

  state = std::unique_ptr<State>(new State{handle});

  logger.Log(LogLevel::Debug, "CrossProcessFileLock acquired: " + lock_path.string());
  return std::unique_ptr<CrossProcessFileLock>(
      new CrossProcessFileLock(std::move(lock_path), std::move(state), logger));
}

}  // namespace fl
