// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session.h"

#include "exception.h"
#include "inferencing/generative/audio/audio_session.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/embeddings/embeddings_session.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session_manager.h"
#include "manager.h"
#include "model.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_action_tracker.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <memory>

namespace fl {

std::optional<SessionType> ClassifySessionTask(std::string_view task) {
  if (IsTextGenerationTask(task) || task == "vision-language-chat") {
    return SessionType::kChat;
  }

  if (task == "automatic-speech-recognition") {
    return SessionType::kAudio;
  }

  if (task == "embeddings") {
    return SessionType::kEmbeddings;
  }

  return std::nullopt;
}

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
    const auto session_type = ClassifySessionTask(info.task);
    if (!session_type) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported model task: ", info.task);
    }

    std::unique_ptr<Session> session;
    switch (*session_type) {
      case SessionType::kChat:
        session = std::make_unique<ChatSession>(model, *loaded, logger, telemetry);
        break;
      case SessionType::kAudio:
        session = std::make_unique<AudioSession>(model, *loaded, logger, telemetry);
        break;
      case SessionType::kEmbeddings:
        session = std::make_unique<EmbeddingsSession>(model, *loaded, logger, telemetry);
        break;
      case SessionType::kPredictive:
        FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED,
                         "predictive sessions are not yet implemented, task: ", info.task);
    }

    tracker.SetStatus(ActionStatus::kSuccess);
    return session;
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

void Session::ProcessRequest(const Request& request, Response& response) {
  // Serialize requests unless the derived class opted into concurrency.
  std::unique_lock<std::mutex> lock(*request_mutex_, std::defer_lock);
  if (!allow_concurrent_requests_) {
    lock.lock();
  }

  ActionTracker tracker(Action::kSessionProcessRequest, telemetry_);
  tracker.SetModelId(CatalogModel().Id());

  try {
    ProcessRequestImpl(request, response);

    tracker.SetStatus(ActionStatus::kSuccess);
  } catch (const std::exception& ex) {
    tracker.RecordException(ex);
    throw;
  }
}

}  // namespace fl
