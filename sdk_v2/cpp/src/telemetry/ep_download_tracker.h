// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry.h"

#include <chrono>
#include <exception>
#include <string>

namespace fl {

/// RAII tracker for the per-provider "EPDownloadAndRegister" event. Mirrors the
/// neutron-server EPDownloadTracker. Stages advance Initial -> Download ->
/// Register -> Final. Each stage that the caller doesn't explicitly complete
/// is recorded with one of:
///   * kFailure on destruction (assumed exception path)
///   * kSkipped if Done() is called before destruction (early-exit without
///     exception, e.g. "EP already registered, nothing to download")
class EpDownloadTracker {
 public:
  EpDownloadTracker(std::string provider_name,
                    std::string user_agent,
                    std::string correlation_id,
                    ITelemetry& telemetry);
  ~EpDownloadTracker();

  // Non-copyable, non-movable
  EpDownloadTracker(const EpDownloadTracker&) = delete;
  EpDownloadTracker& operator=(const EpDownloadTracker&) = delete;

  /// Captures the EP's ready state before the download phase begins. Restarts
  /// the stopwatch so subsequent timings measure the download phase only.
  void RecordInitialState(std::string ready_state = "N/A");

  /// Captures the download phase outcome and ready state, restarts the
  /// stopwatch for the register phase.
  void RecordDownloadComplete(ActionStatus status, std::string ready_state = "N/A");

  /// Captures the register phase outcome and ready state. Stops the stopwatch.
  void RecordRegisterComplete(ActionStatus status, std::string ready_state = "N/A");

  /// Mark tracking as complete, filling any remaining stages with kSkipped
  /// instead of the default kFailure (exception-assumed) status. Call this on
  /// happy-path early exits (e.g. "EP already registered").
  void Done();

  /// Record an exception associated with the bootstrap operation. Does not
  /// finalize the EPDownloadAndRegister event — that happens on destruction.
  void RecordException(const std::exception& ex);

 private:
  enum class Stage {
    Initial = 0,
    Download = 1,
    Register = 2,
    Final = 3,
  };

  void RecordEvent(ActionStatus incomplete_stage_status);

  ITelemetry& telemetry_;
  std::string provider_name_;
  std::string user_agent_;
  std::string correlation_id_;
  std::string init_ready_state_ = "N/A";
  std::string download_ready_state_ = "N/A";
  std::string register_ready_state_ = "N/A";
  ActionStatus download_status_ = ActionStatus::kSkipped;
  ActionStatus register_status_ = ActionStatus::kSkipped;
  int64_t download_duration_ms_ = 0;
  int64_t register_duration_ms_ = 0;
  std::chrono::steady_clock::time_point stage_start_;
  Stage stage_ = Stage::Initial;
  bool recorded_event_ = false;
};

}  // namespace fl
