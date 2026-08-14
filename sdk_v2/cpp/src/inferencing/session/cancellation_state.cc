// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/cancellation_state.h"

#include "inferencing/session/session_control.h"

#include <algorithm>
#include <utility>

namespace fl {

CancellationState::CancellationState(std::optional<Clock::time_point> deadline,
                                     std::weak_ptr<SessionControl> control, std::atomic<bool>& canceled,
                                     std::atomic<bool>& timed_out)
    : deadline_(deadline),
      control_(std::move(control)),
      canceled_(&canceled),
      timed_out_(&timed_out) {}

bool CancellationState::TryStop(CancellationOutcome reason) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outcome_ != CancellationOutcome::kRunning) {
      return false;
    }

    outcome_ = reason;
    LatchDiagnosticsLocked();
    if (RequiresEngineInterruption(reason)) {
      engine_interruption_requested_ = true;
      InterruptGeneratorsLocked();
    }
  }

  Publish();
  return true;
}

bool CancellationState::ShouldStop() {
  bool timed_out = false;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsStopOutcome(outcome_)) {
      return true;
    }

    if (outcome_ != CancellationOutcome::kRunning || !DeadlineExpiredLocked()) {
      return false;
    }

    outcome_ = CancellationOutcome::kTimedOut;
    LatchDiagnosticsLocked();
    engine_interruption_requested_ = true;
    InterruptGeneratorsLocked();
    timed_out = true;
  }

  if (timed_out) {
    Publish();
  }

  return timed_out;
}

bool CancellationState::StopRequested() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return IsStopOutcome(outcome_);
}

CancellationOutcome CancellationState::Outcome() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outcome_;
}

bool CancellationState::TryBeginCompletion() {
  bool timed_out = false;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outcome_ != CancellationOutcome::kRunning) {
      return false;
    }

    if (DeadlineExpiredLocked()) {
      outcome_ = CancellationOutcome::kTimedOut;
      LatchDiagnosticsLocked();
      engine_interruption_requested_ = true;
      InterruptGeneratorsLocked();
      timed_out = true;
    } else {
      outcome_ = CancellationOutcome::kCompleting;
    }
  }

  Publish();
  return !timed_out;
}

void CancellationState::Complete() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outcome_ != CancellationOutcome::kRunning && outcome_ != CancellationOutcome::kCompleting) {
      return;
    }

    outcome_ = CancellationOutcome::kCompleted;
  }

  Publish();
}

void CancellationState::Fail() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outcome_ != CancellationOutcome::kRunning && outcome_ != CancellationOutcome::kCompleting) {
      return;
    }

    outcome_ = CancellationOutcome::kFaulted;
  }

  Publish();
}

bool CancellationState::WaitForDeadline() {
  if (!deadline_) {
    return false;
  }

  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (cv_.wait_until(lock, *deadline_, [this] { return outcome_ != CancellationOutcome::kRunning; })) {
      return false;
    }

    if (outcome_ != CancellationOutcome::kRunning) {
      return false;
    }

    outcome_ = CancellationOutcome::kTimedOut;
    LatchDiagnosticsLocked();
    engine_interruption_requested_ = true;
    InterruptGeneratorsLocked();
  }

  Publish();
  return true;
}

void CancellationState::RegisterGenerator(ICancellable& generator) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto existing = std::find_if(generators_.begin(), generators_.end(),
                                     [&generator](const auto& entry) { return entry.generator == &generator; });
  if (existing != generators_.end()) {
    ++existing->registrations;
    return;
  }

  generators_.push_back({&generator, 1});

  if (RequiresEngineInterruption(outcome_)) {
    try {
      generator.Cancel();
    } catch (...) {
      // The invocation is already stopped. Preserve the lifetime guarantee and let normal teardown continue.
    }
  }
}

void CancellationState::UnregisterGenerator(ICancellable& generator) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto existing = std::find_if(generators_.begin(), generators_.end(),
                                     [&generator](const auto& entry) { return entry.generator == &generator; });
  if (existing == generators_.end()) {
    return;
  }

  if (--existing->registrations == 0) {
    generators_.erase(existing);
  }
}

bool CancellationState::EngineInterruptionRequested() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return engine_interruption_requested_;
}

void CancellationState::DetachDiagnostics() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  canceled_ = nullptr;
  timed_out_ = nullptr;
}

bool CancellationState::DeadlineExpiredLocked() const {
  return deadline_ && Clock::now() >= *deadline_;
}

void CancellationState::InterruptGeneratorsLocked() noexcept {
  for (const auto& entry : generators_) {
    try {
      entry.generator->Cancel();
    } catch (...) {
      // One engine failure must not prevent cancellation from reaching another generator in this invocation.
    }
  }
}

void CancellationState::LatchDiagnosticsLocked() noexcept {
  if (canceled_ && IsStopOutcome(outcome_)) {
    canceled_->store(true, std::memory_order_relaxed);
  }

  if (timed_out_ && outcome_ == CancellationOutcome::kTimedOut) {
    timed_out_->store(true, std::memory_order_relaxed);
  }
}

void CancellationState::Publish() {
  cv_.notify_all();

  if (const auto control = control_.lock()) {
    control->NotifyAdmission();
  }
}

}  // namespace fl
