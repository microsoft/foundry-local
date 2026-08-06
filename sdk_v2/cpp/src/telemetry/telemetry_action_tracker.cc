// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry_action_tracker.h"

#include <utility>

namespace fl {

ActionTracker::ActionTracker(Action action, ITelemetry& telemetry, InvocationContext context)
    : action_(action),
      telemetry_(telemetry),
      context_(std::move(context)),
      start_(std::chrono::steady_clock::now()) {
  // Guarantee a correlation id so this action and anything it triggers can be
  // grouped, even when the caller passed a default-constructed context.
  if (context_.user_agent.empty()) {
    context_.user_agent = DefaultUserAgent();
  }
  context_.EnsureCorrelationId();
}

ActionTracker::~ActionTracker() {
  try {
    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();

    telemetry_.RecordAction(action_, status_, context_, duration_ms, model_id_);
  } catch (...) {
    // Telemetry is best-effort and must not throw from RAII cleanup.
  }
}

void ActionTracker::SetStatus(ActionStatus status) {
  status_ = status;
}

void ActionTracker::RecordException(const std::exception& exception) {
  status_ = ActionStatusFromException(exception);
  try {
    telemetry_.RecordException(action_, exception, context_);
  } catch (...) {
    // Telemetry is best-effort and must not mask the original error path.
  }
}

void ActionTracker::SetModelId(const std::string& model_id) {
  model_id_ = model_id;
}

}  // namespace fl
