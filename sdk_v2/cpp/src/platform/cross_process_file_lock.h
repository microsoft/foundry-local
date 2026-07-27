// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace fl {

class ILogger;

/// RAII exclusive lock backed by an OS-level file lock on
/// `<directory>/.download.lock`. Serializes model downloads across processes
/// that share a cache directory. A crash while holding the lock may leave a
/// zero-byte file behind; the next acquirer reopens and re-locks, so the leak
/// is harmless.
///
/// This is deliberately a separate primitive from `fl::FileLock`
/// (util/file_lock.h), the simple blocking RAII lock the EP bootstrappers use.
/// Model downloads are long and cancellable, so they need non-blocking
/// acquisition (TryAcquireForDirectory), cancellation-aware polling with a
/// timeout (WaitForDirectoryLock), and protection against the flock()+unlink()
/// orphan-inode race — none of which FileLock offers. A future change could
/// consolidate the two if FileLock grows these capabilities.
class CrossProcessFileLock {
 public:
  /// Returning true aborts WaitForDirectoryLock with FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED.
  using CancellationPredicate = std::function<bool()>;

  /// Non-blocking acquisition. Returns nullptr if another process currently
  /// holds the lock. Creates `directory` if missing. Throws fl::Exception on
  /// unexpected errors (permission denied, etc.). `logger` receives acquire/
  /// release diagnostics and is required — callers always have one.
  static std::unique_ptr<CrossProcessFileLock> TryAcquireForDirectory(
      const std::filesystem::path& directory,
      ILogger& logger);

  /// Polls TryAcquireForDirectory until the lock is acquired, `is_cancelled()`
  /// returns true, or `timeout` elapses.
  /// Throws FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED on cancellation, or
  /// FOUNDRY_LOCAL_ERROR_INTERNAL on timeout.
  static std::unique_ptr<CrossProcessFileLock> WaitForDirectoryLock(
      const std::filesystem::path& directory,
      const CancellationPredicate& is_cancelled,
      ILogger& logger,
      std::chrono::milliseconds poll_interval = std::chrono::milliseconds{1250},
      std::chrono::milliseconds timeout = std::chrono::hours{3});

  ~CrossProcessFileLock();

  CrossProcessFileLock(const CrossProcessFileLock&) = delete;
  CrossProcessFileLock& operator=(const CrossProcessFileLock&) = delete;
  CrossProcessFileLock(CrossProcessFileLock&&) = delete;
  CrossProcessFileLock& operator=(CrossProcessFileLock&&) = delete;

  /// Path to the lock file (for diagnostics / tests).
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  struct State;  // Platform-specific; defined in the .cc.

  CrossProcessFileLock(std::filesystem::path path, std::unique_ptr<State> state, ILogger& logger);

  std::filesystem::path path_;
  std::unique_ptr<State> state_;
  ILogger& logger_;
};

}  // namespace fl
