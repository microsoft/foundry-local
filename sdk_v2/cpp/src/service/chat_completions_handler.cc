// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include "service/chat_completions_handler.h"

#include "c_api_types.h"
#include "catalog.h"
#include "contracts/chat_completions.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session.h"
#include "inferencing/session/session_manager.h"
#include "inferencing/session/session_registration.h"
#include "items/text_item.h"
#include "model_info.h"
#include "service/web_service.h"
#include "telemetry/telemetry_action_tracker.h"

#include <foundry_local/foundry_local_c.h>

#include <fmt/format.h>
#include <thread>

namespace fl {

// ========================================================================
// ChatCompletionsHandler — POST /v1/chat/completions
// ========================================================================

ChatCompletionsHandler::ChatCompletionsHandler(ServiceContext& ctx) : ctx_(ctx) {}

// --- Validation & model resolution ---

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ChatCompletionsHandler::ParseAndValidateRequest(
    const std::string& body, ChatCompletionRequest& req) {
  nlohmann::json req_json;

  try {
    req_json = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error& ex) {
    return ErrorResponse(Status::CODE_400, "Invalid JSON", ex.what());
  }

  try {
    req = req_json.get<ChatCompletionRequest>();
  } catch (const nlohmann::json::exception& ex) {
    return ErrorResponse(Status::CODE_400, "Invalid request", ex.what());
  }

  if (req.model.empty()) {
    return ErrorResponse(Status::CODE_400, "Missing required field: model");
  }

  if (req.messages.empty()) {
    return ErrorResponse(Status::CODE_400, "Missing required field: messages");
  }

  return nullptr;
}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ChatCompletionsHandler::ResolveModel(
    const std::string& model_name, Model*& model, GenAIModelInstance*& loaded) {
  model = ctx_.catalog.GetModelVariant(model_name);
  if (!model) {
    return ErrorResponse(Status::CODE_404, "Model not found", "No model matching '" + model_name + "'");
  }

  loaded = ctx_.model_load_manager.GetLoadedModel(model->Id());
  if (!loaded) {
    return ErrorResponse(Status::CODE_400, "Model not loaded",
                         "Model '" + model_name + "' must be loaded before inference");
  }

  return nullptr;
}

// --- Build an OPENAI_JSON-tagged TEXT request item for ChatSession ---

void ChatCompletionsHandler::BuildOpenAIJsonRequest(const std::string& body, const ChatCompletionRequest& req,
                                                    const Model& model, Request& session_request) {
  // Pass the original body directly — ChatSession will parse it once.
  // Avoids a serialize/deserialize round-trip through ChatCompletionRequest.
  session_request.AddOwnedItem(std::make_unique<TextItem>(body, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  // Pass model-specific tool call markers via options so ChatSession can find them.
  // These come from the model catalog, not the OpenAI request.
  if (req.tools.has_value() && !req.tools->empty()) {
    const auto& props = model.Info().string_properties;

    auto tc_start = props.find(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR);
    if (tc_start != props.end()) {
      session_request.options["tool_call_start"] = tc_start->second;
    }

    auto tc_end = props.find(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR);
    if (tc_end != props.end()) {
      session_request.options["tool_call_end"] = tc_end->second;
    }
  }
}

// --- Main handler ---

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ChatCompletionsHandler::handle(
    const std::shared_ptr<IncomingRequest>& request) {
  auto route_ctx = InvocationContext::Direct(GetUserAgent(request));
  auto tracker = std::make_unique<ActionTracker>(Action::kOpenAIChatCompletions, ctx_.telemetry, route_ctx);

  auto body_str = request->readBodyToString();
  if (!body_str || body_str->empty()) {
    tracker->SetStatus(ActionStatus::kClientError);
    return ErrorResponse(Status::CODE_400, "Empty request body");
  }

  // 1. Parse & validate
  // TODO: This provides minimal benefit since we have to parse the body again inside ChatSession.
  // We could push the validation down so the only meaningful thing this is doing is adding the model name to the
  // telemetry. How much do we care about that? Is it worth the double parsing?
  ChatCompletionRequest req;
  if (auto err = ParseAndValidateRequest(body_str->c_str(), req)) {
    tracker->SetStatus(ActionStatus::kClientError);
    return err;
  }

  bool stream = req.stream.value_or(false);

  ctx_.logger.Log(LogLevel::Debug,
                  fmt::format("HandleChatCompletionRequest: model={} stream={}", req.model, stream));

  // 2. Resolve model
  std::string model_name = req.model;
  Model* model = nullptr;
  GenAIModelInstance* loaded = nullptr;
  if (auto err = ResolveModel(model_name, model, loaded)) {
    tracker->SetStatus(ActionStatus::kClientError);
    return err;
  }

  tracker->SetModelId(model_name);

  // 3. Build an OPENAI_JSON-tagged TEXT request item.
  Request session_request;
  BuildOpenAIJsonRequest(body_str->c_str(), req, *model, session_request);

  // 5. Check stream_options for include_usage
  bool include_usage_in_stream = false;
  if (stream && req.stream_options.has_value()) {
    include_usage_in_stream = req.stream_options->include_usage;
  }

  // The session and the inference it drives happen as a consequence of this
  // route, so they are indirect and reuse the route's correlation id.
  auto session_ctx = route_ctx.AsIndirect();

  // 6. Run inference via ChatSession
  try {
    ChatSession session(*model, *loaded, ctx_.logger, ctx_.telemetry);
    {
      ActionTracker create_tracker(Action::kSessionCreate, ctx_.telemetry, session_ctx);
      create_tracker.SetModelId(model_name);
      create_tracker.SetStatus(ActionStatus::kSuccess);
    }
    session.SetRequestContext(session_ctx);

    if (stream) {
      // The route action is recorded by the streaming thread when the stream
      // finishes, so don't set its status here — move the tracker into the thread.
      return HandleStreaming(std::move(session), std::move(session_request), include_usage_in_stream,
                             std::move(tracker));
    } else {
      SessionRegistration reg(ctx_.session_manager, session);
      auto response = HandleNonStreaming(session, session_request);
      tracker->SetStatus(ActionStatus::kSuccess);
      return response;
    }
  } catch (const std::exception& ex) {
    tracker->RecordException(ex);
    ctx_.logger.Log(LogLevel::Error, fmt::format("Chat completion inference failed: {}", ex.what()));
    return ErrorResponse(Status::CODE_500, "Inference failed", ex.what());
  }
}

// --- Inference dispatch ---

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ChatCompletionsHandler::HandleNonStreaming(
    ChatSession& session, Request& session_request) {
  fl::Response session_response;
  session.ProcessRequest(session_request, session_response);

  // ChatSession produces a single OPENAI_JSON-tagged TextItem with the ChatCompletionResponse.
  if (session_response.items.empty() || session_response.items[0]->type != FOUNDRY_LOCAL_ITEM_TEXT) {
    return ErrorResponse(Status::CODE_500, "Internal error", "Expected text response from chat session");
  }

  auto& text_item = static_cast<TextItem&>(*session_response.items[0]);

  if (text_item.text_type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
    return ErrorResponse(Status::CODE_500, "Internal error", "Expected OPENAI_JSON response from chat session");
  }

  auto response_json = nlohmann::json::parse(text_item.text);

  return JsonResponse(Status::CODE_200, response_json);
}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ChatCompletionsHandler::HandleStreaming(
    ChatSession&& session, Request session_request, bool include_usage,
    std::unique_ptr<ActionTracker> route_tracker) {
  auto body = std::make_shared<SseStreamBody>();
  auto body_ptr = body;
  auto& logger = ctx_.logger;
  auto& thread_tracker = ctx_.thread_tracker;

  std::thread streaming_thread([bg_session = std::move(session), body_ptr, &logger,
                                req = std::move(session_request),
                                include_usage, &thread_tracker,
                                route_tracker = std::move(route_tracker),
                                &session_manager = ctx_.session_manager]() mutable {
    SessionRegistration reg(session_manager, bg_session);

    try {
      fl::Response bg_response;

      // Callback receives OPENAI_JSON-tagged TextItem chunks from ChatSession — just wrap in SSE framing.
      fl::Session::StreamingCallbackFn callback_fn = [&](flStreamingCallbackData event, void* /*user_data*/) -> int {
        fl::ItemQueue* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        auto item = queue->TryPop();
        if (!item) {
          return 0;
        }

        if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
          auto& text_item = static_cast<fl::TextItem&>(*item);
          body_ptr->Push("data: " + text_item.text + "\n\n");
        } else {
          logger.Log(LogLevel::Error,
                     fmt::format("Unexpected item type {} in chat streaming callback", static_cast<int>(item->type)));
        }

        return 0;
      };

      bg_session.SetStreamingCallback(callback_fn);
      bg_session.ProcessRequest(req, bg_response);

      // Usage chunk — only if stream_options.include_usage was true
      if (include_usage) {
        ChatCompletionChunk usage_chunk;

        // Read envelope metadata that ChatSession stored on the response
        const char* id = bg_response.metadata.Find("completion_id");
        const char* created_str = bg_response.metadata.Find("created");
        const char* model = bg_response.metadata.Find("model");
        usage_chunk.id = id ? id : "";
        usage_chunk.created = created_str ? std::stoll(created_str) : 0;
        usage_chunk.model = model ? model : "";

        ChatCompletionUsage usage;
        usage.prompt_tokens = static_cast<int>(bg_response.usage.prompt_tokens);
        usage.completion_tokens = static_cast<int>(bg_response.usage.completion_tokens);
        usage.total_tokens = static_cast<int>(bg_response.usage.total_tokens);
        usage_chunk.usage = std::move(usage);

        body_ptr->Push("data: " + nlohmann::json(usage_chunk).dump() + "\n\n");
      }

      body_ptr->Push("data: [DONE]\n\n");

      // Inference streamed to completion — record the route action as a success.
      if (route_tracker) {
        route_tracker->SetStatus(ActionStatus::kSuccess);
      }
    } catch (const std::exception& ex) {
      nlohmann::json err = {
          {"error", {{"message", ex.what()}, {"type", "server_error"}, {"param", nullptr}, {"code", nullptr}}},
      };
      body_ptr->Push("data: " + err.dump() + "\n\n");

      // Mid-stream failure: record the exception. The route action keeps its
      // default kFailure status.
      if (route_tracker) {
        route_tracker->RecordException(ex);
      }
    }

    body_ptr->Finish();
    // route_tracker is destroyed with this closure once the thread completes,
    // recording the route action with the full streaming duration and final status.
    thread_tracker.Remove(std::this_thread::get_id());
  });

  thread_tracker.Track(std::move(streaming_thread));

  auto response = oatpp::web::protocol::http::outgoing::Response::createShared(
      Status::CODE_200, body);
  response->putHeader("Content-Type", "text/event-stream");
  response->putHeader("Cache-Control", "no-cache");
  return response;
}

// ========================================================================
// Factory function
// ========================================================================

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateChatCompletionsHandler(ServiceContext& ctx) {
  return std::make_shared<ChatCompletionsHandler>(ctx);
}

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
