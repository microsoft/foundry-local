// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancel_slot.h"
#include "inferencing/session/cancellable.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace fl {

class RequestControl;
class SessionRuntime;

/// Why an operation was asked to stop. Latched exactly once — first stop wins — so the reason a run ended is
/// unambiguous even when a session cancel, a deadline, and a callback stop all fire at once.
enum class StopReason : uint8_t {
  kNone = 0,        ///< Still running (or finished naturally).
  kExternalCancel,  ///< Session::Cancel(), Operation::Cancel(), or the C ABI Request_Cancel.
  kTimeout,         ///< The operation's absolute deadline expired.
  kCallbackStop,    ///< A streaming callback returned non-zero or threw.
};

/// Terminal outcome of a single operation run. Consumed by Session::ProcessRequest, which maps the failure
/// outcomes onto the public error contract.
enum class OperationOutcome {
  kCompleted,        ///< Inference ran to a natural stop (stop / length / tool-calls).
  kCallbackStopped,  ///< A streaming callback asked to stop. Normal completion with FINISH_NONE.
  kCancelled,        ///< Cancelled by the session, the operation, or the request.
  kTimedOut,         ///< The operation's absolute deadline expired.
  kFaulted,          ///< Inference threw. The original exception is propagated by the caller.
  kAbandoned,        ///< The operation was destroyed without ever being processed.
};

/// Lifecycle of an operation. Advances monotonically; terminal states never revert.
enum class OperationStatus {
  kCreated,          ///< Registered, not yet admitted to run.
  kQueued,           ///< Waiting in the serial admission gate for a permit.
  kRunning,          ///< Executing ProcessRequestImpl.
  kCompleted,        ///< Ran to completion.
  kCallbackStopped,  ///< Stopped by a streaming callback.
  kCancelled,        ///< Cancelled (sticky if it never reached kRunning).
  kTimedOut,         ///< Deadline expired.
  kFaulted,          ///< ProcessRequestImpl threw.
  kAbandoned,        ///< Operation destroyed without being processed.
};

/// The outcome a latched stop maps to. kNone means the run was not stopped at all.
constexpr OperationOutcome OutcomeForStopReason(StopReason reason) {
  switch (reason) {
    case StopReason::kTimeout:
      return OperationOutcome::kTimedOut;
    case StopReason::kCallbackStop:
      return OperationOutcome::kCallbackStopped;
    case StopReason::kExternalCancel:
      return OperationOutcome::kCancelled;
    case StopReason::kNone:
    default:
      return OperationOutcome::kCompleted;
  }
}

/// Per-invocation run authority. One OperationState backs exactly one Operation and one runtime run.
///
/// Ownership: SessionRuntime holds a *strong* ref while the operation is registered; Operation holds a
/// strong ref for its lifetime; OperationState holds only *weak* refs back to its runtime and request
/// control. That keeps the state alive for the whole run (including the watchdog thread) without an
/// ownership cycle, and without any raw Session/Request pointer that could dangle.
///
/// Locking discipline — the runtime rule is "never call into ORT or the logger under a framework lock":
///   - `stop_reason_` is an atomic latch, so a stop can be *observed* without taking any lock.
///   - `mu_` guards the lifecycle status, the completion seal and the watchdog condition variable. RequestStop
///     and TrySeal both synchronise here, which is what makes "seal wins / stop wins" a single decision.
///     An accepted stop mirrors its diagnostic while this lock is still held, taking
///     RequestControl::claim_mu_ below it. No path holds claim_mu_ while acquiring mu_: TryClaim only performs
///     the atomic epoch bind, ActiveOperation releases its snapshot lock before RequestStop, and Release takes
///     only the claim lock.
///   - `slot_mu_` guards only the *vector* of generator cancel slots. A stop detaches the vector by swap;
///     every ICancellable::Cancel() runs afterwards, lock-free, under a CancelLease that pins
///     the target. Lock order stays acyclic: SessionRuntime::mu_ -> OperationState::mu_, slot_mu_ standalone,
///     CancelSlot::mu_ below slot_mu_ and never above it.
///
/// TestHooks is a model-free deterministic seam. Both callbacks are invoked without allocation: `now` while
/// mu_ is held, and `before_generator_drain` after every framework lock has been released.
class OperationState {
 public:
  struct TestHooks {
    using NowFn = std::chrono::steady_clock::time_point (*)(void*) noexcept;
    using HookFn = void (*)(void*) noexcept;

    NowFn now = nullptr;
    HookFn before_generator_drain = nullptr;
    void* context = nullptr;
  };

  /// The result of publishing a generator: the slot that owns the publication, plus whether the caller must
  /// deliver a cancellation because a stop had already been latched. The caller delivers it *outside* every
  /// lock — see ActiveGenerator.
  struct GeneratorRegistration {
    std::shared_ptr<CancelSlot> slot;
    bool cancel_required = false;
  };

  OperationState(std::weak_ptr<SessionRuntime> runtime, std::weak_ptr<RequestControl> request_control,
                 std::optional<std::chrono::steady_clock::time_point> deadline, std::chrono::milliseconds budget,
                 const TestHooks* test_hooks = nullptr);

  OperationState(const OperationState&) = delete;
  OperationState& operator=(const OperationState&) = delete;

  /// The absolute deadline snapshotted at creation (includes any serialized queue wait), or nullopt.
  const std::optional<std::chrono::steady_clock::time_point>& Deadline() const { return deadline_; }
  bool HasDeadline() const { return deadline_.has_value(); }

  /// True once the deadline instant has passed. Cheap and lock-free; used by the admission gate and by the
  /// concurrent (embeddings) path to refuse a run whose budget was already spent while queued.
  bool DeadlineExpired() const;

  /// The originally requested budget, snapshotted for log and error messages.
  std::chrono::milliseconds Budget() const { return budget_; }

  OperationStatus Status() const;

  /// The owning runtime, or nullptr if it has already been destroyed.
  std::shared_ptr<SessionRuntime> Runtime() const { return runtime_.lock(); }

  /// True once a stop has been latched. Lock-free — this is the single production stop authority.
  bool IsStopped() const { return Reason() != StopReason::kNone; }

  StopReason Reason() const { return static_cast<StopReason>(stop_reason_.load(std::memory_order_acquire)); }

  /// True once a generator Cancel() (engine terminate_session) was actually delivered for this operation.
  bool WasEngineCancelDelivered() const { return engine_cancel_delivered_.load(std::memory_order_acquire); }

  /// Bind the request-claim epoch this operation owns. Called by RequestControl::TryClaim while the state is
  /// still unreachable by any other thread, so no synchronisation beyond the atomic store is needed.
  void BindRequestEpoch(uint64_t epoch) { request_epoch_.store(epoch, std::memory_order_release); }
  uint64_t RequestEpoch() const { return request_epoch_.load(std::memory_order_acquire); }

  // --- lifecycle transitions (called by SessionRuntime while orchestrating the run) ---

  /// Move Created -> Queued. No-op if already past Created.
  void MarkQueued();

  /// Move to Running. Returns false if a stop was already latched or the operation already settled — the
  /// caller must not run.
  bool MarkRunning();

  /// Record the terminal outcome and wake the watchdog. Returns the resolved outcome, derived solely from
  /// the latched stop reason (no Request diagnostics are consulted). A sealed operation has no stop reason
  /// by construction, so it always resolves to kCompleted.
  OperationOutcome Finalize();

  /// Terminal fault: ProcessRequestImpl threw. Wakes the watchdog so it can never act on a faulted run.
  void MarkFaulted() noexcept;

  /// Mark the operation abandoned (unprocessed Operation destroyed). Idempotent.
  void Abandon() noexcept;

  // --- cancellation ---

  /// Latch a stop and deliver it: cancels this operation's published generators (engine terminate_session),
  /// wakes the admission gate and the watchdog, and mirrors the stop into the Request's diagnostics.
  ///
  /// Thread-safe, idempotent, and never waits on a Session or Request lifetime lock. Returns true only for
  /// the call that actually latched the stop, so a watchdog that lost the completion race stays quiet.
  /// Returns false immediately — before any slot is even looked at — once the run has been sealed.
  bool RequestStop(StopReason reason) noexcept;

  // --- completion seal ---

  /// Close the outcome to cancellation. Called by the modality layer once every generator slot has been
  /// withdrawn and every callback decision has quiesced, immediately before it commits its response and any
  /// session-visible state.
  ///
  /// Returns true if this run owns the outcome: no stop had been latched, and none can be latched from here
  /// on. Returns false if cancellation or the deadline won the race — the modality must then roll back or
  /// discard whatever it was about to commit and report a stopped run.
  ///
  /// Idempotent: a second call on an already-sealed operation returns true.
  bool TrySeal() noexcept;

  /// True once TrySeal() has succeeded.
  bool IsSealed() const;

  // --- generator publication (per-operation, isolated from sibling operations) ---

  /// Publish a borrowed generator so a stop of *this* operation can interrupt it mid-compute.
  ///
  /// Returns the owning slot and, when a stop had already been latched, `cancel_required = true`. The caller
  /// must then deliver the cancellation itself, outside every lock — this function never calls into the
  /// engine while holding the slot registry.
  [[nodiscard]] GeneratorRegistration RegisterGenerator(ICancellable& generator);

  /// Deliver cancellation to one registered slot, if it still has a live target. The engine-delivery bit is
  /// published only when the target confirms terminate_session succeeded, so an unsupported cancellation
  /// attempt cannot hide an unrelated engine failure.
  void CancelGenerator(const std::shared_ptr<CancelSlot>& slot) noexcept;

  /// Withdraw a published generator. Removes the slot from the registry, then blocks until every outstanding
  /// lease on it has been released, so the generator is never dereferenced after the caller destroys it.
  void WithdrawGenerator(const std::shared_ptr<CancelSlot>& slot) noexcept;

  // --- watchdog support ---

  /// Block until the deadline elapses or the run finishes/stops. Returns true only if the deadline expired
  /// with the run still active.
  bool WaitForDeadline();

  /// Wake WaitForDeadline() (used by the finalize paths).
  void NotifyWatcher() noexcept;

 private:
  /// Latch `reason` if no stop has been latched yet. Returns true if this call won. Caller holds mu_.
  bool LatchStop(StopReason reason);

  /// True once the operation has settled and can no longer be stopped. Caller holds mu_.
  bool IsSettledLocked() const;

  /// Read the steady clock while mu_ is held. The injected clock is used only by model-free tests.
  std::chrono::steady_clock::time_point NowLocked() const noexcept;

  /// Deliver the lock-free side effects of a stop already latched and mirrored under mu_: wake the serial
  /// admission gate and cancel every published generator. Shared by RequestStop, the deadline watchdog
  /// (WaitForDeadline) and the seal-time timeout latch.
  void DeliverStop() noexcept;

  /// Detach every published generator under slot_mu_, then deliver each cancellation through a lease with no
  /// lock held. The swap cannot allocate and also makes each published slot eligible for delivery at most once.
  void CancelGenerators() noexcept;

  /// Mirror the stop into the bound request control's diagnostic bits while mu_ is held. Takes
  /// RequestControl::claim_mu_ second and never dereferences the Request — diagnostics must be committed
  /// before Finalize can release the claim, but must never invoke cancellation.
  void MirrorStop(StopReason reason) noexcept;

  std::weak_ptr<SessionRuntime> runtime_;
  std::weak_ptr<RequestControl> request_control_;
  std::atomic<uint64_t> request_epoch_{0};
  const std::optional<std::chrono::steady_clock::time_point> deadline_;
  const std::chrono::milliseconds budget_;
  const TestHooks* const test_hooks_;

  /// The single production stop authority. Atomic so it can be polled from a token loop and read from the
  /// slot registry without ever nesting the lifecycle mutex.
  std::atomic<uint8_t> stop_reason_{static_cast<uint8_t>(StopReason::kNone)};
  std::atomic<bool> engine_cancel_delivered_{false};

  mutable std::mutex mu_;
  std::condition_variable cv_;  // watchdog wakes on this
  OperationStatus status_ = OperationStatus::kCreated;
  bool finished_ = false;

  /// Set by TrySeal(). Once true, RequestStop() is a no-op: the run owns its outcome.
  bool sealed_ = false;

  /// Guards the slot vector only — never held across an engine call.
  std::mutex slot_mu_;
  std::vector<std::shared_ptr<CancelSlot>> slots_;
};

}  // namespace fl
