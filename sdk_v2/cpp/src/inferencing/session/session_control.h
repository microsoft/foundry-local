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

  /// Wait for the single chat/audio permit. Cancellation and timeout wake this wait through NotifyAdmission().
  Admission AcquirePermit(const CancellationState& state);
  void ReleasePermit();

  /// Terminally stop every active or queued invocation and reject future registrations.
  void Terminate();

  void NotifyAdmission();

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool terminal_ = false;
  bool permit_taken_ = false;
  std::vector<std::shared_ptr<CancellationState>> active_;
};

}  // namespace fl
