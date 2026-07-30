// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/download_tracker.h"

#include "telemetry/invocation_context.h"

#include <utility>

namespace fl {

DownloadTracker::DownloadTracker(std::string model_id,
                                 std::string user_agent,
                                 ITelemetry& telemetry)
    : telemetry_(telemetry) {
  info_.model_id = std::move(model_id);
  info_.user_agent = user_agent.empty() ? DefaultUserAgent() : std::move(user_agent);
  info_.correlation_id = GenerateGuidV4();
  info_.status = ActionStatus::kFailure;
  download_phase_start_ = std::chrono::steady_clock::now();
}

DownloadTracker::~DownloadTracker() {
  // Emit the Download event regardless of outcome. The default status is
  // kFailure so abrupt exits (exceptions) are recorded as failures.
  try {
    telemetry_.RecordDownload(info_);
  } catch (...) {
    // Telemetry is best-effort and must not throw from RAII cleanup.
  }
}

void DownloadTracker::RecordException(const std::exception& exception) {
  info_.status = ActionStatusFromException(exception);
  try {
    telemetry_.RecordException(Action::kModelFileDownload, exception,
                               InvocationContext{info_.user_agent, info_.correlation_id, /*indirect=*/false});
  } catch (...) {
    // Telemetry is best-effort and must not mask the original error path.
  }
}

}  // namespace fl
