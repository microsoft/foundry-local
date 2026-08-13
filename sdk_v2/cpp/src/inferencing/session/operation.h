// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/operation_state.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace fl {

class RequestControl;
class RequestSnapshot;
class SessionRuntime;
struct Response;

/// Controls whether a stopped run's partial response is visible to the caller.
///
///   - kExplicitAtomic (the deferred Operation API): a run that does not complete publishes *no* response.
///     Process() clears it so a cancelled/timed-out/faulted operation never surfaces half-built output, and a
///     reused Response object cannot masquerade as the stopped run's result.
///   - kLegacyPartial (the Session::ProcessRequest convenience): a cancellation or callback stop returns
///     normally and preserves the generated-so-far response the modality publishes, matching the established
///     streaming/SSE contract where a client hanging up is not an error. Process() still clears stale caller
///     output before starting; only partial data produced by this run can survive its stop.
enum class ResponseVisibility : uint8_t {
  kExplicitAtomic,
  kLegacyPartial,
};

/// A single-use, per-invocation unit of inference work.
///
/// Created synchronously by Session::CreateOperation (which snapshots the deadline, claims the Request,
/// captures the immutable execution data and registers a strong OperationState with the runtime) and then
/// executed exactly once via Process(). This is the internal core the ABI layer wraps as an opaque
/// flOperation so it can build a create -> (optionally cancel) -> process flow instead of the current
/// single-shot ProcessRequest call.
///
/// Lifetime: an Operation holds **no Session pointer and no Request pointer**. It holds
///   - a strong `shared_ptr<SessionRuntime>`, captured synchronously at creation, so the executable state it
///     runs against cannot disappear no matter what the caller does with the Session facade, and
///   - a strong `shared_ptr<const RequestSnapshot>`, so the execution data is immutable and independently
///     owned, and
///   - the Request's stable RequestControl, used only for claim release and diagnostics by identity+epoch.
/// Destroying an unprocessed Operation abandons it, releasing exactly its request claim (by identity and
/// epoch) and its runtime registration.
class Operation {
 public:
  Operation(std::shared_ptr<SessionRuntime> runtime, std::shared_ptr<RequestControl> request_control,
            std::shared_ptr<const RequestSnapshot> request, std::shared_ptr<OperationState> state,
            uint64_t claim_epoch, ResponseVisibility visibility = ResponseVisibility::kExplicitAtomic);
  ~Operation() noexcept;

  Operation(const Operation&) = delete;
  Operation& operator=(const Operation&) = delete;
  Operation(Operation&&) = delete;
  Operation& operator=(Operation&&) = delete;

  /// Execute the operation exactly once, filling `response`.
  ///
  /// If the operation was already stopped (sticky created/queued cancellation) it returns the corresponding
  /// outcome without running any inference. Genuine inference errors propagate unchanged (the operation is
  /// left kFaulted); cancellation, callback stop and timeout are reported through the returned outcome so
  /// callers can branch on them — in particular kCancelled stays visible here, which is what the explicit
  /// operation API maps to FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED.
  ///
  /// @throws fl::Exception (FOUNDRY_LOCAL_ERROR_INVALID_USAGE) if called more than once.
  OperationOutcome Process(Response& response);

  /// Cancel just this operation. Thread-safe, idempotent, signal-only. Sticky before the run starts; a
  /// no-op once the operation has reached any terminal status or has been sealed.
  void Cancel() noexcept;

  bool IsProcessed() const { return stage_.load(std::memory_order_acquire) != Stage::kFresh; }

  OperationStatus Status() const { return state_->Status(); }

  const std::shared_ptr<OperationState>& State() const { return state_; }

 private:
  /// Single-use gate. Advanced with a CAS so two threads racing on Process() cannot both run.
  enum class Stage : uint8_t { kFresh, kProcessing, kDone };

  /// Deregister from the runtime and release the exact request claim. Idempotent.
  void ReleaseRegistrations() noexcept;

  std::shared_ptr<SessionRuntime> runtime_;
  std::shared_ptr<RequestControl> request_control_;
  std::shared_ptr<const RequestSnapshot> request_;
  std::shared_ptr<OperationState> state_;
  const uint64_t claim_epoch_;
  const ResponseVisibility visibility_;
  std::atomic<Stage> stage_{Stage::kFresh};
  std::atomic_flag released_;  // C++20: value-initialised clear
};

}  // namespace fl
