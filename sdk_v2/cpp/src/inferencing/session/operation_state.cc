// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/operation_state.h"

#include "inferencing/session/request_control.h"
#include "inferencing/session/session_runtime.h"

#include <algorithm>
#include <utility>

namespace fl {

namespace {

/// Terminal states can no longer be stopped, run, or re-finalized.
bool IsTerminalStatus(OperationStatus status) {
  switch (status) {
    case OperationStatus::kCompleted:
    case OperationStatus::kCallbackStopped:
    case OperationStatus::kCancelled:
    case OperationStatus::kTimedOut:
    case OperationStatus::kFaulted:
    case OperationStatus::kAbandoned:
      return true;
    default:
      return false;
  }
}

/// The sticky status a stop imposes on an operation that never reached kRunning.
OperationStatus StatusForStopReason(StopReason reason) {
  switch (reason) {
    case StopReason::kTimeout:
      return OperationStatus::kTimedOut;
    case StopReason::kCallbackStop:
      return OperationStatus::kCallbackStopped;
    default:
      return OperationStatus::kCancelled;
  }
}

}  // namespace

OperationState::OperationState(std::weak_ptr<SessionRuntime> runtime, std::weak_ptr<RequestControl> request_control,
                               std::optional<std::chrono::steady_clock::time_point> deadline,
                               std::chrono::milliseconds budget, const TestHooks* test_hooks)
    : runtime_(std::move(runtime)),
      request_control_(std::move(request_control)),
      deadline_(deadline),
      budget_(budget),
      test_hooks_(test_hooks) {
}

bool OperationState::DeadlineExpired() const {
  return deadline_.has_value() && std::chrono::steady_clock::now() >= *deadline_;
}

OperationStatus OperationState::Status() const {
  std::lock_guard<std::mutex> lock(mu_);
  return status_;
}

bool OperationState::IsSettledLocked() const {
  return IsTerminalStatus(status_);
}

bool OperationState::LatchStop(StopReason reason) {
  auto expected = static_cast<uint8_t>(StopReason::kNone);
  return stop_reason_.compare_exchange_strong(expected, static_cast<uint8_t>(reason), std::memory_order_acq_rel,
                                              std::memory_order_acquire);
}

void OperationState::MarkQueued() {
  std::lock_guard<std::mutex> lock(mu_);
  if (status_ == OperationStatus::kCreated) {
    status_ = OperationStatus::kQueued;
  }
}

bool OperationState::MarkRunning() {
  std::lock_guard<std::mutex> lock(mu_);
  // Check the stop latch while holding the same lock RequestStop() uses to latch it. Otherwise a stop can
  // land between an unlocked pre-check and this transition, allowing a cancelled queued operation to run.
  if (IsStopped() || IsSettledLocked()) {
    return false;
  }

  status_ = OperationStatus::kRunning;
  return true;
}

OperationOutcome OperationState::Finalize() {
  OperationOutcome outcome;

  {
    std::lock_guard<std::mutex> lock(mu_);
    // Resolve the outcome under the same lock RequestStop() and TrySeal() use. A sealed run has no stop
    // reason by construction, so the seal is consumed here simply by resolving to kCompleted; Finalize can
    // never re-open an outcome the seal already closed.
    outcome = OutcomeForStopReason(Reason());

    // A fault already settled this operation; the caller is propagating the original exception.
    if (status_ != OperationStatus::kFaulted) {
      switch (outcome) {
        case OperationOutcome::kTimedOut:
          status_ = OperationStatus::kTimedOut;
          break;
        case OperationOutcome::kCallbackStopped:
          status_ = OperationStatus::kCallbackStopped;
          break;
        case OperationOutcome::kCancelled:
          status_ = OperationStatus::kCancelled;
          break;
        default:
          status_ = OperationStatus::kCompleted;
          break;
      }
    }

    finished_ = true;
  }

  cv_.notify_all();
  return outcome;
}

void OperationState::MarkFaulted() noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_ = OperationStatus::kFaulted;
    finished_ = true;
  }

  cv_.notify_all();
}

void OperationState::Abandon() noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!IsSettledLocked()) {
      status_ = OperationStatus::kAbandoned;
    }

    finished_ = true;
  }

  cv_.notify_all();
}

bool OperationState::RequestStop(StopReason reason) noexcept {
  if (reason == StopReason::kNone) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);

    // The seal is checked first and under the same lock that sets it: once the modality has committed its
    // outcome, a late cancel or timeout must not reach a generator, a history commit, or the response.
    if (sealed_) {
      return false;
    }

    // Check terminal state and latch the stop atomically with respect to Finalize(), MarkFaulted(),
    // Abandon(), and MarkRunning(). A late cancel must never change diagnostics after completion.
    if (IsSettledLocked()) {
      return false;
    }

    if (!LatchStop(reason)) {
      return false;  // idempotent — the first stop wins and has already been delivered
    }

    // Sticky for a not-yet-running operation: MarkRunning() will refuse to start it.
    if (status_ == OperationStatus::kCreated || status_ == OperationStatus::kQueued) {
      status_ = StatusForStopReason(reason);
    }

    // Wake the watchdog: it must never outlive the stop it can no longer act on.
    finished_ = true;

    // Linearize diagnostics before Finalize can acquire mu_ and before Operation releases this request claim.
    // Lock order is OperationState::mu_ -> RequestControl::claim_mu_; no engine or logger call occurs here.
    MirrorStop(reason);
  }

  cv_.notify_all();

  // Admission notification and engine cancellation run with no framework lock held.
  DeliverStop();
  return true;
}

std::chrono::steady_clock::time_point OperationState::NowLocked() const noexcept {
  if (test_hooks_ != nullptr && test_hooks_->now != nullptr) {
    return test_hooks_->now(test_hooks_->context);
  }

  return std::chrono::steady_clock::now();
}

bool OperationState::TrySeal() noexcept {
  bool latched_timeout = false;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (sealed_) {
      return true;  // idempotent: the run already owns its outcome
    }

    if (IsStopped()) {
      return false;  // cancellation or the deadline won the race earlier
    }

    // Sampling belongs to the terminal decision. Reading the clock before taking mu_ would let a thread pause
    // with a pre-expiry value, resume after the deadline, and seal ahead of the watchdog.
    const auto now = NowLocked();

    // The deadline and the seal are decided together, under the one lock RequestStop() also latches through.
    // If the budget has already elapsed with no earlier stop, latch the timeout right here instead of
    // sealing — a run that overran its deadline must never be able to seal and report a natural outcome.
    // Before the deadline the seal wins and closes the outcome to cancellation.
    if (deadline_.has_value() && now >= *deadline_ && LatchStop(StopReason::kTimeout)) {
      if (status_ == OperationStatus::kCreated || status_ == OperationStatus::kQueued) {
        status_ = StatusForStopReason(StopReason::kTimeout);
      }

      finished_ = true;
      latched_timeout = true;
      MirrorStop(StopReason::kTimeout);
    } else {
      sealed_ = true;
    }
  }

  // Wake the watchdog immediately either way: it can no longer act on this run.
  cv_.notify_all();

  // Admission notification and engine cancellation run with no framework lock held, exactly like RequestStop.
  if (latched_timeout) {
    DeliverStop();
    return false;
  }

  return true;
}

bool OperationState::IsSealed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sealed_;
}

OperationState::GeneratorRegistration OperationState::RegisterGenerator(ICancellable& generator) {
  GeneratorRegistration registration{CancelSlot::Create(generator), false};

  {
    std::lock_guard<std::mutex> slot_lock(slot_mu_);
    if (IsStopped()) {
      // Retain this slot only in the returned registration. The caller will cancel it once outside the lock;
      // publishing it as well would let the stop's detached snapshot deliver a second cancellation.
      registration.cancel_required = true;
    } else {
      // A stop that latches after this check cannot snapshot until it acquires slot_mu_, so it will detach this
      // publication and become its sole cancellation owner.
      slots_.push_back(registration.slot);
    }
  }

  return registration;
}

void OperationState::CancelGenerator(const std::shared_ptr<CancelSlot>& slot) noexcept {
  if (!slot) {
    return;
  }

  const CancelLease lease = slot->Acquire();
  if (!lease) {
    return;
  }

  if (lease.Cancel()) {
    engine_cancel_delivered_.store(true, std::memory_order_release);
  }
}

void OperationState::WithdrawGenerator(const std::shared_ptr<CancelSlot>& slot) noexcept {
  if (!slot) {
    return;
  }

  {
    std::lock_guard<std::mutex> slot_lock(slot_mu_);
    slots_.erase(std::remove(slots_.begin(), slots_.end(), slot), slots_.end());
  }

  // Outside slot_mu_ on purpose: Withdraw() blocks until every outstanding lease is released, and a
  // canceller holding one of those leases must be able to finish without needing the registry lock.
  slot->Withdraw();
}

void OperationState::CancelGenerators() noexcept {
  std::vector<std::shared_ptr<CancelSlot>> to_cancel;

  {
    std::lock_guard<std::mutex> slot_lock(slot_mu_);
    to_cancel.swap(slots_);
  }

  for (const auto& slot : to_cancel) {
    // A slot already withdrawn yields an empty lease and does not count as delivered.
    CancelGenerator(slot);
  }
}

void OperationState::MirrorStop(StopReason reason) noexcept {
  auto request_control = request_control_.lock();
  if (!request_control) {
    return;
  }

  request_control->MirrorStop(*this, RequestEpoch(), reason);
}

void OperationState::DeliverStop() noexcept {
  if (auto runtime = runtime_.lock()) {
    runtime->NotifyAdmission();
  }

  if (test_hooks_ != nullptr && test_hooks_->before_generator_drain != nullptr) {
    test_hooks_->before_generator_drain(test_hooks_->context);
  }

  CancelGenerators();
}

bool OperationState::WaitForDeadline() {
  if (!deadline_.has_value()) {
    return false;
  }

  bool latched = false;

  {
    std::unique_lock<std::mutex> lock(mu_);

    // Wake early if the run finished, was sealed, or was stopped; otherwise sleep out the budget.
    cv_.wait_until(lock, *deadline_, [this] { return finished_ || sealed_ || IsStopped(); });

    // Latch the timeout under the same lock acquisition that observed expiry — there is no separate
    // observe-then-unlock-then-latch window a seal could slip through. Only a run that is still unsettled,
    // unsealed and unstopped, with the deadline genuinely elapsed, becomes a timeout here.
    if (!finished_ && !sealed_ && !IsStopped() && std::chrono::steady_clock::now() >= *deadline_ &&
        LatchStop(StopReason::kTimeout)) {
      if (status_ == OperationStatus::kCreated || status_ == OperationStatus::kQueued) {
        status_ = StatusForStopReason(StopReason::kTimeout);
      }

      finished_ = true;
      latched = true;
      MirrorStop(StopReason::kTimeout);
    }
  }

  // Notify and cancel outside the lock, exactly like RequestStop. Returns true only for the call that actually
  // latched the timeout, so a watchdog that lost the race against a completing or sealed run stays silent.
  if (latched) {
    cv_.notify_all();
    DeliverStop();
  }

  return latched;
}

void OperationState::NotifyWatcher() noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    finished_ = true;
  }

  cv_.notify_all();
}

}  // namespace fl
