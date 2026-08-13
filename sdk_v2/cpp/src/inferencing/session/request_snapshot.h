// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/request.h"

#include <memory>

namespace fl {

/// Immutable execution data for one operation, captured synchronously at CreateOperation.
///
/// Why this exists. An Operation may be created now and processed later, possibly on another thread, while
/// the caller still owns the Request and is free to mutate, move or destroy it. Two obvious answers are both
/// wrong: copying the Request does not compile (it is non-copyable, and several item types are non-copyable
/// by design), and gating ~Request on an in-flight run is unsound and self-deadlocks when the run's own
/// callback releases the Request. So the operation runs against this snapshot instead, and never touches the
/// caller's Request at all.
///
/// What is captured, per item:
///   - **Retained items** (added via AddOwnedItem, which is what the C ABI's take_ownership path and every
///     internal builder use): the snapshot takes *shared ownership* of the exact same object. No copy, so
///     non-copyable payloads — a live streaming ItemQueue, a TensorItem with a custom deleter — keep working
///     and keep their identity, and the caller releasing the Request cannot free them.
///   - **Borrowed items** (AddBorrowedItem): copyable types are deep-cloned into snapshot-owned storage.
///     Types that cannot be cloned without changing their meaning are borrowed through only for the
///     synchronous Session::ProcessRequest convenience, whose caller already guarantees their lifetime.
///     Explicit deferred operations reject such a snapshot at creation.
///
/// Claim identity is deliberately *not* part of this type. The Operation holds the source Request's
/// RequestControl separately, so cancellation and diagnostics still target the exact reusable Request by
/// epoch while execution reads only immutable data.
class RequestSnapshot {
 public:
  /// Capture `source`. Throws only what item cloning throws (allocation).
  static std::shared_ptr<const RequestSnapshot> Capture(const Request& source);

  RequestSnapshot(const RequestSnapshot&) = delete;
  RequestSnapshot& operator=(const RequestSnapshot&) = delete;

  /// The stable request the modality layer executes against.
  const Request& Data() const { return data_; }

  /// True when at least one item is borrowed from caller storage the snapshot could not take over. The
  /// explicit deferred-operation path rejects this; only synchronous ProcessRequest may accept it.
  bool HasBorrowedItems() const { return has_borrowed_items_; }

 private:
  RequestSnapshot() = default;

  Request data_;
  bool has_borrowed_items_ = false;
};

}  // namespace fl
