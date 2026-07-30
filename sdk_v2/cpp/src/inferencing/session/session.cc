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
#include "items/text_item.h"
#include "items/tool_result_item.h"
#include "manager.h"
#include "model.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_action_tracker.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace fl {

namespace {

uint64_t CountOpenAIJsonMessages(std::string_view text) {
  auto json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    return 0;
  }

  uint64_t count = 0;
  const auto messages = json.find("messages");
  if (messages != json.end() && messages->is_array()) {
    count += messages->size();
  }

  return count;
}

uint64_t CountRequestMessages(const Request& request) {
  uint64_t count = 0;
  for (const auto* item : request.items) {
    if (item == nullptr) {
      continue;
    }

    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE || item->type == FOUNDRY_LOCAL_ITEM_TOOL_RESULT) {
      ++count;
      continue;
    }

    if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
      const auto& text_item = static_cast<const TextItem&>(*item);
      if (text_item.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
        count += CountOpenAIJsonMessages(text_item.text);
      }
    }
  }
  return count;
}

}  // namespace

Session::Session(const fl::Model& catalog_model, ILogger& logger, ITelemetry& telemetry,
                 bool allow_concurrent_requests)
    : catalog_model_(catalog_model),
      logger_(logger),
      telemetry_(telemetry),
      allow_concurrent_requests_(allow_concurrent_requests) {
}

Session::~Session() = default;

Session::Session(Session&& other) noexcept
    : catalog_model_(other.catalog_model_),
      logger_(other.logger_),
      telemetry_(other.telemetry_),
      tool_definitions_(std::move(other.tool_definitions_)),
      session_options_(std::move(other.session_options_)),
      callback_fn_(std::move(other.callback_fn_)),
      callback_user_data_(other.callback_user_data_),
      allow_concurrent_requests_(other.allow_concurrent_requests_) {
  std::lock_guard<std::mutex> lock(other.request_context_mutex_);
  request_context_ = std::move(other.request_context_);
  other.request_context_.reset();
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

void Session::ProcessRequest(const Request& request, Response& response) {
  // Serialize requests unless the derived class opted into concurrency.
  std::unique_lock<std::mutex> lock(*request_mutex_, std::defer_lock);
  if (!allow_concurrent_requests_) {
    lock.lock();
  }

  struct ActiveRequestGuard {
    Session& session;
    const Request& request;
    ActiveRequestGuard(Session& session, const Request& request) : session(session), request(request) {
      std::lock_guard<std::mutex> lock(session.active_requests_mutex_);
      session.active_requests_.insert(&request);
      if (session.session_canceled_) {
        request.canceled.store(true, std::memory_order_relaxed);
      }
    }
    ~ActiveRequestGuard() {
      std::lock_guard<std::mutex> lock(session.active_requests_mutex_);
      session.active_requests_.erase(&request);
    }
  } active_request_guard(*this, request);

  // Use the context the caller staged (an HTTP route stages an indirect child
  // with the route's correlation id); otherwise mint a direct context per call
  // for direct SDK use.
  InvocationContext context;
  {
    std::lock_guard<std::mutex> context_lock(request_context_mutex_);
    context = request_context_ ? *request_context_ : InvocationContext::Direct();
    request_context_.reset();
  }
  context.EnsureCorrelationId();
  const bool streaming = static_cast<bool>(callback_fn_);

  ActionTracker tracker(Action::kSessionProcessRequest, telemetry_, context);
  tracker.SetModelId(CatalogModel().Id());

  const auto start = std::chrono::steady_clock::now();
  try {
    ProcessRequestImpl(request, response);

    tracker.SetStatus(request.canceled.load(std::memory_order_relaxed) ? ActionStatus::kCanceled
                                                                       : ActionStatus::kSuccess);
  } catch (const std::exception& ex) {
    tracker.RecordException(ex);
    throw;
  }

  const auto inference_end = std::chrono::steady_clock::now();

  // Per-inference Model event — emitted on success with whatever metrics this run
  // produced, sharing the action's correlation id and indirect flag. TTFT and
  // memory are not surfaced by the generators yet and stay at their unset values.
  ModelUsageInfo usage;
  usage.model_id = CatalogModel().Id();
  usage.execution_provider = ExecutionProvider();
  if (usage.execution_provider.empty()) {
    usage.execution_provider = CatalogModel().Info().execution_provider;
  }
  usage.user_agent = context.user_agent;
  usage.correlation_id = context.correlation_id;
  usage.indirect = context.indirect;
  usage.stream = streaming;
  usage.num_messages = CountRequestMessages(request);
  usage.total_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(inference_end - start).count();
  usage.total_tokens = static_cast<int32_t>(response.usage.total_tokens);
  usage.input_token_count = static_cast<int32_t>(response.usage.prompt_tokens);
  try {
    telemetry_.RecordModelUsage(usage);
    RecordAdditionalModelUsage(request, response, context, usage.total_time_ms, streaming);
  } catch (...) {
    // Telemetry is best-effort and must not turn successful inference into an API failure.
  }
}

void Session::Cancel() {
  std::lock_guard<std::mutex> lock(active_requests_mutex_);
  session_canceled_ = true;
  for (const Request* request : active_requests_) {
    request->canceled.store(true, std::memory_order_relaxed);
  }
}

}  // namespace fl
