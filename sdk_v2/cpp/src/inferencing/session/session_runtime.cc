// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session_runtime.h"

#include "exception.h"
#include "inferencing/session/live_session_registry.h"
#include "logger.h"
#include "model.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_action_tracker.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fmt/format.h>
#include <system_error>
#include <thread>
#include <utility>

namespace fl {

SessionRuntime::SessionRuntime(const fl::Model& catalog_model, ILogger& logger, ITelemetry& telemetry,
                               ModelSessionLease lease, bool allow_concurrent_requests)
    : catalog_model_id_(catalog_model.Id()),
      catalog_model_info_(catalog_model.Info()),
      logger_(logger),
      telemetry_(telemetry),
      model_lease_(std::move(lease)),
      allow_concurrent_requests_(allow_concurrent_requests) {
}

SessionRuntime::~SessionRuntime() noexcept {
  // Reaching here means no operation, callback or facade still references this runtime, so there is nothing
  // to wait for — only the registry entry to drop. Derived state and then the model lease are released by
  // the normal destruction order after this body returns.
  LiveSessionRegistry::Instance().Remove(this);
}

void SessionRuntime::UndoTurns(size_t /*count*/) {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "UndoTurns is not supported for this session type");
}

void SessionRuntime::AddToolDefinition(ToolDefinition tool_def) {
  if (!nlohmann::json::accept(tool_def.json_schema)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "ToolDefinition.json_schema is not valid JSON for tool: " + tool_def.name);
  }

  tool_definitions_.push_back(std::move(tool_def));
}

// ===========================================================================
// Operation registry and terminal state
// ===========================================================================

bool SessionRuntime::IsTerminal() const {
  std::lock_guard<std::mutex> lock(mu_);
  return terminal_;
}

bool SessionRuntime::RegisterOperation(const std::shared_ptr<OperationState>& state) {
  std::lock_guard<std::mutex> lock(mu_);
  if (terminal_) {
    return false;
  }

  operations_.push_back(state);
  return true;
}

void SessionRuntime::DeregisterOperation(const OperationState& state) noexcept {
  // Drop the strong ref outside the lock: releasing the last reference here would run ~OperationState under
  // the structural lock, and its slot registry must never nest this one.
  std::shared_ptr<OperationState> released;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(operations_.begin(), operations_.end(),
                           [&](const std::shared_ptr<OperationState>& op) { return op.get() == &state; });
    if (it == operations_.end()) {
      return;
    }

    released = std::move(*it);
    operations_.erase(it);
  }
}

void SessionRuntime::CancelAll() noexcept {
  std::vector<std::shared_ptr<OperationState>> to_cancel;

  {
    std::lock_guard<std::mutex> lock(mu_);
    terminal_ = true;
    to_cancel.swap(operations_);
  }

  // Wake any queued run so it observes the terminal state and bails before entering ProcessRequestImpl.
  admission_cv_.notify_all();

  // Stop outside every framework lock: each stop reaches into the ORT GenAI engine through a cancel lease.
  for (const auto& operation : to_cancel) {
    operation->RequestStop(StopReason::kExternalCancel);
  }
}

void SessionRuntime::MarkTerminal() noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    terminal_ = true;
  }

  admission_cv_.notify_all();
}

// ===========================================================================
// Serial admission gate
// ===========================================================================

OperationOutcome SessionRuntime::AcquireSerialPermit(OperationState& state) {
  StopReason latch = StopReason::kNone;

  {
    std::unique_lock<std::mutex> lock(mu_);
    state.MarkQueued();

    const auto ready = [&] { return !serial_busy_ || terminal_ || state.IsStopped(); };

    if (state.HasDeadline()) {
      // Wait no longer than the operation's absolute deadline, which already includes this queue wait.
      admission_cv_.wait_until(lock, *state.Deadline(), ready);
    } else {
      admission_cv_.wait(lock, ready);
    }

    if (!state.IsStopped()) {
      if (terminal_) {
        latch = StopReason::kExternalCancel;
      } else if (state.DeadlineExpired()) {
        // Re-check the deadline before granting: a permit that becomes free exactly at expiry must not let
        // an already-spent budget into inference.
        latch = StopReason::kTimeout;
      } else {
        serial_busy_ = true;
        return OperationOutcome::kCompleted;
      }
    }
  }

  // Latch outside mu_: RequestStop() calls back into NotifyAdmission() and into the engine.
  if (latch != StopReason::kNone) {
    state.RequestStop(latch);
  }

  return OutcomeForStopReason(state.Reason());
}

void SessionRuntime::ReleaseSerialPermit() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    serial_busy_ = false;
  }

  admission_cv_.notify_all();
}

void SessionRuntime::NotifyAdmission() noexcept {
  // Take mu_ as a synchronization barrier before notifying. A queued operation's stop is latched in the
  // *operation's* atomic (not under mu_), so without this barrier the wakeup could be lost between a
  // waiter's predicate re-check and its sleep. Acquiring mu_ here blocks until any such waiter is parked.
  {
    std::lock_guard<std::mutex> lock(mu_);
  }

  admission_cv_.notify_all();
}

// ===========================================================================
// Execution
// ===========================================================================

OperationOutcome SessionRuntime::Run(const std::shared_ptr<OperationState>& state, const Request& request,
                                     Response& response) {
  const OperationContext operation(state);

  // 1. Admission. Chat/audio serialize through the gate, which wakes for a permit, this operation's stop,
  //    its deadline, or a terminal session cancel — a queued stop never enters ProcessRequestImpl.
  //    Concurrent runtimes (embeddings) skip the gate, so nothing else would re-check an already-spent
  //    budget before inference starts.
  bool holds_permit = false;

  if (!allow_concurrent_requests_) {
    if (AcquireSerialPermit(*state) != OperationOutcome::kCompleted) {
      response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
      return state->Finalize();
    }

    holds_permit = true;
  } else if (state->DeadlineExpired()) {
    state->RequestStop(StopReason::kTimeout);
    response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
    return state->Finalize();
  }

  struct PermitGuard {
    SessionRuntime& runtime;
    const bool& holds;

    ~PermitGuard() noexcept {
      if (holds) {
        runtime.ReleaseSerialPermit();
      }
    }
  } permit_guard{*this, holds_permit};

  // 2. Transition to Running. A stop latched while queued is sticky, so the run is refused here.
  if (!state->MarkRunning()) {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
    return state->Finalize();
  }

  // 3. Start the per-operation deadline watchdog. It touches only the OperationState — never this runtime
  //    and never the Request — so it can never outlive an object it would dereference.
  std::thread watchdog;
  if (state->HasDeadline()) {
    ILogger& logger = logger_;
    const long long budget_ms = static_cast<long long>(state->Budget().count());

    try {
      watchdog = std::thread([state, &logger, budget_ms] {
        // WaitForDeadline latches and mirrors the timeout under its lifecycle lock, then performs admission
        // wake and generator cancellation outside every lock. It returns true only for the call that actually
        // won the race against a completing or sealed run, so only that call logs.
        if (state->WaitForDeadline()) {
          logger.Log(LogLevel::Warning, fmt::format("request exceeded its {}ms deadline; cancelling", budget_ms));
        }
      });
    } catch (const std::system_error& ex) {
      // Without a watchdog the deadline cannot be enforced; fail the operation instead of running unbounded.
      // The permit guard above and Operation's release guard undo the admission and the claim.
      state->MarkFaulted();
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to start the request deadline watchdog: ", ex.what());
    }
  }

  // Finalize on every exit path (including a throw from ProcessRequestImpl): wake and join the watchdog so
  // it can no longer touch this operation's generators, then run the derived cleanup hook.
  struct RunScope {
    SessionRuntime& runtime;
    const OperationContext& operation;
    const Request& request;
    const std::shared_ptr<OperationState>& state;
    std::thread& watchdog;

    ~RunScope() noexcept {
      state->NotifyWatcher();

      if (watchdog.joinable()) {
        watchdog.join();
      }

      runtime.OnRequestFinished(operation, request);
    }
  } run_scope{*this, operation, request, state, watchdog};

  ActionTracker tracker(Action::kSessionProcessRequest, telemetry_);
  tracker.SetModelId(CatalogModelId());

  // A deadline can expire while the watchdog thread is being created. Do not enter modality code once that
  // stop is already visible; this is the final admission check immediately before inference.
  if (operation.ShouldStop()) {
    tracker.SetStatus(ActionStatus::kCanceled);
    response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
    return state->Finalize();
  }

  try {
    ProcessRequestImpl(operation, request, response);

    // Normalize *only* unsealed stopped runs. A sealed run has already committed a real outcome and must
    // keep its finish reason; an unsealed stopped run must not report a natural stop, whatever the modality
    // left behind before it noticed.
    if (operation.ShouldStop() && !operation.IsSealed()) {
      response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
    }

    tracker.SetStatus(operation.IsSealed() || !operation.ShouldStop() ? ActionStatus::kSuccess
                                                                     : ActionStatus::kCanceled);
  } catch (const std::exception& ex) {
    tracker.RecordException(ex);

    // Terminal fault before the watchdog is joined: the operation must never be left looking runnable.
    state->MarkFaulted();
    throw;
  } catch (...) {
    // Preserve non-std failures unchanged, but still settle the operation before RunScope performs cleanup.
    state->MarkFaulted();
    throw;
  }

  return state->Finalize();
}

}  // namespace fl
