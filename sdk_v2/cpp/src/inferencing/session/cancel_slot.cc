// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/cancel_slot.h"

#include <utility>

namespace fl {

// ===========================================================================
// CancelLease
// ===========================================================================

CancelLease::CancelLease(std::shared_ptr<CancelSlot> slot, ICancellable* target) noexcept
    : slot_(std::move(slot)), target_(target) {
}

CancelLease::~CancelLease() noexcept {
  Release();
}

CancelLease::CancelLease(CancelLease&& other) noexcept
    : slot_(std::move(other.slot_)), target_(std::exchange(other.target_, nullptr)) {
}

CancelLease& CancelLease::operator=(CancelLease&& other) noexcept {
  if (this != &other) {
    Release();
    slot_ = std::move(other.slot_);
    target_ = std::exchange(other.target_, nullptr);
  }

  return *this;
}

bool CancelLease::Cancel() const noexcept {
  if (target_ != nullptr) {
    // No slot lock, no operation lock, no session lock: this call reaches the inference engine.
    return target_->Cancel();
  }

  return false;
}

void CancelLease::Release() noexcept {
  if (target_ == nullptr) {
    slot_.reset();
    return;
  }

  target_ = nullptr;

  // Keep the slot alive across the accounting: the waiter inside Withdraw() is woken while this reference
  // still guarantees the slot's storage, and only then is the reference dropped.
  const std::shared_ptr<CancelSlot> slot = std::move(slot_);
  slot->ReleaseLease();
}

// ===========================================================================
// CancelSlot
// ===========================================================================

std::shared_ptr<CancelSlot> CancelSlot::Create(ICancellable& target) {
  return std::make_shared<CancelSlot>(PrivateTag{}, target);
}

CancelLease CancelSlot::Acquire() noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  if (target_ == nullptr) {
    return CancelLease{};
  }

  auto self = weak_from_this().lock();
  if (!self) {
    return CancelLease{};
  }

  ++active_leases_;
  return CancelLease(std::move(self), target_);
}

void CancelSlot::Withdraw() noexcept {
  std::unique_lock<std::mutex> lock(mu_);

  // Null the target first, so every Acquire() from here on returns an empty lease. Only leases taken before
  // this point can still be holding the generator, and those are exactly what the wait below drains.
  target_ = nullptr;
  idle_cv_.wait(lock, [this] { return active_leases_ == 0; });
}

size_t CancelSlot::OutstandingLeases() const {
  std::lock_guard<std::mutex> lock(mu_);
  return active_leases_;
}

void CancelSlot::ReleaseLease() noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (active_leases_ > 0) {
      --active_leases_;
    }
  }

  idle_cv_.notify_all();
}

}  // namespace fl
