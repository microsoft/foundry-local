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

#include <chrono>
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

void Session::ProcessRequest(const Request& request, Response& response) {
  // Serialize requests unless the derived class opted into concurrency.
  std::unique_lock<std::mutex> lock(*request_mutex_, std::defer_lock);
  if (!allow_concurrent_requests_) {
    lock.lock();
  }

  // Use the context the caller staged (an HTTP route stages an indirect child
  // with the route's correlation id); otherwise mint a direct context per call
  // for direct SDK use.
  InvocationContext context = request_context_ ? *request_context_ : InvocationContext::Direct();
  context.EnsureCorrelationId();
  const bool streaming = static_cast<bool>(callback_fn_);

  ActionTracker tracker(Action::kSessionProcessRequest, telemetry_, context);
  tracker.SetModelId(CatalogModel().Id());

  const auto start = std::chrono::steady_clock::now();
  try {
    ProcessRequestImpl(request, response);

    tracker.SetStatus(ActionStatus::kSuccess);
  } catch (const std::exception& ex) {
    tracker.RecordException(ex);
    throw;
  }

  // Per-inference Model event — emitted on success with whatever metrics this run
  // produced, sharing the action's correlation id and indirect flag. TTFT and
  // memory are not surfaced by the generators yet and stay at their unset values.
  ModelUsageInfo usage;
  usage.model_id = CatalogModel().Id();
  usage.execution_provider = ExecutionProvider();
  usage.user_agent = context.user_agent;
  usage.correlation_id = context.correlation_id;
  usage.indirect = context.indirect;
  usage.stream = streaming;
  usage.total_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  usage.total_tokens = static_cast<int32_t>(response.usage.total_tokens);
  usage.input_token_count = static_cast<int32_t>(response.usage.prompt_tokens);
  telemetry_.RecordModelUsage(usage);
}

}  // namespace fl
