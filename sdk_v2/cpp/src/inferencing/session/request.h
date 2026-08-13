// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/request_control.h"
#include "items/item.h"
#include "util/key_value_pairs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fl {

/// Generic inference request — pure input data plus a stable, shared claim control that binds it to at most
/// one in-flight operation at a time.
///
/// Item storage. `items` is the ordered view; every entry is either *retained* (its shared_ptr lives in
/// owned_items_, so the Request keeps it alive) or *borrowed* (the caller keeps it alive, per the
/// AddBorrowedItem contract). Owned items are held by shared_ptr rather than unique_ptr so a RequestSnapshot
/// can take exact shared ownership of them instead of copying — several item types are deliberately
/// non-copyable, and a live streaming ItemQueue cannot be copied at all.
///
/// Run-state authority lives in the OperationState reached through `control_`, never here. The
/// `canceled`/`timed_out` flags are **views onto a diagnostic mirror that lives in that control**, describing
/// the bound operation's stop state for the current claim epoch: they are written by that operation, read by
/// tests and by callers that want to know how a run ended, and are never consulted by production generation
/// loops (those take an explicit OperationContext). Keeping the bits in the control is what lets a stop
/// record them without ever locking — or even touching — the Request. `canceled` additionally routes an
/// externally requested cancel to the bound operation so the existing C ABI `Request_Cancel` (which assigns
/// `true` to it) still interrupts the engine.
///
/// The Request has **no destructor lifetime gate**. An operation never borrows the live Request across a
/// suspension point; it runs against a RequestSnapshot captured synchronously at CreateOperation. Waiting in
/// ~Request would be both unsound (the wait starts after destruction has begun) and deadlock-prone (a
/// streaming callback that drops the Request from inside its own run would wait on itself).
struct Request {
  /// Read/write view onto one of the control-owned diagnostic bits (see DiagnosticBit).
  ///
  /// The bit itself lives in the shared RequestControl, not in the Request. That is what lets the bound
  /// operation mirror a stop without touching the Request at all, and it makes the mirror travel with the
  /// control across a move.
  ///
  /// The view keeps the `.store()/.load()/= true/if (flag)` syntax the C ABI and existing callers use.
  /// Assigning true (or store(true)) on a routing view additionally *routes* to the bound operation (engine
  /// terminate_session included) instead of merely latching a bit. Once the ABI layer calls
  /// Request::CancelActiveOperation() directly the routing shim can be dropped.
  class DiagnosticFlag {
   public:
    constexpr DiagnosticFlag(DiagnosticBit bit, bool routes_cancel) noexcept
        : bit_(bit), routes_cancel_(routes_cancel) {}

    DiagnosticFlag(const DiagnosticFlag&) = delete;
    DiagnosticFlag& operator=(const DiagnosticFlag&) = delete;

    /// Bind the view to its enclosing Request. Called from every Request constructor; a move never copies
    /// the pointer, so it always refers to the object this view is a member of and can never dangle.
    void BindOwner(const Request& owner) { owner_ = &owner; }

    /// `request->canceled = true;` — the shape the C ABI uses.
    DiagnosticFlag& operator=(bool value) {
      Write(value, std::memory_order_relaxed);
      return *this;
    }

    void store(bool value, std::memory_order order = std::memory_order_relaxed) { Write(value, order); }

    bool load(std::memory_order order = std::memory_order_relaxed) const;

    explicit operator bool() const { return load(); }

    /// Mirror write. Deliberately bypasses cancel routing: a caller already delivering the stop would
    /// otherwise re-enter the operation it is stopping.
    void StoreDiagnostic(bool value, std::memory_order order = std::memory_order_relaxed) const;

   private:
    /// Store the bit and, for a routing view, atomically select either the bound operation or the idle
    /// compatibility diagnostic.
    void Write(bool value, std::memory_order order);

    /// The control that owns the bits, or nullptr on a moved-from Request.
    RequestControl* Control() const;

    /// The enclosing Request. Never dangles: it points at the object this view is a member of.
    const Request* owner_ = nullptr;
    const DiagnosticBit bit_;
    const bool routes_cancel_;
  };

  std::vector<Item*> items;  // ordered view over retained + borrowed items
  KeyValuePairs options;

  /// Diagnostic mirror of the bound operation's stop state; assigning true also routes an external cancel to
  /// that operation. The bit lives in the shared control, so it is written by one thread (callback worker,
  /// watchdog, or C API) and read by another without ever locking the Request.
  mutable DiagnosticFlag canceled{DiagnosticBit::kCancelled, /*routes_cancel=*/true};

  /// Set when the run stopped because its deadline expired rather than by an explicit cancel. Diagnostic
  /// mirror only; callers use it to distinguish a timeout from a user cancel.
  mutable DiagnosticFlag timed_out{DiagnosticBit::kTimedOut, /*routes_cancel=*/false};

  Request() {
    BindDiagnostics();
  }

  ~Request() = default;

  /// Moving transfers the *exact* claim control — identity, epoch, claim and diagnostic bits — to the
  /// destination. The claim is never duplicated and no control is allocated for a move: the moved-from
  /// Request is left with none at all, so it can no longer be used to create an operation (CreateOperation
  /// rejects it with INVALID_USAGE).
  Request(Request&& other) noexcept : Request(NoControlTag{}) {
    MoveFrom(std::move(other));
  }

  Request& operator=(Request&& other) noexcept {
    if (this != &other) {
      MoveFrom(std::move(other));
    }

    return *this;
  }

  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;

  /// Set a wall-clock budget for the request. Zero (the default) means no deadline.
  /// Takes effect on the next operation created for this request.
  void SetTimeout(std::chrono::milliseconds timeout) {
    timeout_ms_.store(timeout.count() < 0 ? 0 : static_cast<uint64_t>(timeout.count()), std::memory_order_relaxed);
  }

  std::chrono::milliseconds Timeout() const {
    return std::chrono::milliseconds(timeout_ms_.load(std::memory_order_relaxed));
  }

  /// Clear the diagnostic mirror. Production runs get this for free: a won claim clears the bits under the
  /// claim lock (RequestControl::TryClaim), so they always describe the operation that currently owns the
  /// Request. This stays for the legacy ArmDeadline path below. No-op on a moved-from Request.
  void ResetDiagnostics() const {
    if (control_) {
      control_->ResetDiagnostics();
    }
  }

  /// Legacy/diagnostic deadline helpers. Production runs get their budget from the operation's absolute
  /// deadline (snapshotted at CreateOperation, watchdog-enforced); these remain for direct-Request unit
  /// coverage that drives a generator without a Session.
  void ArmDeadline() const {
    ResetDiagnostics();

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

  /// Legacy/diagnostic stop check: true once the mirror says cancelled or the legacy armed deadline passed.
  /// **Not** a production stop authority — production paths poll OperationContext::ShouldStop().
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
    canceled.StoreDiagnostic(true);
    return true;
  }

  /// Add a pre-allocated owned item. The Request keeps it alive; a RequestSnapshot retains the same object
  /// rather than copying it.
  void AddOwnedItem(std::unique_ptr<Item> item) {
    AddRetainedItem(std::shared_ptr<Item>(std::move(item)));
  }

  /// Retain an already-shared item. Used by RequestSnapshot to take exact shared ownership of the source
  /// Request's items, which is what makes a deferred Process safe for non-copyable item types.
  void AddRetainedItem(std::shared_ptr<Item> item) {
    if (!item) {
      return;
    }

    items.push_back(item.get());
    owned_items_.push_back(std::move(item));
  }

  /// Add a borrowed item (caller must keep it alive).
  void AddBorrowedItem(Item* item) {
    items.push_back(item);
  }

  /// The items this Request keeps alive, in insertion order.
  const std::vector<std::shared_ptr<Item>>& OwnedItems() const { return owned_items_; }

  /// The shared owner of `item` if this Request retains it, otherwise nullptr (a borrowed item).
  std::shared_ptr<Item> FindOwnedItem(const Item* item) const {
    auto it = std::find_if(owned_items_.begin(), owned_items_.end(),
                           [item](const std::shared_ptr<Item>& owned) { return owned.get() == item; });
    return it != owned_items_.end() ? *it : nullptr;
  }

  // --- operation binding ---

  /// The stable claim control. An Operation holds this shared_ptr instead of a `Request&`, so a claim can be
  /// released by identity and epoch without ever dereferencing the Request. Null only on a moved-from
  /// Request, which Session::CreateOperation rejects with FOUNDRY_LOCAL_ERROR_INVALID_USAGE.
  const std::shared_ptr<RequestControl>& Control() const { return control_; }

  /// Cancel the operation currently bound to this Request, if any. Returns true when a cancel was routed to an
  /// active operation, false when the Request is idle or moved-from. This is the hook the ABI's Request_Cancel
  /// routes through so a cancel targets only the active operation (interrupting its generators mid-compute)
  /// rather than latching a reusable atomic.
  ///
  /// ActiveOperation snapshots the state under RequestControl::claim_mu_ and releases that lock before
  /// entering OperationState::RequestStop. The only nested order is the accepted-stop diagnostic path,
  /// OperationState::mu_ -> RequestControl::claim_mu_; this future API routing path never holds both and does
  /// not prewrite an idle diagnostic. Engine cancellation runs after both locks have been released.
  bool CancelActiveOperation() const {
    if (!control_) {
      return false;  // moved-from shell: it can no longer own an operation
    }

    // ActiveOperation() takes claim_mu_, copies the shared_ptr and releases it, so the RequestStop below runs
    // with no RequestControl lock held.
    auto operation = control_->ActiveOperation();
    if (!operation) {
      return false;  // idle: nothing bound, so routing a cancel is a no-op
    }

    operation->RequestStop(StopReason::kExternalCancel);
    return true;
  }

 private:
  using Clock = std::chrono::steady_clock;

  /// Tag for the allocation-free shell the move constructor starts from: `control_` stays null and is
  /// immediately replaced by the source's control, so no RequestControl is allocated for a move and both
  /// move paths stay noexcept.
  struct NoControlTag {};

  explicit Request(NoControlTag) noexcept : control_(nullptr) {
    BindDiagnostics();
  }

  /// Point both diagnostic views at this object. Called from every constructor.
  void BindDiagnostics() noexcept {
    canceled.BindOwner(*this);
    timed_out.BindOwner(*this);
  }

  /// Shared move implementation. The destination's previous control is simply dropped: an operation that
  /// still holds it keeps it alive and releases against it by identity and epoch, and because operations run
  /// against a snapshot they were never reading this Request's storage in the first place.
  void MoveFrom(Request&& other) noexcept {
    items = std::move(other.items);
    options = std::move(other.options);
    owned_items_ = std::move(other.owned_items_);
    timeout_ms_.store(other.timeout_ms_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    deadline_ticks_.store(other.deadline_ticks_.load(std::memory_order_relaxed), std::memory_order_relaxed);

    // The diagnostic mirror lives in the control, so transferring it carries the flags across unchanged and
    // leaves the moved-from Request reporting the clean state.
    control_ = std::move(other.control_);
    other.control_.reset();
    other.items.clear();
    other.owned_items_.clear();
  }

  mutable std::atomic<uint64_t> timeout_ms_{0};
  /// steady_clock time_point ticks for the legacy armed deadline; 0 means unarmed.
  mutable std::atomic<int64_t> deadline_ticks_{0};

  /// Stable claim gate and diagnostic mirror; shared with the Operation that claimed it. Null only on a
  /// moved-from Request, which can no longer be claimed and reports clean diagnostics.
  std::shared_ptr<RequestControl> control_ = std::make_shared<RequestControl>();

  /// Items whose lifetime this Request owns, in insertion order.
  std::vector<std::shared_ptr<Item>> owned_items_;
};

inline RequestControl* Request::DiagnosticFlag::Control() const {
  return owner_ != nullptr ? owner_->control_.get() : nullptr;
}

inline bool Request::DiagnosticFlag::load(std::memory_order order) const {
  // A moved-from Request owns no control and therefore no diagnostics: report the clean state instead of
  // dereferencing.
  const RequestControl* control = Control();
  return control != nullptr && control->Diagnostic(bit_, order);
}

inline void Request::DiagnosticFlag::StoreDiagnostic(bool value, std::memory_order order) const {
  if (RequestControl* control = Control()) {
    control->SetDiagnostic(bit_, value, order);
  }
}

inline void Request::DiagnosticFlag::Write(bool value, std::memory_order order) {
  // A non-routing view (timed_out), a clearing write, or a view with no owner latches the diagnostic bit
  // directly — there is nothing to linearize against.
  if (!routes_cancel_ || !value || owner_ == nullptr) {
    StoreDiagnostic(value, order);
    return;
  }

  RequestControl* control = Control();
  if (control == nullptr) {
    return;
  }

  // Select the active epoch or preserve the legacy idle diagnostic under the same claim lock TryClaim uses.
  // RequestStop runs after that lock is released. If an operation existed but has since sealed, its rejected
  // stop deliberately does not fall back to a diagnostic write.
  auto operation = control->ActiveOperationOrSetIdleCancelled(order);
  if (operation) {
    operation->RequestStop(StopReason::kExternalCancel);
  }
}

}  // namespace fl
