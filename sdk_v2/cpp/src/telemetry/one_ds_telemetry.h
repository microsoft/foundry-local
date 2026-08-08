// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry.h"
#include "telemetry/telemetry_logger.h"
#include "telemetry/telemetry_metadata.h"
#include "logger.h"

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>

namespace fl {

/// 1DS-backed ITelemetry implementation. Manager config can suppress non-essential uploads while still allowing
/// ProcessInfo; CI and unit-test processes suppress upload entirely.
class OneDsTelemetry : public ITelemetry {
 public:
  /// @param app_name      Configuration::app_name; stamped as AppName on every event.
  /// @param logger        Diagnostic logger; used by the embedded TelemetryLogger mirror.
  /// @param disable_nonessential_telemetry  When true, non-essential uploads are suppressed; ProcessInfo still
  ///                           uploads and events are still written to the local diagnostic logger.
  OneDsTelemetry(const std::string& app_name,
                 ILogger& logger,
                 bool disable_nonessential_telemetry = false);
  ~OneDsTelemetry() override;

  // Non-copyable, non-movable.
  OneDsTelemetry(const OneDsTelemetry&) = delete;
  OneDsTelemetry& operator=(const OneDsTelemetry&) = delete;

  void RecordAction(Action action, ActionStatus status, const InvocationContext& context,
                    int64_t duration_ms, const std::string& model_id = {}) override;

  void RecordException(Action action, const std::exception& exception,
                       const InvocationContext& context) override;

  void RecordModelUsage(const ModelUsageInfo& info) override;
  void RecordAudioUsage(const AudioUsageInfo& info) override;
  void RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) override;
  void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) override;
  void RecordDownload(const DownloadInfo& info) override;
  void RecordCatalogFetch(const CatalogFetchInfo& info) override;
  void RecordProcessInfo(const ProcessInfo& info) override;
  void RecordHardwareInfo(const HardwareInfo& info) override;
  void StartSession() override;
  void EndSession() override;

  /// True if 1DS Initialize succeeded and non-essential uploads are enabled.
  /// ProcessInfo may still upload when disable_nonessential_telemetry suppresses usage events.
  bool IsUploadEnabled() const {
    return initialized_.load(std::memory_order_acquire) && upload_enabled_.load(std::memory_order_acquire);
  }

 private:
  struct Impl;

  std::shared_lock<std::shared_mutex> LockForLogging(bool require_upload = true) const;

  TelemetryLogger local_log_;
  TelemetryMetadata metadata_;       // Cached at construction.
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> upload_enabled_{true};  // False when non-essential uploads are suppressed.
  mutable std::shared_mutex mutex_;  // Serializes logging calls with teardown.
  ILogger& logger_;
};

}  // namespace fl
