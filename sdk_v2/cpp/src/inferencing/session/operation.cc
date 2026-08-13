// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/operation.h"

#include "exception.h"
#include "inferencing/session/request_control.h"
#include "inferencing/session/request_snapshot.h"
#include "inferencing/session/session_runtime.h"

#include <utility>

namespace fl {

namespace {

void ClearResponse(Response& response) noexcept {
  Response empty;
  response.Swap(empty);
}

}  // namespace

Operation::Operation(std::shared_ptr<SessionRuntime> runtime, std::shared_ptr<RequestControl> request_control,
                     std::shared_ptr<const RequestSnapshot> request, std::shared_ptr<OperationState> state,
                     uint64_t claim_epoch, ResponseVisibility visibility)
    : runtime_(std::move(runtime)),
      request_control_(std::move(request_control)),
      request_(std::move(request)),
      state_(std::move(state)),
      claim_epoch_(claim_epoch),
      visibility_(visibility) {
}

Operation::~Operation() noexcept {
  // An Operation destroyed without being processed abandons its work: settle the state so a late cancel is a
  // no-op, then drop the strong OperationState from the runtime and release the Request claim.
  if (stage_.load(std::memory_order_acquire) == Stage::kFresh) {
    state_->Abandon();
  }

  ReleaseRegistrations();
}

OperationOutcome Operation::Process(Response& response) {
  Stage expected = Stage::kFresh;
  if (!stage_.compare_exchange_strong(expected, Stage::kProcessing, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "operation has already been processed (single-use)");
  }

  // Release the claim and the registration on every exit path, including a throw from inference.
  struct ReleaseGuard {
    Operation& operation;

    ~ReleaseGuard() noexcept {
      operation.stage_.store(Stage::kDone, std::memory_order_release);
      operation.ReleaseRegistrations();
    }
  } release_guard{*this};

  // A reused caller-owned Response must never contribute stale output to this run. Install the release guard
  // first so every exit after response cleanup still releases the operation's registrations.
  ClearResponse(response);

  try {
    // No borrowing, no lifetime gate: the runtime was captured strongly at creation and the execution data
    // is an immutable snapshot this Operation owns.
    const auto outcome = runtime_->Run(state_, request_->Data(), response);

    // Explicit (atomic) operations publish no response unless the run completed: clearing it stops a
    // cancelled/timed-out/faulted operation from surfacing half-built output, and stops stale data from a
    // reused Response object from masquerading as this run's result. The legacy convenience path
    // (kLegacyPartial) deliberately preserves the response this run produced so generated-so-far output
    // survives a cancellation or callback stop, per Session::ProcessRequest's contract.
    if (outcome != OperationOutcome::kCompleted && visibility_ == ResponseVisibility::kExplicitAtomic) {
      ClearResponse(response);
    }

    return outcome;
  } catch (...) {
    // Partial output is a cancellation-only legacy contract. Faults always leave a clean response, including
    // when a modality had already populated a pending legacy result before a later operation failed.
    ClearResponse(response);

    // Backstop only. Run() marks the fault itself for std::exception — it has to, because the fault must be
    // latched *before* it joins the watchdog and runs the derived cleanup hook. This catch covers the
    // non-std throw that would otherwise leave the operation looking runnable. Marking twice is harmless;
    // the original exception propagates unchanged either way.
    state_->MarkFaulted();
    throw;
  }
}

void Operation::Cancel() noexcept {
  state_->RequestStop(StopReason::kExternalCancel);
}

void Operation::ReleaseRegistrations() noexcept {
  if (released_.test_and_set(std::memory_order_acq_rel)) {
    return;
  }

  runtime_->DeregisterOperation(*state_);

  // Identity + epoch scoped: releasing through the control never dereferences the Request itself, so this is
  // safe even if the Request was destroyed while the operation was in flight.
  request_control_->Release(*state_, claim_epoch_);
}

}  // namespace fl
