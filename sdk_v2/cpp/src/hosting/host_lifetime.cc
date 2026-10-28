// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/host_lifetime.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace fl::hosting {

struct HostLifetime::SharedState {
  explicit SharedState(TimePoint startup_deadline) : deadline(startup_deadline) {}

  TimePoint Observe(TimePoint now) {
    latest_observation = std::max(latest_observation, now);

    if (state == State::kDraining && ipc_work_count == 0 && http_work_count == 0) {
      BeginGrace(latest_observation);
    }

    if (deadline && latest_observation >= *deadline) {
      state = State::kShutdownCommitted;
      deadline.reset();
    }

    return latest_observation;
  }

  void BeginGrace(TimePoint now) {
    state = State::kGrace;
    const auto latest_start = TimePoint::max() - kReconnectGrace;
    deadline = now > latest_start ? TimePoint::max() : now + kReconnectGrace;
  }

  std::mutex mutex;
  State state = State::kAwaitingFirstOwner;
  std::unordered_set<OwnerId> owners;
  std::size_t ipc_work_count = 0;
  std::size_t http_work_count = 0;
  bool stopping_http = false;
  std::optional<TimePoint> deadline;
  TimePoint latest_observation = TimePoint::min();
};

HostLifetime::HostLifetime(TimePoint startup_deadline) : state_(std::make_shared<SharedState>(startup_deadline)) {}

HostLifetime::~HostLifetime() = default;

bool HostLifetime::AddOwner(OwnerId owner, TimePoint now) {
  const std::lock_guard lock(state_->mutex);
  state_->Observe(now);

  if (state_->state == State::kShutdownCommitted) {
    return false;
  }

  state_->owners.insert(owner);
  state_->state = State::kAccepting;
  state_->deadline.reset();
  return true;
}

bool HostLifetime::RemoveOwner(OwnerId owner, TimePoint now) {
  const std::lock_guard lock(state_->mutex);
  now = state_->Observe(now);

  if (state_->owners.erase(owner) == 0) {
    return false;
  }

  if (state_->owners.empty()) {
    state_->state = State::kDraining;

    if (state_->ipc_work_count == 0 && state_->http_work_count == 0) {
      state_->BeginGrace(now);
    }
  }

  return true;
}

bool HostLifetime::RetireUnusedStartup(TimePoint now) {
  const std::lock_guard lock(state_->mutex);
  state_->Observe(now);
  if (state_->state != State::kAwaitingFirstOwner) {
    return false;
  }

  state_->state = State::kShutdownCommitted;
  state_->deadline.reset();
  return true;
}

HostLifetime::WorkGuard HostLifetime::TryAdmitIpcWork(OwnerId owner, TimePoint now) { return TryAdmitWork(owner, now); }

HostLifetime::WorkGuard HostLifetime::TryAdmitHttpWork(TimePoint now) { return TryAdmitWork(std::nullopt, now); }

HostLifetime::WorkGuard HostLifetime::TryAdmitWebServiceStop(OwnerId owner, TimePoint now) {
  return TryAdmitWork(owner, now, true);
}

HostLifetime::WorkGuard HostLifetime::TryAdmitWork(std::optional<OwnerId> owner, TimePoint now, bool stopping_http) {
  const std::lock_guard lock(state_->mutex);
  state_->Observe(now);

  if (state_->state != State::kAccepting || state_->owners.empty() || (owner && !state_->owners.contains(*owner))) {
    return {};
  }
  if ((!owner && state_->stopping_http) ||
      (stopping_http && (state_->owners.size() != 1 || state_->http_work_count != 0 || state_->stopping_http))) {
    return {};
  }

  if (stopping_http) {
    state_->stopping_http = true;
  }
  if (owner) {
    ++state_->ipc_work_count;
  } else {
    ++state_->http_work_count;
  }

  return WorkGuard(state_, owner, stopping_http);
}

HostLifetime::Snapshot HostLifetime::Poll(TimePoint now) {
  const std::lock_guard lock(state_->mutex);
  state_->Observe(now);
  return {
      .state = state_->state,
      .owner_count = state_->owners.size(),
      .ipc_work_count = state_->ipc_work_count,
      .http_work_count = state_->http_work_count,
      .deadline = state_->deadline,
  };
}

HostLifetime::WorkGuard::WorkGuard(std::shared_ptr<SharedState> state, std::optional<OwnerId> owner,
                                   bool stopping_http) noexcept
    : state_(std::move(state)), owner_(owner), stopping_http_(stopping_http) {}

HostLifetime::WorkGuard::~WorkGuard() { Release(std::nullopt); }

HostLifetime::WorkGuard::WorkGuard(WorkGuard&& other) noexcept
    : state_(std::move(other.state_)),
      owner_(std::exchange(other.owner_, std::nullopt)),
      stopping_http_(std::exchange(other.stopping_http_, false)) {}

HostLifetime::WorkGuard& HostLifetime::WorkGuard::operator=(WorkGuard&& other) noexcept {
  if (this != &other) {
    Release(std::nullopt);
    state_ = std::move(other.state_);
    owner_ = std::exchange(other.owner_, std::nullopt);
    stopping_http_ = std::exchange(other.stopping_http_, false);
  }

  return *this;
}

HostLifetime::WorkGuard::operator bool() const noexcept { return state_ != nullptr; }

std::optional<HostLifetime::OwnerId> HostLifetime::WorkGuard::Owner() const noexcept { return owner_; }

void HostLifetime::WorkGuard::Complete(TimePoint now) noexcept { Release(now); }

void HostLifetime::WorkGuard::Release(std::optional<TimePoint> now) noexcept {
  // The local reference keeps the mutex alive until after unlocking, even if the controller is already gone.
  auto state = std::move(state_);

  if (!state) {
    return;
  }

  const std::lock_guard lock(state->mutex);

  if (now) {
    now = state->Observe(*now);
  }

  if (owner_) {
    --state->ipc_work_count;
  } else {
    --state->http_work_count;
  }

  owner_.reset();
  if (std::exchange(stopping_http_, false)) {
    state->stopping_http = false;
  }

  if (now && state->state == State::kDraining && state->ipc_work_count == 0 && state->http_work_count == 0) {
    state->BeginGrace(*now);
  }
}

}  // namespace fl::hosting
