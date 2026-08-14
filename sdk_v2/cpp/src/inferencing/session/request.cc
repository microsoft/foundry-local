// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/request.h"

#include "exception.h"
#include "inferencing/session/cancellation_state.h"

#include <limits>
#include <utility>

namespace fl {

Request::Request() : cancellation_link_(std::make_shared<RequestCancellationLink>()) {}

Request::Request(Request&& other) noexcept
    : items(std::move(other.items)),
      options(std::move(other.options)),
      canceled(other.canceled.load(std::memory_order_relaxed)),
      timed_out(other.timed_out.load(std::memory_order_relaxed)),
      timeout_ms_(other.timeout_ms_.load(std::memory_order_relaxed)),
      cancellation_link_(other.cancellation_link_),
      owned_items(std::move(other.owned_items)) {}

Request& Request::operator=(Request&& other) noexcept {
  items = std::move(other.items);
  options = std::move(other.options);
  canceled.store(other.canceled.load(std::memory_order_relaxed), std::memory_order_relaxed);
  timed_out.store(other.timed_out.load(std::memory_order_relaxed), std::memory_order_relaxed);
  timeout_ms_.store(other.timeout_ms_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  cancellation_link_ = other.cancellation_link_;
  owned_items = std::move(other.owned_items);
  return *this;
}

void Request::SetTimeout(std::chrono::milliseconds timeout) {
  timeout_ms_.store(timeout.count() > 0 ? static_cast<uint64_t>(timeout.count()) : uint64_t{0},
                    std::memory_order_relaxed);
}

void Request::SetTimeoutMs(uint64_t timeout_ms) {
  using Rep = std::chrono::milliseconds::rep;
  const auto max_timeout = static_cast<uint64_t>((std::numeric_limits<Rep>::max)());
  if (timeout_ms > max_timeout) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "request timeout is outside the supported range");
  }

  SetTimeout(std::chrono::milliseconds{static_cast<Rep>(timeout_ms)});
}

std::chrono::milliseconds Request::Timeout() const {
  return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
      timeout_ms_.load(std::memory_order_relaxed))};
}

void Request::BeginInvocation(std::shared_ptr<CancellationState> state) const {
  std::lock_guard<std::mutex> lock(cancellation_link_->mutex);
  canceled.store(false, std::memory_order_relaxed);
  timed_out.store(false, std::memory_order_relaxed);
  cancellation_link_->active = std::move(state);
}

void Request::EndInvocation(CancellationState& state) const {
  std::lock_guard<std::mutex> lock(cancellation_link_->mutex);
  if (cancellation_link_->active.get() != &state) {
    return;
  }

  LatchOutcome(state);
  state.DetachDiagnostics();
  cancellation_link_->active.reset();
}

std::shared_ptr<CancellationState> Request::ActiveCancellationState() const {
  std::lock_guard<std::mutex> lock(cancellation_link_->mutex);
  return cancellation_link_->active;
}

void Request::Cancel() const {
  const auto state = ActiveCancellationState();
  if (!state) {
    return;
  }

  state->TryStop(CancellationOutcome::kRequestCanceled);
  LatchOutcome(*state);
}

void Request::StopForConsumer() const {
  canceled.store(true, std::memory_order_relaxed);

  const auto state = ActiveCancellationState();
  if (state) {
    state->TryStop(CancellationOutcome::kConsumerStopped);
  }
}

bool Request::ShouldStop() const {
  if (canceled.load(std::memory_order_relaxed)) {
    return true;
  }

  const auto state = ActiveCancellationState();
  if (!state || !state->ShouldStop()) {
    return false;
  }

  LatchOutcome(*state);
  return true;
}

bool Request::TryBeginCompletion() const {
  if (canceled.load(std::memory_order_relaxed)) {
    return false;
  }

  const auto state = ActiveCancellationState();
  if (!state) {
    return true;
  }

  const bool completing = state->TryBeginCompletion();
  LatchOutcome(*state);
  return completing;
}

bool Request::EngineInterruptionRequested() const {
  const auto state = ActiveCancellationState();
  return state && state->EngineInterruptionRequested();
}

void Request::LatchOutcome(const CancellationState& state) const {
  const auto outcome = state.Outcome();
  if (IsStopOutcome(outcome)) {
    canceled.store(true, std::memory_order_relaxed);
  }

  if (outcome == CancellationOutcome::kTimedOut) {
    timed_out.store(true, std::memory_order_relaxed);
  }
}

}  // namespace fl
