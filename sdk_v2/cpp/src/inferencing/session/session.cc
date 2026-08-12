// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session.h"

#include "exception.h"
#include "inferencing/generative/audio/audio_session.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/embeddings/embeddings_session.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/live_session_registry.h"
#include "inferencing/session/session_manager.h"
#include "manager.h"
#include "model.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_action_tracker.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <memory>

namespace fl {

Session::Session(const fl::Model& catalog_model, ILogger& logger, ITelemetry& telemetry,
                 bool allow_concurrent_requests)
    : catalog_model_(catalog_model),
      logger_(logger),
      telemetry_(telemetry),
      allow_concurrent_requests_(allow_concurrent_requests) {
  LiveSessionRegistry::Instance().Add(*this);
}

// Moving relocates the session, so the registry must follow the object's address.
// The moved-from shell stays registered until its own destructor runs; cancelling it is
// harmless because its cancel state moved with it.
Session::Session(Session&& other) noexcept
    : catalog_model_(other.catalog_model_),
      logger_(other.logger_),
      telemetry_(other.telemetry_),
      tool_definitions_(std::move(other.tool_definitions_)),
      session_options_(std::move(other.session_options_)),
      callback_fn_(std::move(other.callback_fn_)),
      callback_user_data_(other.callback_user_data_),
      allow_concurrent_requests_(other.allow_concurrent_requests_),
      request_mutex_(std::move(other.request_mutex_)),
      cancel_state_(std::move(other.cancel_state_)) {
  // Give the moved-from shell fresh state rather than null pointers: it stays live (and
  // reachable from the registry) until its destructor runs, and Cancel() may race with it.
  other.request_mutex_ = std::make_unique<std::mutex>();
  other.cancel_state_ = std::make_unique<CancelState>();

  LiveSessionRegistry::Instance().Add(*this);
}

Session::~Session() {
  LiveSessionRegistry::Instance().Remove(*this);
}

std::unique_ptr<Session> Session::Create(const fl::Model& model) {
  auto& mgr = Manager::Instance();
  auto& telemetry = mgr.GetTelemetry();
  ActionTracker tracker(Action::kSessionCreate, telemetry);

  auto& logger = mgr.GetLogger();

  try {
    if (mgr.IsShutdownRequested()) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                       "cannot create session during shutdown");
    }

    if (!model.IsLoaded()) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model must be loaded before creating a session");
    }

    auto& lm = mgr.GetModelLoadManager();
    auto* loaded = lm.GetLoadedModel(model.Id());
    if (!loaded) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL, "loaded model not found in load manager");
    }

    tracker.SetModelId(model.Id());

    const auto& info = model.Info();
    if (info.task == "chat-completion" || info.task == "vision-language-chat") {
      auto session = std::make_unique<ChatSession>(model, *loaded, logger, telemetry);
      tracker.SetStatus(ActionStatus::kSuccess);
      return session;
    }

    if (info.task == "automatic-speech-recognition") {
      auto session = std::make_unique<AudioSession>(model, *loaded, logger, telemetry);
      tracker.SetStatus(ActionStatus::kSuccess);
      return session;
    }

    if (info.task == "embeddings") {
      auto session = std::make_unique<EmbeddingsSession>(model, *loaded, logger, telemetry);
      tracker.SetStatus(ActionStatus::kSuccess);
      return session;
    }

    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported model task: ", info.task);
  } catch (const std::exception& ex) {
    tracker.RecordException(ex);
    throw;
  }
}

void Session::UndoTurns(size_t /*count*/) {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "UndoTurns is not supported for this session type");
}

void Session::AddToolDefinition(ToolDefinition tool_def) {
  if (!nlohmann::json::accept(tool_def.json_schema)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "ToolDefinition.json_schema is not valid JSON for tool: " + tool_def.name);
  }

  tool_definitions_.push_back(std::move(tool_def));
}

void Session::Cancel() {
  std::vector<ICancellable*> generators;
  std::vector<const Request*> requests;

  {
    std::lock_guard<std::mutex> lock(cancel_state_->mutex);
    cancel_state_->cancel_requested = true;
    generators = cancel_state_->active_generators;
    requests = cancel_state_->active_requests_list;
  }

  // Latch the flag on every in-flight request, not just the generators. Interrupting the
  // engine alone is not enough: the loops would exit but the run would still be reported
  // as a natural stop, and cancelled turns would be committed to history. Cancellation
  // during prefill is exactly this case — it produces no tokens, so nothing else marks it.
  for (const auto* request : requests) {
    request->canceled.store(true, std::memory_order_relaxed);
  }

  // Wake the deadline watchdog so it exits instead of sleeping out the full budget.
  cancel_state_->cv.notify_all();

  // Cancel outside the lock: each Cancel() reaches into ORT GenAI, and holding the
  // mutex across that would block the request thread's generator bookkeeping.
  for (auto* generator : generators) {
    generator->Cancel();
  }
}

void Session::AddActiveGenerator(ICancellable* generator) {
  bool cancel_now = false;

  {
    std::lock_guard<std::mutex> lock(cancel_state_->mutex);
    cancel_state_->active_generators.push_back(generator);

    // Cancel() may have landed between the caller's pre-flight check and this
    // publication. Apply the pending stop to the newly-visible generator so the
    // request cannot slip past an already-issued cancellation.
    cancel_now = cancel_state_->cancel_requested;
  }

  if (cancel_now) {
    generator->Cancel();
  }
}

void Session::RemoveActiveGenerator(ICancellable* generator) {
  std::lock_guard<std::mutex> lock(cancel_state_->mutex);
  auto& generators = cancel_state_->active_generators;
  generators.erase(std::remove(generators.begin(), generators.end(), generator), generators.end());
}

void Session::WatchDeadline(const Request& request) {
  const auto timeout = request.Timeout();

  std::unique_lock<std::mutex> lock(cancel_state_->mutex);

  // Wait out the budget, but wake early if the request finished or was cancelled —
  // otherwise ProcessRequest would block on joining this thread for the full timeout.
  const bool woken = cancel_state_->cv.wait_for(lock, timeout, [this] {
    return cancel_state_->active_requests == 0 || cancel_state_->cancel_requested;
  });

  if (woken) {
    return;
  }

  // Deadline expired. Latch the timeout on the request so generation loops stop at the
  // next token boundary, and cancel the active generators to interrupt an in-flight
  // compute (a long prefill can exceed the budget without ever reaching a boundary).
  request.canceled.store(true, std::memory_order_relaxed);
  request.timed_out.store(true, std::memory_order_relaxed);

  auto generators = cancel_state_->active_generators;
  lock.unlock();

  logger_.Log(LogLevel::Warning,
              fmt::format("request exceeded its {}ms deadline; cancelling", timeout.count()));

  for (auto* generator : generators) {
    generator->Cancel();
  }
}

void Session::ProcessRequest(const Request& request, Response& response) {
  // Serialize requests unless the derived class opted into concurrency.
  std::unique_lock<std::mutex> lock(*request_mutex_, std::defer_lock);
  if (!allow_concurrent_requests_) {
    lock.lock();
  }

  // A session cancelled during teardown must not start new work — otherwise a caller
  // looping over requests could keep the model refcount pinned past Manager::Shutdown.
  {
    std::lock_guard<std::mutex> cancel_lock(cancel_state_->mutex);
    if (cancel_state_->cancel_requested) {
      request.canceled.store(true, std::memory_order_relaxed);
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "session has been cancelled");
    }

    ++cancel_state_->active_requests;
    cancel_state_->active_requests_list.push_back(&request);
  }

  // Start the wall-clock budget (if any) and clear stale stop state from a prior run.
  request.ArmDeadline();

  // Watchdog enforces the deadline for paths that would otherwise block indefinitely
  // inside the engine. Only started when a timeout was requested — no cost otherwise.
  std::thread watchdog;
  if (request.Timeout().count() > 0) {
    watchdog = std::thread(&Session::WatchDeadline, this, std::cref(request));
  }

  // Guarantees the watchdog is woken and joined on every exit path, including throws.
  // Declared after the thread so it runs first on unwind.
  struct RunScope {
    Session& session;
    const Request& request;
    std::thread& watchdog;

    ~RunScope() {
      {
        std::lock_guard<std::mutex> lock(session.cancel_state_->mutex);
        --session.cancel_state_->active_requests;

        auto& list = session.cancel_state_->active_requests_list;
        list.erase(std::remove(list.begin(), list.end(), &request), list.end());
      }

      session.cancel_state_->cv.notify_all();

      if (watchdog.joinable()) {
        watchdog.join();
      }

      request.DisarmDeadline();
    }
  } run_scope{*this, request, watchdog};

  ActionTracker tracker(Action::kSessionProcessRequest, telemetry_);
  tracker.SetModelId(CatalogModel().Id());

  try {
    ProcessRequestImpl(request, response);

    tracker.SetStatus(request.canceled.load(std::memory_order_relaxed) ? ActionStatus::kCanceled
                                                                      : ActionStatus::kSuccess);
  } catch (const std::exception& ex) {
    tracker.RecordException(ex);
    throw;
  }

  // A timeout is a failure of the caller's contract, not a silent truncation: surface it
  // so callers can distinguish "the model stopped early" from "we ran out of time".
  if (request.timed_out.load(std::memory_order_relaxed)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_TIMEOUT, "request timed out after ", request.Timeout().count(), "ms");
  }
}

}  // namespace fl
