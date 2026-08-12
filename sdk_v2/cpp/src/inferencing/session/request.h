// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"
#include "util/key_value_pairs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace fl {

/// Generic inference request — pure input data.
/// Items are stored as borrowed pointers. Owned items are kept alive in owned_items.
struct Request {
  std::vector<Item*> items;  // all items (borrowed pointers)
  KeyValuePairs options;

  /// Cancellation flag — set by the C API or streaming callback handler to cancel
  /// an in-flight request. Checked in generation loops. Atomic because it is written
  /// by one thread (callback worker or C API) and read by another (generator loop).
  /// Uses relaxed ordering since it is a one-way flag and exact timing doesn't matter.
  mutable std::atomic<bool> canceled{false};

  /// Set to true when the request was stopped because its deadline expired rather than
  /// by an explicit Cancel(). Callers use this to distinguish a timeout from a user cancel.
  mutable std::atomic<bool> timed_out{false};

  Request() = default;

  Request(Request&& other) noexcept
      : items(std::move(other.items)),
        options(std::move(other.options)),
        canceled(other.canceled.load(std::memory_order_relaxed)),
        timed_out(other.timed_out.load(std::memory_order_relaxed)),
        timeout_ms_(other.timeout_ms_.load(std::memory_order_relaxed)),
        deadline_ticks_(other.deadline_ticks_.load(std::memory_order_relaxed)),
        owned_items(std::move(other.owned_items)) {}

  Request& operator=(Request&& other) noexcept {
    items = std::move(other.items);
    options = std::move(other.options);
    canceled.store(other.canceled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    timed_out.store(other.timed_out.load(std::memory_order_relaxed), std::memory_order_relaxed);
    timeout_ms_.store(other.timeout_ms_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    deadline_ticks_.store(other.deadline_ticks_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    owned_items = std::move(other.owned_items);
    return *this;
  }

  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;

  /// Set a wall-clock budget for the request. Zero (the default) means no deadline.
  /// Takes effect on the next ArmDeadline() — i.e. the next ProcessRequest call.
  void SetTimeout(std::chrono::milliseconds timeout) {
    timeout_ms_.store(timeout.count() < 0 ? 0 : static_cast<uint64_t>(timeout.count()), std::memory_order_relaxed);
  }

  std::chrono::milliseconds Timeout() const {
    return std::chrono::milliseconds(timeout_ms_.load(std::memory_order_relaxed));
  }

  /// Start the timeout clock and clear per-run stop state. Called by Session::ProcessRequest
  /// so a Request reused across calls gets a fresh budget each time.
  void ArmDeadline() const {
    canceled.store(false, std::memory_order_relaxed);
    timed_out.store(false, std::memory_order_relaxed);

    const auto budget = timeout_ms_.load(std::memory_order_relaxed);
    if (budget == 0) {
      deadline_ticks_.store(0, std::memory_order_relaxed);
      return;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(budget);
    deadline_ticks_.store(static_cast<int64_t>(deadline.time_since_epoch().count()), std::memory_order_relaxed);
  }

  /// Clear the deadline so a later check cannot trip after the run has ended.
  void DisarmDeadline() const { deadline_ticks_.store(0, std::memory_order_relaxed); }

  /// True once the request should stop — either explicitly cancelled or past its deadline.
  /// Deadline expiry latches `canceled` so the existing post-loop cancellation handling
  /// (rewind, finish_reason, history rollback) applies unchanged to timeouts.
  bool ShouldStop() const {
    if (canceled.load(std::memory_order_relaxed)) {
      return true;
    }

    const auto ticks = deadline_ticks_.load(std::memory_order_relaxed);
    if (ticks == 0) {
      return false;
    }

    if (Clock::now().time_since_epoch().count() < ticks) {
      return false;
    }

    timed_out.store(true, std::memory_order_relaxed);
    canceled.store(true, std::memory_order_relaxed);
    return true;
  }

  /// Add a pre-allocated owned item.
  void AddOwnedItem(std::unique_ptr<Item> item) {
    items.push_back(item.get());
    owned_items.push_back(std::move(item));
  }

  /// Add a borrowed item (caller must keep it alive).
  void AddBorrowedItem(Item* item) {
    items.push_back(item);
  }

 private:
  using Clock = std::chrono::steady_clock;

  mutable std::atomic<uint64_t> timeout_ms_{0};
  /// steady_clock time_point ticks for the current run's deadline; 0 means unarmed.
  mutable std::atomic<int64_t> deadline_ticks_{0};
  std::vector<std::unique_ptr<Item>> owned_items;  // owned items (lifetime)
};

}  // namespace fl
