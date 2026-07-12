// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/cross_process_file_lock.h"
#include "exception.h"
#include "logger.h"

#include <foundry_local/foundry_local_c.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <process.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fl {

namespace {

constexpr const char* kLockFileName = ".download.lock";

/// `PID:<pid>,Time:<iso8601-utc>\n`
std::string FormatProcessInfo() {
#ifdef _WIN32
  auto pid = static_cast<unsigned long>(_getpid());
#else
  auto pid = static_cast<unsigned long>(getpid());
#endif
  auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << "PID:" << pid << ",Time:" << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ") << '\n';
  return oss.str();
}

}  // namespace

// Platform-specific resource handle. The destructor here is the only thing
// that releases the lock; CrossProcessFileLock's destructor is defaulted.
#ifdef _WIN32
struct CrossProcessFileLock::State {
  HANDLE handle;
  ~State() {
    if (handle != INVALID_HANDLE_VALUE) {
      // FILE_FLAG_DELETE_ON_CLOSE removes the file when the last handle closes.
      CloseHandle(handle);
    }
  }
};
#else
struct CrossProcessFileLock::State {
  int fd;
  std::filesystem::path path;
  ~State() {
    if (fd >= 0) {
      // Unlink before close so the file disappears the instant the lock
      // releases; a concurrent acquirer simply recreates it. This is the
      // classic flock()+unlink() pattern, and it is safe here because every
      // acquirer verifies, while holding the flock, that the inode it locked is
      // still the one at `path` (see the fstat/stat check in
      // TryAcquireForDirectory). An acquirer that raced in on the old inode
      // between our unlink and a third party's recreate will see the inode
      // mismatch and retry, so two processes never hold "the lock" at once.
      // There is also no protected work between this unlink and close.
      ::unlink(path.c_str());
      ::close(fd);
    }
  }
};
#endif

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

#ifdef _WIN32
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
#else
  int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "open failed for lock '" + lock_path.string() + "' (errno=" + std::to_string(errno) + ")");
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int err = errno;
    ::close(fd);
    if (err == EWOULDBLOCK || err == EAGAIN) {
      return nullptr;
    }
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "flock failed for '" + lock_path.string() + "' (errno=" + std::to_string(err) + ")");
  }

  // Robust-flock inode check. We now hold an exclusive flock on whatever inode
  // `fd` refers to, but a releaser unlink()s the lock file in its destructor —
  // so between our open() and flock() the path may have been unlinked and a
  // third process may have recreated it. If so, we are holding a lock on an
  // orphaned inode that guards nothing while the live file at `lock_path` is a
  // different inode. Confirm the inode we locked is still the one at the path;
  // if not, drop it and report contention so the caller retries against the
  // live file. This closes the flock()+unlink() orphan-inode race, which is
  // what lets two processes never both believe they hold the lock.
  struct stat fd_stat {};
  struct stat path_stat {};
  if (::fstat(fd, &fd_stat) != 0 || ::stat(lock_path.c_str(), &path_stat) != 0 ||
      fd_stat.st_dev != path_stat.st_dev || fd_stat.st_ino != path_stat.st_ino) {
    ::close(fd);  // releases the flock on the stale / orphaned inode
    return nullptr;
  }

  auto info = FormatProcessInfo();
  // Best-effort: record this process's identity in the lock file for diagnostics.
  // A failure here doesn't affect lock correctness, so it is only logged at Debug.
  if (::ftruncate(fd, 0) != 0 || ::write(fd, info.data(), info.size()) < 0) {
    logger.Log(LogLevel::Debug,
               "CrossProcessFileLock: failed to write diagnostic process info to lock file '" +
                   lock_path.string() + "' (errno=" + std::to_string(errno) + ")");
  }

  state = std::unique_ptr<State>(new State{fd, lock_path});
#endif

  logger.Log(LogLevel::Debug, "CrossProcessFileLock acquired: " + lock_path.string());
  return std::unique_ptr<CrossProcessFileLock>(
      new CrossProcessFileLock(std::move(lock_path), std::move(state), logger));
}

std::unique_ptr<CrossProcessFileLock> CrossProcessFileLock::WaitForDirectoryLock(
    const std::filesystem::path& directory,
    const CancellationPredicate& is_cancelled,
    ILogger& logger,
    std::chrono::milliseconds poll_interval,
    std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  // `is_cancelled` is the caller's progress callback, which also serves as the
  // liveness heartbeat — it emits 0% on every invocation. We therefore poll it
  // on a single cadence (once per `poll_interval`) rather than on a separate
  // fast cancellation tick: a faster tick would spam the user callback (~10x/s)
  // for the entire wait, and cancelling a multi-minute cross-process wait a
  // second sooner is imperceptible. There is no separate cancellation channel
  // to decouple the heartbeat from.
  while (true) {
    if (is_cancelled && is_cancelled()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "lock acquisition cancelled");
    }
    auto lock = CrossProcessFileLock::TryAcquireForDirectory(directory, logger);
    if (lock) {
      return lock;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "timed out waiting for cross-process download lock on '" + directory.string() + "'");
    }
    std::this_thread::sleep_for(poll_interval);
  }
}

}  // namespace fl
