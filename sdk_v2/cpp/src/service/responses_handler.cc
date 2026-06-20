// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include "service/responses_handler.h"

#include "c_api_types.h"
#include "catalog.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/openresponses/response_converter.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session.h"
#include "inferencing/session/session_manager.h"
#include "inferencing/session/session_registration.h"
#include "items/text_item.h"
#include "service/web_service.h"
#include "telemetry/telemetry_action_tracker.h"

#include <chrono>
#include <fmt/format.h>
#include <thread>

namespace fl {

using namespace fl::responses;

// ========================================================================
// ResponsesHandler — POST /v1/responses
// ========================================================================

ResponsesHandler::ResponsesHandler(ServiceContext& ctx) : ctx_(ctx) {}

// --- Extracted steps ---

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ResponsesHandler::ParseAndValidateRequest(
    const std::string& body, nlohmann::json& req_json, ResponseCreateParams& params) {
  try {
    req_json = nlohmann::json::parse(body.c_str());
  } catch (const nlohmann::json::parse_error& ex) {
    return ErrorResponse(Status::CODE_400, "Invalid JSON", ex.what());
  }

  if (!req_json.contains("model") || !req_json["model"].is_string()) {
    return ErrorResponse(Status::CODE_400, "Missing required field: model");
  }

  if (!req_json.contains("input")) {
    return ErrorResponse(Status::CODE_400, "Missing required field: input");
  }

  // Validate input type
  const auto& input = req_json["input"];
  if (!input.is_string() && !input.is_array()) {
    return ErrorResponse(Status::CODE_400, "Invalid input", "'input' must be a string or array");
  }

  if (input.is_array()) {
    for (const auto& entry : input) {
      if (!entry.is_object()) {
        return ErrorResponse(Status::CODE_400, "Invalid input item", "Each input item must be an object");
      }

      std::string type = entry.value("type", "");
      std::string role = entry.value("role", "");
      if (type != "function_call_output" && type != "function_call" && role.empty()) {
        return ErrorResponse(Status::CODE_400, "Invalid input item", "Message items must have a 'role' field");
      }
    }
  }

  try {
    params = req_json.get<ResponseCreateParams>();
  } catch (const nlohmann::json::exception& ex) {
    return ErrorResponse(Status::CODE_400, "Invalid request parameters", ex.what());
  }

  return nullptr;
}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ResponsesHandler::ResolveModel(
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

void ResponsesHandler::LoadPreviousContext(const ResponseCreateParams& params,
                                           const nlohmann::json*& previous_input,
                                           const nlohmann::json*& previous_output,
                                           nlohmann::json& prev_input_storage,
                                           nlohmann::json& prev_output_storage) {
  previous_input = nullptr;
  previous_output = nullptr;

  if (!params.previous_response_id.has_value()) {
    return;
  }

  const std::string& prev_id = *params.previous_response_id;
  auto prev_response = ctx_.response_store.Get(prev_id);

  if (!prev_response) {
    ctx_.logger.Log(LogLevel::Warning, fmt::format("Previous response {} not found", prev_id));
    return;
  }

  auto prev_items = ctx_.response_store.GetInputItems(prev_id);
  if (prev_items) {
    prev_input_storage = *prev_items;
    previous_input = &prev_input_storage;
  }

  if (prev_response->contains("output")) {
    prev_output_storage = (*prev_response)["output"];
    previous_output = &prev_output_storage;
  }
}

// --- Main handler ---

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ResponsesHandler::handle(
    const std::shared_ptr<IncomingRequest>& request) {
  auto route_ctx = InvocationContext::Direct(GetUserAgent(request));
  auto tracker = std::make_unique<ActionTracker>(Action::kOpenAIResponsesCreate, ctx_.telemetry, route_ctx);

  auto body_str = request->readBodyToString();
  if (!body_str || body_str->empty()) {
    tracker->SetStatus(ActionStatus::kClientError);
    return ErrorResponse(Status::CODE_400, "Empty request body");
  }

  // 1. Parse & validate
  nlohmann::json req_json;
  ResponseCreateParams params;
  if (auto err = ParseAndValidateRequest(body_str->c_str(), req_json, params)) {
    tracker->SetStatus(ActionStatus::kClientError);
    return err;
  }

  ctx_.logger.Log(LogLevel::Information,
                  fmt::format("CreateResponse: model={} stream={} max_output_tokens={}",
                              params.model, params.stream,
                              params.max_output_tokens.has_value() ? std::to_string(*params.max_output_tokens)
                                                                   : "default"));

  // 2. Resolve model
  std::string model_name = params.model;
  Model* model = nullptr;
  GenAIModelInstance* loaded = nullptr;
  if (auto err = ResolveModel(model_name, model, loaded)) {
    tracker->SetStatus(ActionStatus::kClientError);
    return err;
  }

  tracker->SetModelId(model_name);

  // 3. Load previous context if chaining via previous_response_id
  const nlohmann::json* previous_input = nullptr;
  const nlohmann::json* previous_output = nullptr;
  nlohmann::json prev_input_storage;
  nlohmann::json prev_output_storage;
  LoadPreviousContext(params, previous_input, previous_output, prev_input_storage, prev_output_storage);

  // 4. Obtain or reuse cached session
  auto now = std::chrono::system_clock::now();
  int64_t created_at = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  std::string response_id = ResponseConverter::GenerateId("resp");

  std::unique_ptr<ChatSession> session;

  if (params.previous_response_id) {
    session = ctx_.session_manager.CheckOut(*params.previous_response_id);

    if (session) {
      ctx_.logger.Log(LogLevel::Information,
                      fmt::format("CreateResponse: reusing cached session from '{}'", *params.previous_response_id));
    }
  }

  // 5. Build session request — skip previous context if session was reused (it's already in the KV cache)
  const nlohmann::json* effective_prev_input = session ? nullptr : previous_input;
  const nlohmann::json* effective_prev_output = session ? nullptr : previous_output;
  Request session_request = ResponseConverter::ToSessionRequest(params, effective_prev_input, effective_prev_output);

  // Extract tools + tool_choice. Done outside ToSessionRequest to mirror the chat-completions
  // path (BuildRequestItems / ExtractToolDefinitions split) and so attachment to the session
  // happens here in the handler that owns the session lifetime.
  std::string tools_json = ResponseConverter::ExtractResponsesToolDefinitions(params, session_request);

  // The session and the inference it drives happen as a consequence of this
  // route, so they are indirect and reuse the route's correlation id.
  auto session_ctx = route_ctx.AsIndirect();

  try {
    if (!session) {
      ActionTracker create_tracker(Action::kSessionCreate, ctx_.telemetry, session_ctx);
      create_tracker.SetModelId(model_name);
      session = std::make_unique<ChatSession>(*model, *loaded, ctx_.logger, ctx_.telemetry);
      create_tracker.SetStatus(ActionStatus::kSuccess);
    }
    session->SetRequestContext(session_ctx);

    // Sessions can be reused via previous_response_id; clear any stale tool defs from the prior
    // turn before applying this request's tools so the request stays self-contained.
    session->ClearToolDefinitions();
    if (!tools_json.empty()) {
      session->AddToolDefinition({{}, {}, std::move(tools_json)});
    }

    if (params.stream) {
      ctx_.logger.Log(LogLevel::Debug,
                      fmt::format("Creating streaming response {} for model {}", response_id, model_name));

      // The route action is recorded by the streaming thread when the stream
      // finishes — move the tracker in rather than marking success now.
      return HandleStreaming(std::move(session), std::move(session_request), model_name,
                             response_id, created_at, params, req_json, std::move(tracker));
    } else {
      ctx_.logger.Log(LogLevel::Debug,
                      fmt::format("Creating response {} for model {}", response_id, model_name));

      auto response = HandleNonStreaming(std::move(session), session_request, model_name,
                                         response_id, created_at, params, req_json);
      tracker->SetStatus(ActionStatus::kSuccess);

      return response;
    }
  } catch (const std::exception& ex) {
    tracker->RecordException(ex);

    ctx_.logger.Log(LogLevel::Error, fmt::format("Response {} failed: {}", response_id, ex.what()));

    auto failed = ResponseConverter::BuildFailedResponseObject(response_id, created_at, model_name, params,
                                                               "server_error", ex.what());
    nlohmann::json failed_json = failed;
    return JsonResponse(Status::CODE_500, failed_json);
  }
}

// --- Inference dispatch ---

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ResponsesHandler::HandleNonStreaming(
    std::unique_ptr<ChatSession> session, Request& session_request,
    const std::string& model_name, const std::string& response_id,
    int64_t created_at, const ResponseCreateParams& params,
    const nlohmann::json& req_json) {
  SessionRegistration reg(ctx_.session_manager, *session);

  fl::Response session_response;
  session->ProcessRequest(session_request, session_response);

  auto [output, output_text] = ResponseConverter::FromSessionResponse(session_response);

  auto response = ResponseConverter::BuildResponseObject(response_id, created_at, model_name, params,
                                                         std::move(output), output_text, session_response.usage);

  nlohmann::json response_json = response;

  // Store if requested
  if (params.store) {
    auto input_items = ResponseConverter::ToInputItems(req_json);
    ctx_.response_store.Store(response_id, response_json, std::move(input_items));

    // Deregister before caching — session is idle, not actively working.
    // Must happen before CheckIn to prevent a race where another thread
    // checks out the session while it's still registered by this guard.
    reg.Release();

    // Clear per-request streaming callback before caching. The callback
    // captures stack-local state (SSE body, accumulators) that won't survive
    // past this scope. Without this, a reused session would fire a stale
    // callback into a dead stack frame.
    session->SetStreamingCallback(nullptr);

    // Cache the session for potential reuse on the next turn
    ctx_.session_manager.CheckIn(response_id, std::move(session));
  }

  return JsonResponse(Status::CODE_200, response_json);
}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ResponsesHandler::HandleStreaming(
    std::unique_ptr<ChatSession> session, Request session_request,
    const std::string& model_name, const std::string& response_id,
    int64_t created_at, const ResponseCreateParams& params,
    const nlohmann::json& req_json, std::unique_ptr<ActionTracker> route_tracker) {
  auto body = std::make_shared<SseStreamBody>();

  auto initial_response = ResponseConverter::BuildInitialResponseObject(response_id, created_at, model_name, params);

  // IDs for the assistant message and any reasoning items are minted lazily by the streaming thread, one per item,
  // because the model may emit interleaved reasoning/visible runs and each contiguous run becomes its own
  // output item (matching the OpenAI Responses API shape).

  // Emit: response.created
  StreamEvent created_event;
  created_event.type = StreamEventType::kResponseCreated;
  created_event.sequence_number = 0;
  created_event.response = initial_response;
  body->Push("event: response.created\ndata: " + nlohmann::json(created_event).dump() + "\n\n");

  // Emit: response.in_progress
  StreamEvent in_progress_event;
  in_progress_event.type = StreamEventType::kResponseInProgress;
  in_progress_event.sequence_number = 1;
  in_progress_event.response = initial_response;
  body->Push("event: response.in_progress\ndata: " + nlohmann::json(in_progress_event).dump() + "\n\n");

  // The output_item.added / content_part.added events are deferred to the streaming thread so we can decide
  // per-segment whether to open a `reasoning` or `message` item, and so each transition between reasoning and
  // visible text can start a fresh item with its own id and output_index.

  // Capture for background thread
  auto body_ptr = body;
  bool should_store = params.store;
  auto& store = ctx_.response_store;
  nlohmann::json req_copy = req_json;
  ResponseCreateParams params_copy = params;
  auto& logger = ctx_.logger;
  auto& session_manager = ctx_.session_manager;
  auto& tracker = ctx_.thread_tracker;

  // Background thread is required: oatpp needs the Response returned immediately so it can
  // start writing SSE events. ProcessRequest blocks until generation completes.
  std::thread streaming_thread([body_ptr, &logger, &session_manager,
                                session = std::move(session),
                                req = std::move(session_request),
                                model_name, response_id, created_at,
                                should_store, &store,
                                req_copy = std::move(req_copy),
                                params_copy = std::move(params_copy),
                                route_tracker = std::move(route_tracker),
                                &tracker]() mutable {
    SessionRegistration reg(session_manager, *session);

    int seq = 2;
    std::string full_text;  // concatenation of all visible runs, used for output_text in completed_response

    // Per-item state for the currently-open item. `current_kind == nullopt` means no item is open. On every type
    // transition we close the open item (emitting its done events) and open a new one with a fresh id at the next
    // output_index. Adjacent same-typed segments accumulate into the same item naturally because we don't close
    // until the type changes.
    enum class ItemKind { Reasoning,
                          Message };
    std::optional<ItemKind> current_kind;
    std::string current_id;
    std::string current_text;
    int current_output_index = -1;
    int next_output_index = 0;

    // Items that have been *closed* (or, for the currently-open item at end-of-stream, finalized in place).
    // Used to construct the final `output[]` array for the response.completed event.
    std::vector<ResponseOutputItem> closed_items;

    auto push_event = [&](const std::string& event_name, const StreamEvent& ev) {
      body_ptr->Push("event: " + event_name + "\ndata: " + nlohmann::json(ev).dump() + "\n\n");
    };

    auto close_current = [&]() {
      if (!current_kind.has_value()) {
        return;
      }

      if (*current_kind == ItemKind::Reasoning) {
        // Emit: response.reasoning.done
        StreamEvent done_ev;
        done_ev.type = StreamEventType::kReasoningDone;
        done_ev.sequence_number = seq++;
        done_ev.output_index = current_output_index;
        done_ev.item_id = current_id;
        done_ev.text = current_text;
        push_event("response.reasoning.done", done_ev);

        // Emit: response.output_item.done
        ReasoningOutputItem rs;
        rs.id = current_id;
        rs.status = ResponseStatus::kCompleted;
        rs.summary.push_back(ReasoningSummaryText{current_text});

        StreamEvent item_done;
        item_done.type = StreamEventType::kOutputItemDone;
        item_done.sequence_number = seq++;
        item_done.output_index = current_output_index;
        item_done.item = rs;
        push_event("response.output_item.done", item_done);

        closed_items.push_back(std::move(rs));
      } else {
        // Message: emit text.done, content_part.done, output_item.done.
        StreamEvent text_done;
        text_done.type = StreamEventType::kTextDone;
        text_done.sequence_number = seq++;
        text_done.output_index = current_output_index;
        text_done.content_index = 0;
        text_done.item_id = current_id;
        text_done.text = current_text;
        push_event("response.output_text.done", text_done);

        OutputTextContent done_text_part{current_text};

        StreamEvent part_done;
        part_done.type = StreamEventType::kContentPartDone;
        part_done.sequence_number = seq++;
        part_done.output_index = current_output_index;
        part_done.content_index = 0;
        part_done.item_id = current_id;
        part_done.content_part = done_text_part;
        push_event("response.content_part.done", part_done);

        ResponseOutputMessage done_msg;
        done_msg.id = current_id;
        done_msg.role = "assistant";
        done_msg.status = ResponseStatus::kCompleted;
        done_msg.content.push_back(OutputTextContent{current_text});

        StreamEvent item_done;
        item_done.type = StreamEventType::kOutputItemDone;
        item_done.sequence_number = seq++;
        item_done.output_index = current_output_index;
        item_done.item = done_msg;
        push_event("response.output_item.done", item_done);

        closed_items.push_back(std::move(done_msg));
      }

      current_kind.reset();
      current_id.clear();
      current_text.clear();
      current_output_index = -1;
    };

    auto open_reasoning = [&]() {
      current_kind = ItemKind::Reasoning;
      current_id = ResponseConverter::GenerateId("rs");
      current_output_index = next_output_index++;
      current_text.clear();

      ReasoningOutputItem rs;
      rs.id = current_id;
      rs.status = ResponseStatus::kInProgress;

      StreamEvent ev;
      ev.type = StreamEventType::kOutputItemAdded;
      ev.sequence_number = seq++;
      ev.output_index = current_output_index;
      ev.item = rs;
      push_event("response.output_item.added", ev);
    };

    auto open_message = [&]() {
      current_kind = ItemKind::Message;
      current_id = ResponseConverter::GenerateId("msg");
      current_output_index = next_output_index++;
      current_text.clear();

      ResponseOutputMessage output_msg;
      output_msg.id = current_id;
      output_msg.role = "assistant";
      output_msg.status = ResponseStatus::kInProgress;

      StreamEvent item_added;
      item_added.type = StreamEventType::kOutputItemAdded;
      item_added.sequence_number = seq++;
      item_added.output_index = current_output_index;
      item_added.item = output_msg;
      push_event("response.output_item.added", item_added);

      OutputTextContent empty_text_part{""};

      StreamEvent part_added;
      part_added.type = StreamEventType::kContentPartAdded;
      part_added.sequence_number = seq++;
      part_added.output_index = current_output_index;
      part_added.content_index = 0;
      part_added.item_id = current_id;
      part_added.content_part = empty_text_part;
      push_event("response.content_part.added", part_added);
    };

    try {
      fl::Response bg_response;
      fl::Session::StreamingCallbackFn callback_fn = [&](flStreamingCallbackData event, void* /*user_data*/) -> int {
        fl::ItemQueue* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        auto item = queue->TryPop();

        if (!item) {
          // should never happen
          return 0;
        }

        assert(item->type == FOUNDRY_LOCAL_ITEM_TEXT);
        auto* text_item = static_cast<fl::TextItem*>(item.get());

        ItemKind incoming = (text_item->text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING)
                                ? ItemKind::Reasoning
                                : ItemKind::Message;

        // Type transition (or first segment): close the open item and open a fresh one of the new kind.
        if (!current_kind.has_value() || *current_kind != incoming) {
          close_current();

          if (incoming == ItemKind::Reasoning) {
            open_reasoning();
          } else {
            open_message();
          }
        }

        current_text += text_item->text;

        if (incoming == ItemKind::Reasoning) {
          StreamEvent delta;
          delta.type = StreamEventType::kReasoningDelta;
          delta.sequence_number = seq++;
          delta.output_index = current_output_index;
          delta.item_id = current_id;
          delta.delta = text_item->text;
          push_event("response.reasoning.delta", delta);
        } else {
          full_text += text_item->text;

          StreamEvent text_delta;
          text_delta.type = StreamEventType::kTextDelta;
          text_delta.sequence_number = seq++;
          text_delta.output_index = current_output_index;
          text_delta.content_index = 0;
          text_delta.item_id = current_id;
          text_delta.delta = text_item->text;
          push_event("response.output_text.delta", text_delta);
        }

        return 0;
      };

      session->SetStreamingCallback(callback_fn);

      session->ProcessRequest(req, bg_response);

      // Close whatever item is still open at end-of-generation so the SSE stream is well-formed.
      close_current();

      auto completed_response = ResponseConverter::BuildResponseObject(
          response_id, created_at, model_name, params_copy, std::move(closed_items), full_text, bg_response.usage);

      StreamEvent completed;
      completed.type = StreamEventType::kResponseCompleted;
      completed.sequence_number = seq++;
      completed.response = completed_response;
      push_event("response.completed", completed);

      // Store if requested
      if (should_store) {
        auto input_items = ResponseConverter::ToInputItems(req_copy);
        nlohmann::json response_json = completed_response;
        store.Store(response_id, response_json, std::move(input_items));

        // Deregister before caching — see non-streaming path comment.
        reg.Release();

        // Clear per-request streaming callback before caching — see non-streaming path comment.
        session->SetStreamingCallback(nullptr);

        // Cache the session for potential reuse on the next turn
        session_manager.CheckIn(response_id, std::move(session));
      }

      // Streamed to completion — record the route action as a success.
      if (route_tracker) {
        route_tracker->SetStatus(ActionStatus::kSuccess);
      }

    } catch (const std::exception& ex) {
      logger.Log(LogLevel::Error, fmt::format("Response {} failed during streaming: {}", response_id, ex.what()));

      auto error_response = ResponseConverter::BuildFailedResponseObject(
          response_id, created_at, model_name, params_copy,
          "server_error", ex.what());

      StreamEvent failed;
      failed.type = StreamEventType::kResponseFailed;
      failed.sequence_number = seq++;
      failed.response = error_response;
      body_ptr->Push("event: response.failed\ndata: " + nlohmann::json(failed).dump() + "\n\n");

      // Mid-stream failure: record the exception; the route action keeps kFailure.
      if (route_tracker) {
        route_tracker->RecordException(ex);
      }
    }

    // Terminal event per spec
    body_ptr->Push("data: [DONE]\n\n");
    body_ptr->Finish();

    tracker.Remove(std::this_thread::get_id());
  });

  tracker.Track(std::move(streaming_thread));

  auto response = oatpp::web::protocol::http::outgoing::Response::createShared(
      Status::CODE_200, body);
  response->putHeader("Content-Type", "text/event-stream");
  response->putHeader("Cache-Control", "no-cache");
  return response;
}

// ========================================================================
// GetResponseHandler — GET /v1/responses/{id}
// ========================================================================

GetResponseHandler::GetResponseHandler(ServiceContext& ctx) : ctx_(ctx) {}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> GetResponseHandler::handle(
    const std::shared_ptr<IncomingRequest>& request) {
  ActionTracker tracker(Action::kOpenAIResponsesGet, ctx_.telemetry,
                        InvocationContext::Direct(GetUserAgent(request)));

  auto id = request->getPathVariable("id");
  if (!id) {
    tracker.SetStatus(ActionStatus::kClientError);
    return ErrorResponse(Status::CODE_400, "Missing response ID");
  }

  ctx_.logger.Log(LogLevel::Debug, fmt::format("GetResponse: responseId={}", std::string(id->c_str())));

  auto response = ctx_.response_store.Get(id->c_str());
  if (!response) {
    tracker.SetStatus(ActionStatus::kClientError);
    nlohmann::json error_body = {
        {"error", {
                      {"message", "The response '" + std::string(id->c_str()) + "' does not exist."},
                      {"type", "invalid_request_error"},
                      {"param", "id"},
                      {"code", "response_not_found"},
                  }},
    };
    return JsonResponse(Status::CODE_404, error_body);
  }

  tracker.SetStatus(ActionStatus::kSuccess);

  return JsonResponse(Status::CODE_200, *response);
}

// ========================================================================
// ListResponsesHandler — GET /v1/responses
// ========================================================================

ListResponsesHandler::ListResponsesHandler(ServiceContext& ctx) : ctx_(ctx) {}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> ListResponsesHandler::handle(
    const std::shared_ptr<IncomingRequest>& request) {
  ActionTracker tracker(Action::kOpenAIResponsesList, ctx_.telemetry,
                        InvocationContext::Direct(GetUserAgent(request)));

  // Parse query parameters
  auto limit_str = request->getQueryParameter("limit", "20");
  auto order = request->getQueryParameter("order", "desc");
  auto after = request->getQueryParameter("after", "");

  int limit = 20;
  try {
    limit = std::stoi(limit_str->c_str());
    if (limit <= 0) {
      limit = 20;
    }

    if (limit > 100) {
      limit = 100;
    }
  } catch (...) {
    // keep default
  }

  ctx_.logger.Log(LogLevel::Debug, fmt::format("ListResponses: limit={}", limit));

  auto data = ctx_.response_store.List(limit, after->c_str(), order->c_str());

  nlohmann::json result = {
      {"object", "list"},
      {"data", nlohmann::json::array()},
      {"first_id", nullptr},
      {"last_id", nullptr},
      {"has_more", static_cast<int>(data.size()) == limit},
  };

  for (auto& item : data) {
    result["data"].push_back(std::move(item));
  }

  if (!result["data"].empty()) {
    result["first_id"] = result["data"].front().value("id", "");
    result["last_id"] = result["data"].back().value("id", "");
  }

  tracker.SetStatus(ActionStatus::kSuccess);

  return JsonResponse(Status::CODE_200, result);
}

// ========================================================================
// DeleteResponseHandler — DELETE /v1/responses/{id}
// ========================================================================

DeleteResponseHandler::DeleteResponseHandler(ServiceContext& ctx) : ctx_(ctx) {}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> DeleteResponseHandler::handle(
    const std::shared_ptr<IncomingRequest>& request) {
  ActionTracker tracker(Action::kOpenAIResponsesDelete, ctx_.telemetry,
                        InvocationContext::Direct(GetUserAgent(request)));

  auto id = request->getPathVariable("id");
  if (!id) {
    tracker.SetStatus(ActionStatus::kClientError);
    return ErrorResponse(Status::CODE_400, "Missing response ID");
  }

  ctx_.logger.Log(LogLevel::Debug, fmt::format("DeleteResponse: responseId={}", std::string(id->c_str())));

  bool deleted = ctx_.response_store.Delete(id->c_str());
  if (!deleted) {
    tracker.SetStatus(ActionStatus::kClientError);
    nlohmann::json error_body = {
        {"error", {
                      {"message", "The response '" + std::string(id->c_str()) + "' does not exist."},
                      {"type", "invalid_request_error"},
                      {"param", "id"},
                      {"code", "response_not_found"},
                  }},
    };
    return JsonResponse(Status::CODE_404, error_body);
  }

  // Drop any cached ChatSession keyed on this response id so it stops pinning the model.
  // A miss here is fine — chained calls with store=false never cache.
  ctx_.session_manager.EvictCached(id->c_str());

  nlohmann::json result = {
      {"id", std::string(id->c_str())},
      {"object", "response.deleted"},
      {"deleted", true},
  };

  tracker.SetStatus(ActionStatus::kSuccess);

  return JsonResponse(Status::CODE_200, result);
}

// ========================================================================
// GetInputItemsHandler — GET /v1/responses/{id}/input_items
// ========================================================================

GetInputItemsHandler::GetInputItemsHandler(ServiceContext& ctx) : ctx_(ctx) {}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> GetInputItemsHandler::handle(
    const std::shared_ptr<IncomingRequest>& request) {
  ActionTracker tracker(Action::kOpenAIResponsesGetInputItems, ctx_.telemetry,
                        InvocationContext::Direct(GetUserAgent(request)));

  auto id = request->getPathVariable("id");
  if (!id) {
    tracker.SetStatus(ActionStatus::kClientError);
    return ErrorResponse(Status::CODE_400, "Missing response ID");
  }

  ctx_.logger.Log(LogLevel::Debug, fmt::format("GetInputItems: responseId={}", std::string(id->c_str())));

  // Check that response exists
  auto response = ctx_.response_store.Get(id->c_str());
  if (!response) {
    tracker.SetStatus(ActionStatus::kClientError);
    nlohmann::json error_body = {
        {"error", {
                      {"message", "The response '" + std::string(id->c_str()) + "' does not exist."},
                      {"type", "invalid_request_error"},
                      {"param", "id"},
                      {"code", "response_not_found"},
                  }},
    };
    return JsonResponse(Status::CODE_404, error_body);
  }

  auto input_items = ctx_.response_store.GetInputItems(id->c_str());
  auto data = input_items.value_or(nlohmann::json::array());

  nlohmann::json result = {
      {"object", "list"},
      {"data", data},
      {"first_id", nullptr},
      {"last_id", nullptr},
      {"has_more", false},
  };

  if (!data.empty()) {
    if (data.front().contains("id")) {
      result["first_id"] = data.front()["id"];
    }

    if (data.back().contains("id")) {
      result["last_id"] = data.back()["id"];
    }
  }

  tracker.SetStatus(ActionStatus::kSuccess);

  return JsonResponse(Status::CODE_200, result);
}

// ========================================================================
// Factory functions
// ========================================================================

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateResponsesHandler(ServiceContext& ctx) {
  return std::make_shared<ResponsesHandler>(ctx);
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateGetResponseHandler(ServiceContext& ctx) {
  return std::make_shared<GetResponseHandler>(ctx);
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateListResponsesHandler(ServiceContext& ctx) {
  return std::make_shared<ListResponsesHandler>(ctx);
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateDeleteResponseHandler(ServiceContext& ctx) {
  return std::make_shared<DeleteResponseHandler>(ctx);
}

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateGetInputItemsHandler(ServiceContext& ctx) {
  return std::make_shared<GetInputItemsHandler>(ctx);
}

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
