// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancellable.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace fl {

class SessionControl;

/// First-winner outcome for one native Session::ProcessRequest invocation.
enum class CancellationOutcome {
  kRunning,
  kConsumerStopped,
  kCanceled,
  kTimedOut,
  kCompleting,
  kFinished,
};

constexpr bool IsStopOutcome(CancellationOutcome outcome) {
  return outcome == CancellationOutcome::kConsumerStopped ||
         outcome == CancellationOutcome::kCanceled ||
         outcome == CancellationOutcome::kTimedOut;
}

constexpr bool RequiresEngineInterruption(CancellationOutcome outcome) {
  return outcome == CancellationOutcome::kCanceled ||
         outcome == CancellationOutcome::kTimedOut;
}

/// Cancellation and deadline state owned by one ProcessRequest invocation.
///
/// Generator cancellation is synchronous. UnregisterGenerator() cannot complete until Cancel() returns.
class CancellationState {
 public:
  using Clock = std::chrono::steady_clock;

  CancellationState(std::optional<Clock::time_point> deadline, std::weak_ptr<SessionControl> control,
                    std::atomic<bool>& canceled, std::atomic<bool>& timed_out);

  CancellationState(const CancellationState&) = delete;
  CancellationState& operator=(const CancellationState&) = delete;

  bool TryStop(CancellationOutcome reason);
  bool ShouldStop();
  bool StopRequested() const;
  CancellationOutcome Outcome() const;

  /// Enter completion only if cancellation or timeout has not stopped this invocation.
  bool TryBeginCompletion();
  void Complete();
  void Fail();

  /// Wait until the deadline or another outcome. Return true only when this call records the timeout.
  bool WaitForDeadline();

  void RegisterGenerator(ICancellable& generator);
  void UnregisterGenerator(ICancellable& generator) noexcept;

  bool EngineInterruptionRequested() const;

  /// Clear pointers to Request-owned flags before this state can outlive the Request.
  void DetachRequestFlags() noexcept;

 private:
  struct GeneratorEntry {
    ICancellable* generator;
    std::size_t registrations;
  };

  bool DeadlineExpiredLocked() const;
  void RecordTimeoutLocked() noexcept;
  void InterruptGeneratorsLocked() noexcept;
  void UpdateRequestFlagsLocked() noexcept;
  void NotifyWaiters();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  CancellationOutcome outcome_ = CancellationOutcome::kRunning;
  bool engine_interruption_requested_ = false;
  const std::optional<Clock::time_point> deadline_;
  const std::weak_ptr<SessionControl> control_;
  std::atomic<bool>* canceled_;
  std::atomic<bool>* timed_out_;
  std::vector<GeneratorEntry> generators_;
};

}  // namespace fl
