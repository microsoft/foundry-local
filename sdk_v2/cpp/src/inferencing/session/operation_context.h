// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancel_slot.h"
#include "inferencing/session/cancellable.h"
#include "inferencing/session/operation_state.h"

#include <memory>
#include <utility>

namespace fl {

/// The explicit, per-run cancellation handle handed to every production inference path.
///
/// This replaces the old implicit model where a generation loop polled `Request::canceled` and generator
/// registration resolved the "current" operation from thread-local state. Both were unsound: the Request
/// flag is a reusable object-lifetime-scoped bit (a stale run could erase or observe another run's cancel),
/// and thread-local resolution silently no-oped whenever a helper ran on a different thread. An
/// OperationContext is passed down every modality helper that polls cancellation or publishes a generator,
/// so the stop authority is always the exact operation driving the call.
///
/// Header-only: every member is a one-line forward to the shared OperationState, so a .cc would add a
/// translation unit and an out-of-line call for no benefit.
///
/// Const-ness: the context is handed out as `const OperationContext&` because callees must not rebind it.
/// The *state* it refers to is shared and mutable by design (the watchdog and external cancellers act on it
/// concurrently), so the mutating operations are const member functions.
class OperationContext {
 public:
  explicit OperationContext(std::shared_ptr<OperationState> state) : state_(std::move(state)) {}

  OperationContext(const OperationContext&) = delete;
  OperationContext& operator=(const OperationContext&) = delete;

  /// True once this operation has been asked to stop, for any reason. The only stop authority production
  /// generation loops may consult.
  bool ShouldStop() const { return state_->IsStopped(); }

  /// Why the operation stopped (kNone while running).
  StopReason Reason() const { return state_->Reason(); }

  /// Ask this exact operation to stop. Returns true only for the call that latched the stop.
  bool RequestStop(StopReason reason) const { return state_->RequestStop(reason); }

  /// True once a generator Cancel() (engine terminate_session) was actually delivered for this operation.
  /// A generator that reports this is permanently terminated: it must not be rewound or reused.
  bool EngineCancelDelivered() const { return state_->WasEngineCancelDelivered(); }

  /// True once ProcessRequestImpl threw and the run settled as faulted. Modality cleanup hooks use it to
  /// discard cross-request state (a cached generator, tool context) a failed run may have already mutated.
  bool IsFaulted() const { return state_->Status() == OperationStatus::kFaulted; }

  /// Close this run's outcome to cancellation immediately before committing it.
  ///
  /// The modality contract is strict, and the whole point of the seal is that it is checked in one place:
  ///   1. every ActiveGenerator guard for this run has been destroyed,
  ///   2. every engine read the commit depends on has already happened,
  ///   3. every streaming callback that could ask to stop has quiesced (CallbackHandler::Quiesce),
  ///   4. only then TrySeal().
  /// On true the run owns its outcome and no later cancel or timeout can touch the generator, the session's
  /// history, or the response. On false cancellation won: discard or roll back and report a stopped run.
  bool TrySeal() const { return state_->TrySeal(); }

  /// True once TrySeal() has succeeded for this run.
  bool IsSealed() const { return state_->IsSealed(); }

  /// Publish/withdraw a borrowed generator. Prefer the ActiveGenerator guard below over calling these.
  OperationState::GeneratorRegistration RegisterGenerator(ICancellable& generator) const {
    return state_->RegisterGenerator(generator);
  }

  void CancelGenerator(const std::shared_ptr<CancelSlot>& slot) const { state_->CancelGenerator(slot); }

  void WithdrawGenerator(const std::shared_ptr<CancelSlot>& slot) const { state_->WithdrawGenerator(slot); }

  const std::shared_ptr<OperationState>& State() const { return state_; }

 private:
  std::shared_ptr<OperationState> state_;
};

/// RAII guard publishing the generator currently driving a request so that a session cancel, an explicit
/// operation cancel, or the per-operation deadline watchdog can interrupt it mid-compute.
///
/// The generator is registered with the *explicitly supplied* operation, so concurrent operations on the
/// same session (embeddings) stay isolated: a stop of one operation only cancels its own generators.
///
/// Register-after-stop: if a stop was already latched when the guard is constructed, the cancellation is
/// delivered here — through a lease, with no framework, operation or slot lock held — rather than inside the
/// registration itself. The lease dies before the constructor returns, so the destructor's withdraw can
/// never end up waiting on a lease this same thread still holds.
///
/// Destruction withdraws the slot and blocks until any in-flight Cancel() on it has returned, which is what
/// makes it safe for the caller to destroy the generator right after the guard's scope ends.
class ActiveGenerator {
 public:
  ActiveGenerator(const OperationContext& operation, ICancellable& generator) : operation_(operation) {
    auto registration = operation_.RegisterGenerator(generator);
    slot_ = std::move(registration.slot);

    if (registration.cancel_required) {
      operation_.CancelGenerator(slot_);
    }
  }

  ~ActiveGenerator() noexcept { operation_.WithdrawGenerator(slot_); }

  ActiveGenerator(const ActiveGenerator&) = delete;
  ActiveGenerator& operator=(const ActiveGenerator&) = delete;

 private:
  const OperationContext& operation_;
  std::shared_ptr<CancelSlot> slot_;
};

}  // namespace fl
