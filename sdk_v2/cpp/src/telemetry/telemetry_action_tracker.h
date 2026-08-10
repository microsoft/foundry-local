// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry.h"

#include <chrono>
#include <string>

namespace fl {

/// RAII action tracker that records timing and status via ITelemetry on destruction.
/// Default status is kFailure — call SetStatus(kSuccess) on the happy path.
/// Matches the C# ActionTracker (IDisposable) pattern.
class ActionTracker {
 public:
  ActionTracker(Action action, ITelemetry& telemetry, InvocationContext context = {});
  ~ActionTracker();

  // Non-copyable, non-movable
  ActionTracker(const ActionTracker&) = delete;
  ActionTracker& operator=(const ActionTracker&) = delete;

  /// Set the final status. Default is kFailure if not called.
  void SetStatus(ActionStatus status);

  /// Record an exception associated with this action.
  void RecordException(const std::exception& exception);

  /// Associate a model ID with this action.
  void SetModelId(const std::string& model_id);

  /// This action's context, with its correlation id resolved. Use to derive a
  /// child context (Context().AsIndirect()) for any caused-by action so all
  /// events from one operation share a correlation id.
  const InvocationContext& Context() const { return context_; }

 private:
  Action action_;
  ITelemetry& telemetry_;
  InvocationContext context_;
  ActionStatus status_ = ActionStatus::kFailure;
  std::string model_id_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace fl
