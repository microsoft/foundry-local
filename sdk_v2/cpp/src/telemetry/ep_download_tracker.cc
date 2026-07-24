// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/ep_download_tracker.h"

#include "telemetry/invocation_context.h"

#include <utility>

namespace fl {

namespace {

int64_t ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

EpDownloadTracker::EpDownloadTracker(std::string provider_name,
                                     std::string user_agent,
                                     std::string correlation_id,
                                     ITelemetry& telemetry)
    : telemetry_(telemetry),
      provider_name_(std::move(provider_name)),
      user_agent_(user_agent.empty() ? DefaultUserAgent() : std::move(user_agent)),
      correlation_id_(std::move(correlation_id)),
      stage_start_(std::chrono::steady_clock::now()) {
}

EpDownloadTracker::~EpDownloadTracker() {
  // Mirror neutron-server: if the caller didn't reach Done() or
  // RecordRegisterComplete, assume the abrupt exit was an exception path and
  // record any unfinished stage as kFailure.
  try {
    RecordEvent(ActionStatus::kFailure);
  } catch (...) {
    // Telemetry is best-effort and must not throw from RAII cleanup.
  }
}

void EpDownloadTracker::RecordInitialState(EpReadyState ready_state) {
  init_ready_state_ = ready_state;
  stage_ = Stage::Download;
  stage_start_ = std::chrono::steady_clock::now();
}

void EpDownloadTracker::RecordDownloadComplete(ActionStatus status, EpReadyState ready_state) {
  download_duration_ms_ = ElapsedMs(stage_start_);
  download_ready_state_ = ready_state;
  download_status_ = status;
  stage_ = Stage::Register;
  stage_start_ = std::chrono::steady_clock::now();
}

void EpDownloadTracker::RecordRegisterComplete(ActionStatus status, EpReadyState ready_state) {
  register_duration_ms_ = ElapsedMs(stage_start_);
  register_ready_state_ = ready_state;
  register_status_ = status;
  stage_ = Stage::Final;
}

void EpDownloadTracker::Done() {
  RecordEvent(ActionStatus::kSkipped);
}

void EpDownloadTracker::RecordException(const std::exception& ex) {
  const auto status = ActionStatusFromException(ex);
  // The per-provider attempt happens as a consequence of the overall
  // DownloadAndRegisterEps call, so it is indirect and shares its correlation id.
  try {
    telemetry_.RecordException(Action::kEpDownloadAndRegister, ex,
                               InvocationContext{user_agent_, correlation_id_, /*indirect=*/true});
  } catch (...) {
    // Telemetry is best-effort and must not mask the original error path.
  }
  RecordEvent(status);
}

void EpDownloadTracker::RecordEvent(ActionStatus incomplete_stage_status) {
  if (recorded_event_) {
    return;
  }
  recorded_event_ = true;

  if (stage_ == Stage::Download) {
    download_duration_ms_ = ElapsedMs(stage_start_);
    download_status_ = incomplete_stage_status;
  } else if (stage_ == Stage::Register) {
    register_duration_ms_ = ElapsedMs(stage_start_);
    register_status_ = incomplete_stage_status;
  }

  EpDownloadAndRegisterInfo info;
  info.user_agent = user_agent_;
  info.correlation_id = correlation_id_;
  info.provider_name = provider_name_;
  info.init_ready_state = ReadyStateToString(init_ready_state_);
  info.download_ready_state = ReadyStateToString(download_ready_state_);
  info.download_status = download_status_;
  info.download_duration_ms = download_duration_ms_;
  info.register_ready_state = ReadyStateToString(register_ready_state_);
  info.register_status = register_status_;
  info.register_duration_ms = register_duration_ms_;
  telemetry_.RecordEpDownloadAndRegister(info);
}

const char* EpDownloadTracker::ReadyStateToString(EpReadyState state) {
  switch (state) {
    case EpReadyState::kNotApplicable:
      return "N/A";
    case EpReadyState::kNotPresent:
      return "NotPresent";
    case EpReadyState::kInstalled:
      return "Installed";
    case EpReadyState::kRegistered:
      return "Registered";
    case EpReadyState::kUnknown:
      return "Unknown";
    default:
      return "Unknown";
  }
}

}  // namespace fl
