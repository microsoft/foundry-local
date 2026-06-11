// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry.h"

#include <chrono>
#include <string>

namespace fl {

/// RAII tracker for the per-model "Download" event.
/// Caller updates fields as the download progresses; the event is emitted on
/// destruction. Status defaults to kFailure so that abrupt exits (exceptions)
/// are recorded as failures unless the caller calls SetStatus(kSuccess) on the
/// happy path or SetStatus(kSkipped) when the model was already cached.
class DownloadTracker {
 public:
  DownloadTracker(std::string model_id,
                  std::string user_agent,
                  ITelemetry& telemetry);
  ~DownloadTracker();

  // Non-copyable, non-movable
  DownloadTracker(const DownloadTracker&) = delete;
  DownloadTracker& operator=(const DownloadTracker&) = delete;

  void SetStatus(ActionStatus status) { info_.status = status; }
  void SetLockWaitMs(int64_t v) { info_.lock_wait_ms = v; }
  void SetEnumerationMs(int64_t v) { info_.enumeration_ms = v; }
  void SetTotalSizeBytes(int64_t v) { info_.total_size_bytes = v; }
  void SetAlreadyCachedBytes(int64_t v) { info_.already_cached_bytes = v; }
  void SetFileCount(int32_t v) { info_.file_count = v; }
  void SetSkippedFileCount(int32_t v) { info_.skipped_file_count = v; }
  void SetDownloadWaitResult(std::string v) { info_.download_wait_result = std::move(v); }
  void SetMaxConcurrency(int32_t v) { info_.max_concurrency = v; }

  /// Start the timer for the download phase. Use after lock wait and
  /// enumeration are complete, so download_ms only reflects byte transfer.
  void BeginDownloadPhase() { download_phase_start_ = std::chrono::steady_clock::now(); }

  /// Stop the timer for the download phase. The duration is captured into the
  /// emitted event. Safe to call multiple times — the last call wins.
  void EndDownloadPhase() {
    info_.download_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - download_phase_start_)
                            .count();
  }

  void RecordException(const std::exception& exception);

 private:
  ITelemetry& telemetry_;
  DownloadInfo info_;
  std::chrono::steady_clock::time_point download_phase_start_;
};

}  // namespace fl
