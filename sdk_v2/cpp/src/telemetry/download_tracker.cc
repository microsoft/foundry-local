// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/download_tracker.h"

#include <utility>

namespace fl {

DownloadTracker::DownloadTracker(std::string model_id,
                                 std::string user_agent,
                                 ITelemetry& telemetry)
    : telemetry_(telemetry) {
  info_.model_id = std::move(model_id);
  info_.user_agent = std::move(user_agent);
  info_.status = ActionStatus::kFailure;
  download_phase_start_ = std::chrono::steady_clock::now();
}

DownloadTracker::~DownloadTracker() {
  // Emit the Download event regardless of outcome. The default status is
  // kFailure so abrupt exits (exceptions) are recorded as failures.
  telemetry_.RecordDownload(info_);
}

void DownloadTracker::RecordException(const std::exception& exception) {
  telemetry_.RecordException(Action::kModelFileDownload, exception, info_.user_agent);
}

}  // namespace fl
