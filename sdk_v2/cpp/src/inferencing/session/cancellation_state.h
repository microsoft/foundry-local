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
  kRequestCanceled,
  kSessionCanceled,
  kTimedOut,
  kCompleting,
  kCompleted,
  kFaulted,
};

constexpr bool IsStopOutcome(CancellationOutcome outcome) {
  return outcome == CancellationOutcome::kConsumerStopped ||
         outcome == CancellationOutcome::kRequestCanceled ||
         outcome == CancellationOutcome::kSessionCanceled ||
         outcome == CancellationOutcome::kTimedOut;
}

constexpr bool RequiresEngineInterruption(CancellationOutcome outcome) {
  return outcome == CancellationOutcome::kRequestCanceled ||
         outcome == CancellationOutcome::kSessionCanceled ||
         outcome == CancellationOutcome::kTimedOut;
}

/// Cancellation and deadline state owned by one ProcessRequest invocation.
///
/// Generator cancellation is deliberately synchronous. The registry mutex stays held while Cancel() runs, so a
/// generator guard cannot unregister and permit destruction until cancellation returns.
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

  /// Seal successful completion against a cancellation or timeout that has already won.
  bool TryBeginCompletion();
  void Complete();
  void Fail();

  /// Wait until the absolute deadline or another terminal outcome. Returns true when this call latched the timeout.
  bool WaitForDeadline();

  void RegisterGenerator(ICancellable& generator);
  void UnregisterGenerator(ICancellable& generator) noexcept;

  bool EngineInterruptionRequested() const;

  /// Stop mirroring diagnostics before the Request may be destroyed. A late session-cancel snapshot then stays safe.
  void DetachDiagnostics() noexcept;

 private:
  struct GeneratorEntry {
    ICancellable* generator;
    std::size_t registrations;
  };

  bool DeadlineExpiredLocked() const;
  void InterruptGeneratorsLocked() noexcept;
  void LatchDiagnosticsLocked() noexcept;
  void Publish();

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
