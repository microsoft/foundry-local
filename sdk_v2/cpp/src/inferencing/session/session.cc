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
#include "items/message_item.h"
#include "manager.h"
#include "model.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_action_tracker.h"
#include "utils.h"

#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <chrono>
#include <algorithm>
#include <memory>
#include <optional>
#include <thread>

namespace fl {

namespace {

std::optional<CancellationState::Clock::time_point> DeadlineFor(
    CancellationState::Clock::time_point entry, std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) {
    return std::nullopt;
  }

  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(CancellationState::Clock::time_point::max() - entry);
  if (timeout > remaining) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "request timeout would overflow the steady-clock deadline");
  }

  return entry + timeout;
}

[[noreturn]] void ThrowForOutcome(CancellationOutcome outcome, std::chrono::milliseconds timeout) {
  if (outcome == CancellationOutcome::kTimedOut) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_TIMEOUT, "request timed out after ", timeout.count(), "ms");
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "request was cancelled");
}

ActionStatus StatusForOutcome(CancellationOutcome outcome) {
  return outcome == CancellationOutcome::kTimedOut ? ActionStatus::kTimeout : ActionStatus::kCanceled;
}

bool FinishConsumerOrThrow(CancellationOutcome outcome, std::chrono::milliseconds timeout, ActionTracker& tracker) {
  if (!IsStopOutcome(outcome)) {
    return false;
  }

  if (outcome == CancellationOutcome::kConsumerStopped) {
    tracker.SetStatus(ActionStatus::kSuccess);
    return true;
  }

  tracker.SetStatus(StatusForOutcome(outcome));
  ThrowForOutcome(outcome, timeout);
}

}  // namespace

Session::Session(const fl::Model& catalog_model, ILogger& logger, ITelemetry& telemetry,
                 bool allow_concurrent_requests)
    : catalog_model_(catalog_model),
      logger_(logger),
      telemetry_(telemetry),
      allow_concurrent_requests_(allow_concurrent_requests),
      control_(std::make_shared<SessionControl>()) {
  LiveSessionRegistry::Instance().Add(control_);
}

// The destination keeps the existing control. The moved-from Session receives a new one.
Session::Session(Session&& other) noexcept
    : catalog_model_(other.catalog_model_),
      logger_(other.logger_),
      telemetry_(other.telemetry_),
      tool_definitions_(std::move(other.tool_definitions_)),
      session_options_(std::move(other.session_options_)),
      callback_fn_(std::move(other.callback_fn_)),
      callback_user_data_(other.callback_user_data_),
      allow_concurrent_requests_(other.allow_concurrent_requests_),
      control_(std::move(other.control_)) {
  other.control_ = std::make_shared<SessionControl>();

  LiveSessionRegistry::Instance().Add(other.control_);
}

Session::~Session() {
  LiveSessionRegistry::Instance().Remove(control_);
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

void Session::ValidateRequestItems(const Request& request) const {
  // Only chat tasks are validated: other tasks either have no IO descriptor (embeddings) or accept
  // transport items that the descriptor does not advertise (the ASR streaming QUEUE item).
  const auto& task = catalog_model_.Info().task;
  if (task != "chat-completion" && task != "vision-language-chat") {
    return;
  }

  // The model's task metadata is the source of truth for which input modalities are accepted.
  const auto io_info = catalog_model_.GetInputOutputInfo();

  // An item type is accepted only if it matches one of the advertised inputs.
  auto check = [&](flItemType type) {
    const bool supported = std::any_of(io_info.inputs, io_info.inputs + io_info.num_inputs,
                                      [type](const Item* input) { return input->type == type; });
    if (!supported) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               fmt::format("{} input is not supported by model task '{}'",
                          Item::TypeName(type), catalog_model_.Info().task));
    }
  };

  // Walk every request item, unwrapping containers so the check always lands on a modality item.
  for (const auto* item : request.items) {
    if (!item) {
      continue;
    }

    switch (item->type) {
      case FOUNDRY_LOCAL_ITEM_MESSAGE:
        // The message wrapper itself is not a modality; validate the parts it carries.
        for (const auto& part : static_cast<const MessageItem&>(*item).content) {
          if (part.view) {
            check(part.view->type);
          }
        }
        break;
      case FOUNDRY_LOCAL_ITEM_TOOL_CALL:
      case FOUNDRY_LOCAL_ITEM_TOOL_RESULT:
        // Tool plumbing, not model input.
        break;
      default:
        check(item->type);
        break;
    }
  }
}

void Session::Cancel() {
  control_->Terminate();
}

void Session::MonitorDeadline(const std::shared_ptr<CancellationState>& state, ILogger& logger,
                             std::chrono::milliseconds timeout) {
  if (state->WaitForDeadline()) {
    logger.Log(LogLevel::Warning, fmt::format("request exceeded its {}ms deadline; cancelling", timeout.count()));
  }
}

void Session::ProcessRequest(const Request& request, Response& response, CancellationState::Clock::time_point entry) {
  const auto timeout = request.Timeout();
  auto state =
      std::make_shared<CancellationState>(DeadlineFor(entry, timeout), control_, request.canceled, request.timed_out);
  request.BeginInvocation(state);

  std::thread deadline_thread;
  struct InvocationCleanup {
    Session& session;
    const Request& request;
    std::shared_ptr<CancellationState> state;
    std::thread& deadline_thread;
    bool registered = false;
    bool holds_inference_slot = false;

    ~InvocationCleanup() {
      state->Fail();

      if (deadline_thread.joinable()) {
        deadline_thread.join();
      }

      if (holds_inference_slot) {
        session.control_->ReleaseInferenceSlot();
      }

      if (registered) {
        session.control_->Unregister(*state);
      }

      request.EndInvocation(*state);
    }
  } cleanup{*this, request, state, deadline_thread};

  ActionTracker tracker(Action::kSessionProcessRequest, telemetry_);
  tracker.SetModelId(CatalogModel().Id());

  const auto finish_if_stopped = [&] {
    return FinishConsumerOrThrow(state->Outcome(), timeout, tracker);
  };

  if (!control_->Register(state)) {
    state->TryStop(CancellationOutcome::kCanceled);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "session has been cancelled");
  }
  cleanup.registered = true;

  if (timeout.count() > 0) {
    deadline_thread = std::thread(&Session::MonitorDeadline, state, std::ref(logger_), timeout);
  }

  if (!allow_concurrent_requests_) {
    const auto admission = control_->AcquireInferenceSlot(*state);
    if (admission == SessionControl::Admission::kTerminal) {
      state->TryStop(CancellationOutcome::kCanceled);
    } else if (admission == SessionControl::Admission::kAdmitted) {
      cleanup.holds_inference_slot = true;
    }
  }

  if (state->ShouldStop() && finish_if_stopped()) {
    return;
  }

  try {
    ValidateRequestItems(request);

    ProcessRequestImpl(request, response);
  } catch (const std::exception& ex) {
    state->Fail();
    if (finish_if_stopped()) {
      return;
    }

    tracker.RecordException(ex);
    throw;
  } catch (...) {
    state->Fail();
    if (finish_if_stopped()) {
      return;
    }

    throw;
  }

  static_cast<void>(state->TryBeginCompletion());

  if (finish_if_stopped()) {
    return;
  }

  state->Complete();
  tracker.SetStatus(ActionStatus::kSuccess);
}

}  // namespace fl
