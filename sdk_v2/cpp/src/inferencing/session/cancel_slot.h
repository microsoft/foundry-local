// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancellable.h"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace fl {

class CancelSlot;

/// A short-lived, strong pin on a CancelSlot's target.
///
/// Holding a lease guarantees the target ICancellable is still alive and cannot be withdrawn: the slot's
/// withdraw path nulls the target and then blocks until every outstanding lease has been released. The lease
/// also keeps the slot itself alive, so a canceller never touches freed control storage even if the
/// operation it belongs to finishes underneath it.
///
/// Cancel() is deliberately invoked *outside* every framework, operation, session and slot mutex: it reaches
/// into the ORT GenAI engine (terminate_session), which must never run under a lock the inference thread can
/// also be waiting on.
class CancelLease {
 public:
  CancelLease() = default;
  ~CancelLease() noexcept;

  CancelLease(CancelLease&& other) noexcept;
  CancelLease& operator=(CancelLease&& other) noexcept;

  CancelLease(const CancelLease&) = delete;
  CancelLease& operator=(const CancelLease&) = delete;

  /// True when this lease actually pinned a live target.
  explicit operator bool() const noexcept { return target_ != nullptr; }

  /// Deliver the cancellation. Must be called with no mutex held. Returns true only when the live target
  /// confirms that engine termination was delivered; returns false for an empty lease or a failed delivery.
  [[nodiscard]] bool Cancel() const noexcept;

 private:
  friend class CancelSlot;

  CancelLease(std::shared_ptr<CancelSlot> slot, ICancellable* target) noexcept;

  void Release() noexcept;

  std::shared_ptr<CancelSlot> slot_;
  ICancellable* target_ = nullptr;
};

/// Lifetime-pinned publication point for one borrowed ICancellable.
///
/// One slot is created per generator publication (ActiveGenerator owns it). The generator is borrowed
/// storage owned by the modality; the slot is the only thing that decides whether it may still be touched.
///
/// This exists to remove the previous "cancel under the registry lock" pattern, where delivering a stop
/// invoked ICancellable::Cancel() while holding the operation's generator mutex. That made an engine call
/// part of the framework's lock graph and forced unregistration to block on it. Here a registry lock is only
/// ever held long enough to copy shared_ptrs; the Cancel() itself runs lock-free under a lease.
class CancelSlot : public std::enable_shared_from_this<CancelSlot> {
 private:
  struct PrivateTag {
    explicit PrivateTag() = default;
  };

 public:
  /// Publish `target`. The caller must Withdraw() before `target` is destroyed.
  static std::shared_ptr<CancelSlot> Create(ICancellable& target);

  /// Only reachable through Create(): PrivateTag keeps this effectively private while letting make_shared
  /// construct the object in one allocation.
  CancelSlot(PrivateTag /*tag*/, ICancellable& target) noexcept : target_(&target) {}

  CancelSlot(const CancelSlot&) = delete;
  CancelSlot& operator=(const CancelSlot&) = delete;

  /// Pin the current target, if any. Returns an empty lease once the slot has been withdrawn.
  [[nodiscard]] CancelLease Acquire() noexcept;

  /// Retire the slot: nulls the target so no later Acquire() can see it, then waits until every outstanding
  /// lease has been released. After it returns, the target can be destroyed safely.
  ///
  /// Must never be called from a thread that currently holds a lease on this slot. The call paths are built
  /// so that cannot happen (a canceller never withdraws, and the owning guard never holds a lease across its
  /// own scope) rather than relying on a thread-id escape hatch.
  void Withdraw() noexcept;

  /// Diagnostic: number of leases currently outstanding.
  size_t OutstandingLeases() const;

 private:
  friend class CancelLease;

  void ReleaseLease() noexcept;

  mutable std::mutex mu_;
  std::condition_variable idle_cv_;
  ICancellable* target_ = nullptr;
  size_t active_leases_ = 0;
};

}  // namespace fl
