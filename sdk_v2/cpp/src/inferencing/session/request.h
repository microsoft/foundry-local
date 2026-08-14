// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"
#include "util/key_value_pairs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace fl {

class CancellationState;

/// Link between a Request and the native invocation currently using it.
struct RequestCancellationLink {
  std::mutex mutex;
  std::shared_ptr<CancellationState> active;
};

/// Generic inference request — pure input data.
/// Items are stored as borrowed pointers. Owned items are kept alive in owned_items.
struct Request {
  std::vector<Item*> items;  // all items (borrowed pointers)
  KeyValuePairs options;

  /// Diagnostic stop flags used by generation and callback paths.
  mutable std::atomic<bool> canceled{false};
  mutable std::atomic<bool> timed_out{false};

  Request();

  Request(Request&& other) noexcept;
  Request& operator=(Request&& other) noexcept;

  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;

  /// Set the per-invocation wall-clock timeout. Zero disables it.
  void SetTimeout(std::chrono::milliseconds timeout);
  void SetTimeoutMs(uint64_t timeout_ms);
  std::chrono::milliseconds Timeout() const;

  /// Attach fresh state for one invocation. Overlapping use of one Request is unsupported; the latest attach wins.
  void BeginInvocation(std::shared_ptr<CancellationState> state) const;
  void EndInvocation(CancellationState& state) const;

  std::shared_ptr<CancellationState> ActiveCancellationState() const;

  /// Cancel only the invocation currently attached to this Request. Idle cancellation is a no-op.
  void Cancel() const;

  /// Record a streaming callback's successful cooperative stop.
  void StopForConsumer() const;

  /// True once callback stop, API cancellation, session cancellation, or timeout requires generation to stop.
  bool ShouldStop() const;

  /// First-winner success seam used before committing stateful results such as chat history.
  bool TryBeginCompletion() const;

  /// True when API cancellation or timeout may have terminated the invocation's generator.
  bool EngineInterruptionRequested() const;

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
  void LatchOutcome(const CancellationState& state) const;

  mutable std::atomic<uint64_t> timeout_ms_{0};
  std::shared_ptr<RequestCancellationLink> cancellation_link_;
  std::vector<std::unique_ptr<Item>> owned_items;  // owned items (lifetime)
};

}  // namespace fl
