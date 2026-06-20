// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include "service/audio_transcriptions_handler.h"

#include "c_api_types.h"
#include "catalog.h"
#include "contracts/audio_transcriptions.h"
#include "inferencing/generative/audio/audio_session.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session_manager.h"
#include "inferencing/session/session_registration.h"
#include "items/text_item.h"
#include "service/web_service.h"
#include "telemetry/telemetry_action_tracker.h"

#include <filesystem>
#include <fmt/format.h>
#include <thread>

namespace fl {

// ========================================================================
// AudioTranscriptionsHandler — POST /v1/audio/transcriptions
// ========================================================================

AudioTranscriptionsHandler::AudioTranscriptionsHandler(ServiceContext& ctx) : ctx_(ctx) {}

// --- Extracted steps ---

std::shared_ptr<HttpRequestHandler::OutgoingResponse> AudioTranscriptionsHandler::ParseAndValidateRequest(
    const std::string& body, AudioTranscriptionRequest& req) {
  nlohmann::json req_json;

  try {
    req_json = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error& ex) {
    return ErrorResponse(Status::CODE_400, "Invalid JSON", ex.what());
  }

  try {
    req = req_json.get<AudioTranscriptionRequest>();
  } catch (const nlohmann::json::exception& ex) {
    return ErrorResponse(Status::CODE_400, "Invalid request", ex.what());
  }

  if (req.model.empty()) {
    return ErrorResponse(Status::CODE_400, "Missing required field: model");
  }

  if (req.filename.empty()) {
    return ErrorResponse(Status::CODE_400, "Missing required field: filename");
  }

  return nullptr;
}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> AudioTranscriptionsHandler::ResolveModel(
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

  if (model->Info().task != "automatic-speech-recognition") {
    return ErrorResponse(Status::CODE_400, "Model does not support audio transcription",
                         "Model '" + model_name + "' has task '" + model->Info().task + "'");
  }

  return nullptr;
}

// --- Build an OPENAI_JSON-tagged TEXT request item for AudioSession ---

void AudioTranscriptionsHandler::BuildOpenAIJsonRequest(const std::string& body, Request& session_request) {
  session_request.AddOwnedItem(std::make_unique<TextItem>(body, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
}

// --- Main handler ---

std::shared_ptr<HttpRequestHandler::OutgoingResponse> AudioTranscriptionsHandler::handle(
    const std::shared_ptr<IncomingRequest>& request) {
  auto route_ctx = InvocationContext::Direct(GetUserAgent(request));
  auto tracker = std::make_unique<ActionTracker>(Action::kOpenAIAudioTranscribe, ctx_.telemetry, route_ctx);

  auto body_str = request->readBodyToString();
  if (!body_str || body_str->empty()) {
    tracker->SetStatus(ActionStatus::kClientError);
    return ErrorResponse(Status::CODE_400, "Empty request body");
  }

  // 1. Parse & validate
  AudioTranscriptionRequest req;
  if (auto err = ParseAndValidateRequest(body_str->c_str(), req)) {
    tracker->SetStatus(ActionStatus::kClientError);
    return err;
  }

  bool stream = req.stream.value_or(false);

  ctx_.logger.Log(LogLevel::Debug,
                  fmt::format("HandleAudioTranscriptionRequest: model={} stream={}", req.model, stream));

  // 2. Validate file path
  try {
    if (!std::filesystem::exists(req.filename)) {
      tracker->SetStatus(ActionStatus::kClientError);
      return ErrorResponse(Status::CODE_400, "Audio file not found", "'" + req.filename + "'");
    }
  } catch (const std::filesystem::filesystem_error& ex) {
    tracker->SetStatus(ActionStatus::kClientError);
    return ErrorResponse(Status::CODE_400, "Invalid file path", ex.what());
  }

  // 3. Resolve model
  std::string model_name = req.model;
  Model* model = nullptr;
  GenAIModelInstance* loaded = nullptr;
  if (auto err = ResolveModel(model_name, model, loaded)) {
    tracker->SetStatus(ActionStatus::kClientError);
    return err;
  }

  tracker->SetModelId(model_name);

  // 4. Build an OPENAI_JSON-tagged TEXT request item — pass original body directly
  Request session_request;
  BuildOpenAIJsonRequest(body_str->c_str(), session_request);

  // The session and the inference it drives are indirect children of this route.
  auto session_ctx = route_ctx.AsIndirect();

  // 5. Dispatch to streaming or non-streaming
  try {
    AudioSession session(*model, *loaded, ctx_.logger, ctx_.telemetry);
    {
      ActionTracker create_tracker(Action::kSessionCreate, ctx_.telemetry, session_ctx);
      create_tracker.SetModelId(model_name);
      create_tracker.SetStatus(ActionStatus::kSuccess);
    }
    session.SetRequestContext(session_ctx);

    if (stream) {
      // The route action is recorded by the streaming thread on completion.
      return HandleStreaming(std::move(session), std::move(session_request), std::move(tracker));
    } else {
      SessionRegistration reg(ctx_.session_manager, session);
      auto response = HandleNonStreaming(session, session_request);
      tracker->SetStatus(ActionStatus::kSuccess);
      return response;
    }
  } catch (const std::exception& ex) {
    tracker->RecordException(ex);
    ctx_.logger.Log(LogLevel::Error, fmt::format("Audio transcription inference failed: {}", ex.what()));
    return ErrorResponse(Status::CODE_500, "Inference failed", ex.what());
  } catch (...) {
    ctx_.logger.Log(LogLevel::Error, "Audio transcription failed with unknown exception type");
    return ErrorResponse(Status::CODE_500, "Inference failed", "Unknown error");
  }
}

// --- Inference dispatch ---

std::shared_ptr<HttpRequestHandler::OutgoingResponse> AudioTranscriptionsHandler::HandleNonStreaming(
    AudioSession& session, Request& session_request) {
  ctx_.logger.Log(LogLevel::Debug, "HandleNonStreaming: creating audio session request");

  fl::Response session_response;
  session.ProcessRequest(session_request, session_response);

  // AudioSession produces a single OPENAI_JSON-tagged TextItem with the AudioTranscriptionResponse.
  if (session_response.items.empty() || session_response.items[0]->type != FOUNDRY_LOCAL_ITEM_TEXT) {
    return ErrorResponse(Status::CODE_500, "Internal error", "Expected text response from audio session");
  }

  auto& text_item = static_cast<TextItem&>(*session_response.items[0]);

  if (text_item.text_type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
    return ErrorResponse(Status::CODE_500, "Internal error", "Expected OPENAI_JSON response from audio session");
  }

  auto response_json = nlohmann::json::parse(text_item.text);

  return JsonResponse(Status::CODE_200, response_json);
}

std::shared_ptr<HttpRequestHandler::OutgoingResponse> AudioTranscriptionsHandler::HandleStreaming(
    AudioSession&& session, Request session_request, std::unique_ptr<ActionTracker> route_tracker) {
  auto body = std::make_shared<SseStreamBody>();
  auto body_ptr = body;
  auto& logger = ctx_.logger;
  auto& thread_tracker = ctx_.thread_tracker;

  std::thread streaming_thread([bg_session = std::move(session), body_ptr, &logger,
                                req = std::move(session_request), &thread_tracker,
                                route_tracker = std::move(route_tracker),
                                &session_manager = ctx_.session_manager]() mutable {
    SessionRegistration reg(session_manager, bg_session);

    try {
      fl::Response bg_response;

      // Callback receives OPENAI_JSON-tagged TextItem chunks from AudioSession — just wrap in SSE framing.
      Session::StreamingCallbackFn callback_fn = [&](flStreamingCallbackData event, void*) -> int {
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
                     fmt::format("Unexpected item type {} in audio streaming callback",
                                 static_cast<int>(item->type)));
        }

        return 0;
      };

      bg_session.SetStreamingCallback(callback_fn);
      bg_session.ProcessRequest(req, bg_response);

      // Send terminal event
      body_ptr->Push("data: [DONE]\n\n");

      if (route_tracker) {
        route_tracker->SetStatus(ActionStatus::kSuccess);
      }
    } catch (const std::exception& ex) {
      logger.Log(LogLevel::Error, fmt::format("Audio streaming transcription failed: {}", ex.what()));

      // Push error to stream so client doesn't hang
      nlohmann::json error = {{"error", {{"message", ex.what()}}}};
      body_ptr->Push("data: " + error.dump() + "\n\n");

      if (route_tracker) {
        route_tracker->RecordException(ex);
      }
    }

    body_ptr->Finish();
    thread_tracker.Remove(std::this_thread::get_id());
  });

  thread_tracker.Track(std::move(streaming_thread));

  auto response = oatpp::web::protocol::http::outgoing::Response::createShared(Status::CODE_200, body);
  response->putHeader("Content-Type", "text/event-stream");
  response->putHeader("Cache-Control", "no-cache");
  return response;
}

// ========================================================================
// Factory function
// ========================================================================

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateAudioTranscriptionsHandler(ServiceContext& ctx) {
  return std::make_shared<AudioTranscriptionsHandler>(ctx);
}

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
