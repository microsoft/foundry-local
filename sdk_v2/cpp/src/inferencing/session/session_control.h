// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancellation_state.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace fl {

/// Admission and terminal cancellation shared by all invocations on one Session.
class SessionControl {
 public:
  enum class Admission {
    kAdmitted,
    kStopped,
    kTerminal,
  };

  bool Register(const std::shared_ptr<CancellationState>& state);
  void Unregister(const CancellationState& state);

  /// Wait until no other chat/audio invocation is running. Cancellation or timeout ends the wait.
  Admission AcquireInferenceSlot(const CancellationState& state);
  void ReleaseInferenceSlot();

  /// Terminally stop every active or queued invocation and reject future registrations.
  void Terminate();

  void NotifyAdmission();

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool terminal_ = false;
  bool inference_slot_in_use_ = false;
  std::vector<std::shared_ptr<CancellationState>> active_;
};

}  // namespace fl
