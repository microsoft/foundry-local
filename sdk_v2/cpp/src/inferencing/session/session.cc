// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session.h"

#include "exception.h"
#include "inferencing/generative/audio/audio_session.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/embeddings/embeddings_session.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/operation.h"
#include "inferencing/session/request_control.h"
#include "inferencing/session/request_snapshot.h"
#include "logger.h"
#include "manager.h"
#include "model.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_action_tracker.h"
#include "utils.h"

#include <chrono>
#include <memory>
#include <optional>
#include <utility>

namespace fl {

Session::~Session() noexcept {
  if (!runtime_) {
    return;  // moved-from shell: the destination owns the runtime
  }

  // Signal-only, and deliberately so. Cancelling here guarantees that dropping the last handle to a session
  // stops a runaway generation instead of leaving it pinning the model, and returning immediately is what
  // makes it legal to release a Session from inside its own streaming callback. Physical destruction of the
  // runtime — and of the model lease it holds — happens when the last operation/callback reference goes.
  runtime_->CancelAll();
}

std::unique_ptr<Session> Session::Create(const fl::Model& model) {
  auto& mgr = Manager::Instance();
  auto& telemetry = mgr.GetTelemetry();
  ActionTracker tracker(Action::kSessionCreate, telemetry);

  auto& logger = mgr.GetLogger();

  try {
    if (mgr.IsShutdownRequested()) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot create session during shutdown");
    }

    if (!model.IsLoaded()) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model must be loaded before creating a session");
    }

    // Acquire the lease *before* building anything: the load check and the lease increment happen in one
    // critical section inside the manager, so the model cannot be unloaded out from under the runtime that
    // is about to adopt it.
    auto lease = mgr.GetModelLoadManager().AcquireLoadedModel(model.Id());
    if (!lease.has_value()) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL, "loaded model not found in load manager");
    }

    tracker.SetModelId(model.Id());

    const auto& info = model.Info();
    if (info.task == "chat-completion" || info.task == "vision-language-chat") {
      auto session = std::make_unique<ChatSession>(model, std::move(*lease), logger, telemetry);
      tracker.SetStatus(ActionStatus::kSuccess);
      return session;
    }

    if (info.task == "automatic-speech-recognition") {
      auto session = std::make_unique<AudioSession>(model, std::move(*lease), logger, telemetry);
      tracker.SetStatus(ActionStatus::kSuccess);
      return session;
    }

    if (info.task == "embeddings") {
      auto session = std::make_unique<EmbeddingsSession>(model, std::move(*lease), logger, telemetry);
      tracker.SetStatus(ActionStatus::kSuccess);
      return session;
    }

    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported model task: ", info.task);
  } catch (const std::exception& ex) {
    tracker.RecordException(ex);
    throw;
  }
}

void Session::Cancel() {
  if (!runtime_) {
    return;
  }

  // Terminal, signal-only: mark the runtime terminal and stop every registered operation. Never waits.
  runtime_->CancelAll();
}

std::unique_ptr<Operation> Session::CreateOperation(const Request& request) {
  return CreateOperationImpl(request, /*allow_borrowed_items=*/false, ResponseVisibility::kExplicitAtomic);
}

std::unique_ptr<Operation> Session::CreateOperationImpl(const Request& request, bool allow_borrowed_items,
                                                        ResponseVisibility visibility) {
  // Capture the implementation before doing any work. ProcessRequest subsequently runs only through the
  // Operation's strong reference, so a callback may release the facade without ending the executable state.
  const auto runtime = runtime_;
  if (!runtime) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "session has been moved from and can no longer run requests");
  }

  // Reject a terminal session up front. RegisterOperation re-checks atomically to close the race where the
  // session goes terminal between here and registration.
  if (runtime->IsTerminal()) {
    // Preserve the legacy late-admission diagnostic without disturbing a Request claimed by another operation.
    if (const auto& request_control = request.Control()) {
      static_cast<void>(request_control->ActiveOperationOrSetIdleCancelled());
    }

    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "session has been cancelled");
  }

  // Snapshot the timeout into an absolute deadline immediately so the budget covers any serialized queue
  // wait before the run actually starts. The budget itself is kept for logs and error messages.
  const auto timeout = request.Timeout();
  std::optional<std::chrono::steady_clock::time_point> deadline;
  if (timeout.count() > 0) {
    deadline = std::chrono::steady_clock::now() + timeout;
  }

  const std::shared_ptr<RequestControl>& request_control = request.Control();
  if (!request_control) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "request has been moved from and can no longer be used");
  }

  auto state = std::make_shared<OperationState>(runtime, request_control, deadline, timeout);

  // Claim the Request. A second concurrent operation on the same Request is rejected. A won claim also
  // clears the previous run's diagnostic mirror under the claim lock, so those flags always describe the
  // operation that currently owns the Request.
  const auto claim = request_control->TryClaim(state);
  if (!claim.claimed) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "request is already in use by another operation");
  }

  try {
    // Capture execution data now, while the caller is still inside this synchronous call. Everything the run
    // needs is either shared-owned or deep-cloned here; the operation never reads the caller's Request again.
    auto snapshot = RequestSnapshot::Capture(request);

    if (!allow_borrowed_items && snapshot->HasBorrowedItems()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "deferred operations require owned or clonable request items; use AddOwnedItem or the "
               "synchronous ProcessRequest convenience for non-clonable borrowed items");
    }

    if (!runtime->RegisterOperation(state)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "session has been cancelled");
    }

    return std::make_unique<Operation>(runtime, request_control, std::move(snapshot), state, claim.epoch,
                                       visibility);
  } catch (...) {
    // Roll back everything this call took: a terminal session, a failed snapshot capture, or a failed
    // allocation of either the registration vector entry or the Operation itself must not leave the Request
    // claimed or the state registered.
    runtime->DeregisterOperation(*state);
    request_control->Release(*state, claim.epoch);
    throw;
  }
}

void Session::ProcessRequest(const Request& request, Response& response) {
  auto operation = CreateOperationImpl(request, /*allow_borrowed_items=*/true, ResponseVisibility::kLegacyPartial);
  const OperationOutcome outcome = operation->Process(response);

  // Legacy convenience contract, preserved exactly. A deadline is the only stop that becomes an exception
  // here: a request that outlived its budget is a broken caller contract. Every other stop — an explicit
  // session/operation/request cancel, or a streaming callback hanging up — is a *normal* return with
  // finish_reason == NONE, which is what keeps the existing SSE behaviour intact. The explicit
  // Operation::Process() path still surfaces kCancelled so the operation API can map it separately.
  if (outcome == OperationOutcome::kTimedOut) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_TIMEOUT, "request timed out after ", operation->State()->Budget().count(), "ms");
  }

  if (outcome == OperationOutcome::kAbandoned) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "request was abandoned before it could run");
  }

  if (outcome != OperationOutcome::kCompleted) {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
  }
}

SessionRuntime& Session::CheckedRuntime() const {
  if (!runtime_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "session has been moved from and can no longer be used");
  }

  return *runtime_;
}

}  // namespace fl
