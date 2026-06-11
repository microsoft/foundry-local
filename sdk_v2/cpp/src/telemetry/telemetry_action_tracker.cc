// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry_action_tracker.h"

namespace fl {

ActionTracker::ActionTracker(Action action, ITelemetry& telemetry, const std::string& user_agent, bool indirect)
    : action_(action),
      telemetry_(telemetry),
      user_agent_(user_agent),
      indirect_(indirect),
      start_(std::chrono::steady_clock::now()) {
}

ActionTracker::~ActionTracker() {
  auto end = std::chrono::steady_clock::now();
  auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();

  telemetry_.RecordAction(action_, status_, user_agent_, indirect_, duration_ms);

  if (!model_id_.empty()) {
    telemetry_.RecordModelId(action_, model_id_, status_, user_agent_);
  }
}

void ActionTracker::SetStatus(ActionStatus status) {
  status_ = status;
}

void ActionTracker::RecordException(const std::exception& exception) {
  telemetry_.RecordException(action_, exception, user_agent_);
}

void ActionTracker::SetModelId(const std::string& model_id) {
  model_id_ = model_id;
}

}  // namespace fl
