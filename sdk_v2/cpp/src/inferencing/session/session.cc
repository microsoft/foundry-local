// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session.h"

#include "exception.h"
#include "inferencing/generative/audio/audio_session.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/embeddings/embeddings_session.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session_manager.h"
#include "items/message_item.h"
#include "manager.h"
#include "model.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_action_tracker.h"
#include "utils.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>

namespace fl {

Session::Session(const fl::Model& catalog_model, ILogger& logger, ITelemetry& telemetry,
                 bool allow_concurrent_requests)
    : catalog_model_(catalog_model),
      logger_(logger),
      telemetry_(telemetry),
      allow_concurrent_requests_(allow_concurrent_requests) {
}

Session::~Session() = default;

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

void Session::ProcessRequest(const Request& request, Response& response) {
  // Serialize requests unless the derived class opted into concurrency.
  std::unique_lock<std::mutex> lock(*request_mutex_, std::defer_lock);
  if (!allow_concurrent_requests_) {
    lock.lock();
  }

  {
    std::lock_guard<std::mutex> active_lock(*active_requests_mutex_);
    active_requests_.insert(&request);

    // If Cancel() already ran (shutdown began before this request was admitted), stamp it now so the
    // generation loop exits at its first poll instead of running an uncanceled turn.
    if (session_canceled_) {
      request.canceled.store(true, std::memory_order_relaxed);
    }
  }

  // RAII: deregister the request even if ProcessRequestImpl throws, so Cancel() never
  // dereferences a dangling Request after this call unwinds.
  struct ActiveRequestGuard {
    Session& session;
    const Request& request;
    ~ActiveRequestGuard() {
      std::lock_guard<std::mutex> active_lock(*session.active_requests_mutex_);
      session.active_requests_.erase(&request);
    }
  } active_guard{*this, request};

  ActionTracker tracker(Action::kSessionProcessRequest, telemetry_);
  tracker.SetModelId(CatalogModel().Id());

  try {
    ValidateRequestItems(request);

    ProcessRequestImpl(request, response);

    tracker.SetStatus(ActionStatus::kSuccess);
  } catch (const std::exception& ex) {
    tracker.RecordException(ex);
    throw;
  }
}

void Session::Cancel() {
  // Only flip cancel flags — never block or join — so this is safe to call while the
  // SessionManager holds its own lock during shutdown. Generation loops poll the flag.
  std::lock_guard<std::mutex> lock(*active_requests_mutex_);
  session_canceled_ = true;
  for (const Request* r : active_requests_) {
    r->canceled.store(true, std::memory_order_relaxed);
  }
}

}  // namespace fl
