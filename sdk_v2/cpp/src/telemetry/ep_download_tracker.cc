// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/ep_download_tracker.h"

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
      user_agent_(std::move(user_agent)),
      correlation_id_(std::move(correlation_id)),
      stage_start_(std::chrono::steady_clock::now()) {
}

EpDownloadTracker::~EpDownloadTracker() {
  // Mirror neutron-server: if the caller didn't reach Done() or
  // RecordRegisterComplete, assume the abrupt exit was an exception path and
  // record any unfinished stage as kFailure.
  RecordEvent(ActionStatus::kFailure);
}

void EpDownloadTracker::RecordInitialState(std::string ready_state) {
  init_ready_state_ = std::move(ready_state);
  stage_ = Stage::Download;
  stage_start_ = std::chrono::steady_clock::now();
}

void EpDownloadTracker::RecordDownloadComplete(ActionStatus status, std::string ready_state) {
  download_duration_ms_ = ElapsedMs(stage_start_);
  download_ready_state_ = std::move(ready_state);
  download_status_ = status;
  stage_ = Stage::Register;
  stage_start_ = std::chrono::steady_clock::now();
}

void EpDownloadTracker::RecordRegisterComplete(ActionStatus status, std::string ready_state) {
  register_duration_ms_ = ElapsedMs(stage_start_);
  register_ready_state_ = std::move(ready_state);
  register_status_ = status;
  stage_ = Stage::Final;
}

void EpDownloadTracker::Done() {
  RecordEvent(ActionStatus::kSkipped);
}

void EpDownloadTracker::RecordException(const std::exception& ex) {
  // The per-provider attempt happens as a consequence of the overall
  // DownloadAndRegisterEps call, so it is indirect and shares its correlation id.
  telemetry_.RecordException(Action::kEpDownloadAndRegister, ex,
                             InvocationContext{user_agent_, correlation_id_, /*indirect=*/true});
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
  info.init_ready_state = init_ready_state_;
  info.download_ready_state = download_ready_state_;
  info.download_status = download_status_;
  info.download_duration_ms = download_duration_ms_;
  info.register_ready_state = register_ready_state_;
  info.register_status = register_status_;
  info.register_duration_ms = register_duration_ms_;
  telemetry_.RecordEpDownloadAndRegister(info);
}

}  // namespace fl
