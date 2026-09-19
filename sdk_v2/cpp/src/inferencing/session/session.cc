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
#include "manager.h"
#include "model.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_action_tracker.h"
#include "util/scope_guard.h"
#include "utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>

namespace fl {

namespace {

void LogUsageTelemetryFailure(ILogger& logger) noexcept {
  try {
    logger.Log(LogLevel::Warning, "Unable to record inference usage telemetry");
  } catch (...) {
    // Even a caller-provided diagnostic logger must not change a completed inference result.
  }
}

struct RequestMessageCount {
  uint64_t count = 0;
};

void from_json(const nlohmann::json& json, RequestMessageCount& result) {
  const auto messages = json.find("messages");
  if (messages == json.end()) {
    return;
  }
  if (!messages->is_array()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "messages must be an array");
  }
  result.count = messages->size();
}

uint64_t CountRequestMessages(const Request& request) {
  uint64_t count = 0;
  for (const auto* item : request.items) {
    if (!item) {
      continue;
    }

    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE || item->type == FOUNDRY_LOCAL_ITEM_TOOL_RESULT) {
      ++count;
    } else if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
      const auto& text = static_cast<const TextItem&>(*item);
      if (text.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
        count += nlohmann::json::parse(text.text).get<RequestMessageCount>().count;
      }
    }
  }

  return count;
}

[[noreturn]] void ThrowCancellation(const Request& request) {
  switch (request.GetCancellationReason()) {
    case Request::CancellationReason::StreamingCallback:
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "request cancelled by streaming callback");
    case Request::CancellationReason::StreamingCallbackException: {
      const auto detail = request.CancellationDetail();
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               detail.empty() ? "streaming callback threw an exception"
                              : fmt::format("streaming callback threw an exception: {}", detail));
    }
    case Request::CancellationReason::SessionShutdown:
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "request cancelled because the session is shutting down");
    case Request::CancellationReason::None:
    case Request::CancellationReason::Caller:
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "request cancelled by caller");
  }
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

    auto* loaded = mgr.GetModelLoadManager().GetLoadedModel(model.Id(), model.GetPath());
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

std::unique_ptr<Session::RequestPreflightOperation> Session::CreateRequestPreflight(const Request& request) const {
  auto lock = LockRequestMutex();
  if (Type() != SessionType::kChat) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "request preflight is only supported for chat sessions");
  }

  auto snapshot = request.CaptureChatSnapshot();
  ValidateRequestItems(snapshot);
  return CreateRequestPreflightImpl(std::move(snapshot));
}

std::unique_ptr<Session::RequestPreflightOperation> Session::CreateRequestPreflightImpl(Request) const {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "request preflight is only supported for chat sessions");
}

void Session::AddToolDefinition(ToolDefinition tool_def) {
  tool_registry_.Add(std::move(tool_def));
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

  bool admitted = false;
  bool registered = false;
  auto finish_lifecycle = [&]() noexcept {
    if (!admitted) {
      return;
    }

    if (registered) {
      std::lock_guard<std::mutex> active_lock(*active_requests_mutex_);
      active_requests_.erase(&request);
      registered = false;
    }

    request.PublishCompletion();
    admitted = false;
  };
  ScopeGuard lifecycle_guard([&]() noexcept { finish_lifecycle(); });

  {
    std::lock_guard<std::mutex> active_lock(*active_requests_mutex_);
    if (!request.TryBegin()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "request is already being processed");
    }
    admitted = true;

    // A late shutdown admission must first claim the request, then be canceled under the same lock that protects
    // active registration. This keeps TryBegin as the sole admission gate while preventing backend entry.
    if (session_canceled_) {
      request.Cancel(Request::CancellationReason::SessionShutdown);
    }

    active_requests_.insert(&request);
    registered = true;
  }

  ActionTracker tracker(Action::kSessionProcessRequest, telemetry_, TakeInvocationContext());
  tracker.SetModelId(CatalogModel().Id());

  const auto start = std::chrono::steady_clock::now();
  Response staged_response;
  try {
    if (request.IsCancellationRequested()) {
      ThrowCancellation(request);
    }

    ValidateRequestItems(request);

    ProcessRequestImpl(request, staged_response);

    if (!request.TryComplete()) {
      ThrowCancellation(request);
    }

    response = std::move(staged_response);
    finish_lifecycle();
    lifecycle_guard.Dismiss();
    tracker.SetStatus(ActionStatus::kSuccess);
  } catch (const std::exception& ex) {
    if (request.TryComplete()) {
      finish_lifecycle();
      lifecycle_guard.Dismiss();
      tracker.RecordException(ex);
      throw;
    }

    try {
      ThrowCancellation(request);
    } catch (const std::exception& cancellation) {
      finish_lifecycle();
      lifecycle_guard.Dismiss();
      tracker.RecordException(cancellation);
      throw;
    }
  } catch (...) {
    if (request.TryComplete()) {
      finish_lifecycle();
      lifecycle_guard.Dismiss();
      throw;
    }

    try {
      ThrowCancellation(request);
    } catch (const std::exception& cancellation) {
      finish_lifecycle();
      lifecycle_guard.Dismiss();
      tracker.RecordException(cancellation);
      throw;
    }
  }

  const auto elapsed = std::chrono::steady_clock::now() - start;
  RecordUsage(request, response, tracker.Context(),
              std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

InvocationContext Session::TakeInvocationContext() {
  std::lock_guard<std::mutex> lock(*invocation_context_mutex_);
  auto context = invocation_context_ ? std::move(*invocation_context_) : InvocationContext::Direct();
  invocation_context_.reset();
  return context;
}

int32_t Session::TelemetryTokenCount(int64_t count) {
  return static_cast<int32_t>(std::clamp<int64_t>(count, 0, std::numeric_limits<int32_t>::max()));
}

void Session::RecordUsage(const Request& request, const Response& response,
                          const InvocationContext& context, int64_t total_time_ms) {
  // This boundary covers metric preparation as well as emission; neither may change inference results.
  try {
    ModelUsageInfo usage;
    usage.model_id = CatalogModel().Id();
    usage.execution_provider = ExecutionProvider();
    if (usage.execution_provider.empty()) {
      usage.execution_provider = CatalogModel().Info().execution_provider;
    }

    usage.user_agent = context.user_agent;
    usage.correlation_id = context.correlation_id;
    usage.indirect = context.indirect;
    usage.stream = static_cast<bool>(callback_fn_);
    usage.num_messages = CountRequestMessages(request);
    usage.total_time_ms = total_time_ms;
    usage.total_tokens = TelemetryTokenCount(response.usage.total_tokens);
    usage.input_token_count = TelemetryTokenCount(response.usage.prompt_tokens);
    // TTFT and memory remain unknown; token counts come from the current backend's per-turn accounting.
    try {
      telemetry_.RecordModelUsage(usage);
    } catch (...) {
      // Keep the modality-specific event independent of failure in the generic telemetry sink.
      LogUsageTelemetryFailure(logger_);
    }

    RecordAdditionalModelUsage(response, usage);
  } catch (...) {
    LogUsageTelemetryFailure(logger_);
  }
}

void Session::Cancel() {
  // Only update request lifecycle state — never block or join — so this is safe to call while the
  // SessionManager holds its own lock during shutdown. Generation loops poll the flag.
  std::lock_guard<std::mutex> lock(*active_requests_mutex_);
  session_canceled_ = true;
  for (const Request* r : active_requests_) {
    r->Cancel(Request::CancellationReason::SessionShutdown);
  }
}

}  // namespace fl
