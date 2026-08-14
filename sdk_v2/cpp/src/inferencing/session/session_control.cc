// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session_control.h"

#include <algorithm>

namespace fl {

bool SessionControl::Register(const std::shared_ptr<CancellationState>& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (terminal_) {
    return false;
  }

  active_.push_back(state);
  return true;
}

void SessionControl::Unregister(const CancellationState& state) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_.erase(std::remove_if(active_.begin(), active_.end(),
                                 [&state](const auto& entry) { return entry.get() == &state; }),
                  active_.end());
  }

  cv_.notify_all();
}

SessionControl::Admission SessionControl::AcquirePermit(const CancellationState& state) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this, &state] { return terminal_ || state.StopRequested() || !permit_taken_; });

  if (terminal_) {
    return Admission::kTerminal;
  }

  if (state.StopRequested()) {
    return Admission::kStopped;
  }

  permit_taken_ = true;
  return Admission::kAdmitted;
}

void SessionControl::ReleasePermit() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    permit_taken_ = false;
  }

  cv_.notify_all();
}

void SessionControl::Terminate() {
  std::vector<std::shared_ptr<CancellationState>> active;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    terminal_ = true;
    active = active_;
  }

  cv_.notify_all();

  for (const auto& state : active) {
    state->TryStop(CancellationOutcome::kSessionCanceled);
  }
}

void SessionControl::NotifyAdmission() {
  // Synchronize with the predicate-to-wait transition so a queued cancellation cannot lose its wakeup.
  {
    std::lock_guard<std::mutex> lock(mutex_);
  }

  cv_.notify_all();
}

}  // namespace fl
