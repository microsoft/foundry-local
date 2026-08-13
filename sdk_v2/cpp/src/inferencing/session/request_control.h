// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/operation_state.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace fl {

/// One of the control-owned diagnostic bits describing how the current (or last) run ended.
///
/// The bits deliberately live in the RequestControl rather than in the Request: mirroring a stop must never
/// reach through anything the Request owns, because the thread delivering that stop may be the run itself.
/// Keeping them here also means the mirror travels with the control across a move, and that a destroyed
/// Request can never make a stop block.
enum class DiagnosticBit : uint8_t {
  kCancelled,  ///< The bound operation was asked to stop, for any reason.
  kTimedOut,   ///< ... and the reason was its own deadline expiring.
};

/// Stable, shared claim identity for one Request.
///
/// The control owns *claim identity only* — deliberately not the Request's storage or lifetime. An earlier
/// shape put a reader/writer "owner gate" here and had ~Request block on it so an in-flight operation could
/// borrow the live Request. That is unsound: the wait happens after destruction has already begun, and a
/// callback that releases the Request from inside its own run self-deadlocks. Execution data is snapshotted
/// instead (see RequestSnapshot), so nothing ever borrows a Request across a suspension point and this class
/// reduces to claim bookkeeping plus a diagnostic mirror.
///
/// At most one operation may hold a Request at a time and each claim gets a monotonic epoch. Release and
/// MirrorStop validate the exact OperationState **and** epoch, so a stale release or a late stop from a
/// finished operation can never touch a later run's claim. The same lock gates the diagnostic mirror, so a
/// new claim always starts from a deterministically cleared state.
///
/// Lock order is OperationState::mu_ -> claim_mu_. TryClaim calls only BindRequestEpoch (an atomic store)
/// while holding claim_mu_; operation snapshots release claim_mu_ before RequestStop; Release takes only
/// claim_mu_. No path nests the locks in the opposite order.
class RequestControl {
 public:
  struct ClaimResult {
    bool claimed = false;
    uint64_t epoch = 0;
  };

  RequestControl() = default;

  RequestControl(const RequestControl&) = delete;
  RequestControl& operator=(const RequestControl&) = delete;

  // --- claim state ---

  /// Claim the Request for `state`. Fails if another operation already holds it. On success the diagnostic
  /// mirror is cleared and the state is bound to the new epoch, both before it becomes reachable through
  /// ActiveOperation().
  ClaimResult TryClaim(const std::shared_ptr<OperationState>& state);

  /// Release the claim held by exactly `state` at exactly `epoch`. A mismatched (stale) release is ignored.
  void Release(const OperationState& state, uint64_t epoch) noexcept;

  /// The operation currently holding this Request, or nullptr when idle. Used by the C ABI cancel path.
  std::shared_ptr<OperationState> ActiveOperation() const;

  /// For the legacy routed `request.canceled = true` shape, atomically choose one epoch under claim_mu_:
  /// return a strong snapshot of its active operation, or write the idle cancelled diagnostic. The caller
  /// releases claim_mu_ before RequestStop; if the returned operation later rejects a post-seal stop there is
  /// deliberately no fallback diagnostic write.
  std::shared_ptr<OperationState> ActiveOperationOrSetIdleCancelled(
      std::memory_order order = std::memory_order_relaxed);

  // --- diagnostic mirror (control-owned; lock-free to read) ---

  /// Read one diagnostic bit. Never blocks.
  bool Diagnostic(DiagnosticBit bit, std::memory_order order = std::memory_order_relaxed) const {
    return (bit == DiagnosticBit::kTimedOut ? timed_out_ : cancelled_).load(order);
  }

  /// Write one diagnostic bit directly. Deliberately bypasses the Request's cancel routing: the caller is
  /// either the operation already delivering the stop or a diagnostic-only legacy path.
  void SetDiagnostic(DiagnosticBit bit, bool value, std::memory_order order = std::memory_order_relaxed) {
    (bit == DiagnosticBit::kTimedOut ? timed_out_ : cancelled_).store(value, order);
  }

  /// Clear both diagnostic bits, serialised against MirrorStop through claim_mu_.
  void ResetDiagnostics();

  /// Mirror a latched stop into the diagnostic bits. Called while OperationState::mu_ is held and validates
  /// the exact state and epoch under claim_mu_, so the diagnostic is committed before Finalize can release
  /// the claim. Never dereferences the Request or invokes cancellation.
  void MirrorStop(const OperationState& state, uint64_t epoch, StopReason reason) noexcept;

 private:
  /// Clear the mirror. Caller holds claim_mu_.
  void ClearDiagnosticsLocked();

  mutable std::mutex claim_mu_;
  std::shared_ptr<OperationState> operation_;
  uint64_t epoch_ = 0;
  bool claimed_ = false;

  /// Accepted-stop mirrors and routed idle writes are serialized by claim_mu_, so neither can contaminate a
  /// later claim. Diagnostic-only compatibility writes remain lock-free. All reads are lock-free and
  /// independent of the owning Request's lifetime.
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> timed_out_{false};
};

}  // namespace fl
