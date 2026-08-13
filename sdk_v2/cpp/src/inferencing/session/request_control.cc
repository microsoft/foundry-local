// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/request_control.h"

namespace fl {

RequestControl::ClaimResult RequestControl::TryClaim(const std::shared_ptr<OperationState>& state) {
  std::lock_guard<std::mutex> lock(claim_mu_);
  if (claimed_) {
    return {};
  }

  ++epoch_;
  claimed_ = true;

  // Clear the previous run's mirror under the same lock that gates it: the claim that wins always starts
  // from a deterministically clean slate, with no window for a late stop of the previous run to land after.
  ClearDiagnosticsLocked();

  // Bind the epoch before publishing the state: until `operation_` is set nothing else can reach it, so the
  // diagnostic mirror can never run against epoch 0.
  state->BindRequestEpoch(epoch_);
  operation_ = state;

  return ClaimResult{.claimed = true, .epoch = epoch_};
}

void RequestControl::Release(const OperationState& state, uint64_t epoch) noexcept {
  // Drop the strong ref outside the lock: releasing the last reference here would run ~OperationState under
  // the claim lock.
  std::shared_ptr<OperationState> released;

  {
    std::lock_guard<std::mutex> lock(claim_mu_);

    // Exact identity + epoch match: a stale release from an already-finished operation must not clear the
    // claim of a later run on the same Request.
    if (!claimed_ || epoch_ != epoch || operation_.get() != &state) {
      return;
    }

    claimed_ = false;
    released = std::move(operation_);
    operation_.reset();
  }
}

std::shared_ptr<OperationState> RequestControl::ActiveOperation() const {
  std::lock_guard<std::mutex> lock(claim_mu_);
  return operation_;
}

std::shared_ptr<OperationState> RequestControl::ActiveOperationOrSetIdleCancelled(std::memory_order order) {
  std::lock_guard<std::mutex> lock(claim_mu_);
  if (operation_) {
    return operation_;
  }

  // TryClaim clears this under the same lock before publishing a new epoch, so an idle compatibility write
  // can describe only the idle/previous epoch and can never leak into a newly claimed operation.
  cancelled_.store(true, order);
  return {};
}

void RequestControl::ResetDiagnostics() {
  std::lock_guard<std::mutex> lock(claim_mu_);
  ClearDiagnosticsLocked();
}

void RequestControl::MirrorStop(const OperationState& state, uint64_t epoch, StopReason reason) noexcept {
  std::lock_guard<std::mutex> lock(claim_mu_);

  // Exact identity + epoch match: a finished operation must never write into a later run's diagnostics.
  if (!claimed_ || epoch_ != epoch || operation_.get() != &state) {
    return;
  }

  if (reason == StopReason::kTimeout) {
    timed_out_.store(true, std::memory_order_relaxed);
  }

  // Mirror writes bypass the Request's cancel routing on purpose: routing here would re-enter the operation
  // that is already delivering this very stop.
  cancelled_.store(true, std::memory_order_relaxed);
}

void RequestControl::ClearDiagnosticsLocked() {
  cancelled_.store(false, std::memory_order_relaxed);
  timed_out_.store(false, std::memory_order_relaxed);
}

}  // namespace fl
