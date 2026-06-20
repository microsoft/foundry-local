// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry.h"
#include "telemetry/telemetry_logger.h"
#include "telemetry/telemetry_metadata.h"
#include "logger.h"

#include <atomic>
#include <memory>
#include <string>

namespace fl {

/// 1DS-backed ITelemetry implementation. Built only when the
/// cpp-client-telemetry vcpkg port is available (find_package(MSTelemetry CONFIG)
/// succeeded and FOUNDRY_LOCAL_USE_TELEMETRY=ON).
///
/// Lifecycle:
///   * Constructor:
///     - If TelemetryEnvironment::IsCiEnvironment() -> CI mode: skip 1DS Initialize
///       entirely. All RecordX calls become no-ops on the 1DS side; the embedded
///       TelemetryLogger still mirrors them to ILogger.
///     - Else if tenant_token is empty -> uninitialized: same behavior as CI mode.
///       Manager normally avoids constructing OneDsTelemetry when the token is
///       empty, but the guard here makes the class robust to misconfiguration.
///     - Else: call MAT::LogManager::Initialize(token), set process-wide common
///       context (app session GUID, version, OS info, app name, test flag).
///
///   * RecordX: builds an EventProperties with the event-specific fields and the
///     `test` bool, then logs via ILogger->LogEvent. Local mirror via TelemetryLogger
///     always runs first so the diagnostic log is preserved regardless of upload.
///
///   * Destructor: MAT::LogManager::FlushAndTeardown so events on disk are pushed
///     before the process exits. Safe to call even when initialization was skipped.
///
/// Thread safety: 1DS LogManager methods are documented as thread-safe. The
/// `initialized_` flag is set once during construction; all other writes happen
/// only in the destructor.
class OneDsTelemetry : public ITelemetry {
 public:
  /// @param tenant_token  1DS ingestion token. Empty disables uploads (CI-equivalent).
  /// @param app_name      Configuration::app_name; stamped as AppName on every event.
  /// @param logger        Diagnostic logger; used by the embedded TelemetryLogger mirror.
  OneDsTelemetry(const std::string& tenant_token,
                 const std::string& app_name,
                 ILogger& logger);
  ~OneDsTelemetry() override;

  // Non-copyable, non-movable (singleton owns LogManager state).
  OneDsTelemetry(const OneDsTelemetry&) = delete;
  OneDsTelemetry& operator=(const OneDsTelemetry&) = delete;

  void RecordAction(Action action, ActionStatus status, const InvocationContext& context,
                    int64_t duration_ms) override;

  void RecordException(Action action, const std::exception& exception,
                       const InvocationContext& context) override;

  void RecordModelUsage(const ModelUsageInfo& info) override;
  void RecordModelId(Action action, const std::string& model_id,
                     ActionStatus status, const InvocationContext& context) override;
  void RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) override;
  void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) override;
  void RecordDownload(const DownloadInfo& info) override;
  void RecordCatalogFetch(const CatalogFetchInfo& info) override;

  /// True if 1DS Initialize succeeded (i.e. events are actually uploaded).
  /// False in CI or when the tenant token was empty.
  bool IsUploadEnabled() const { return initialized_; }

 private:
  TelemetryLogger local_log_;        // Always-on local mirror.
  TelemetryMetadata metadata_;       // Cached at construction.
  std::atomic<bool> initialized_{false};  // True iff MAT::LogManager::Initialize ran.
  ILogger& logger_;
};

}  // namespace fl
