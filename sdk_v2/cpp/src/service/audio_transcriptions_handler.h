// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "service/handler_utils.h"

#include <memory>
#include <string>

namespace fl {

struct ServiceContext;
struct AudioTranscriptionRequest;
class AudioSession;
class Model;
class GenAIModelInstance;
class ActionTracker;
struct Request;

// ========================================================================
// Handler: POST /v1/audio/transcriptions — OpenAI audio transcriptions
// ========================================================================

class AudioTranscriptionsHandler : public HttpRequestHandler {
 public:
  explicit AudioTranscriptionsHandler(ServiceContext& ctx);

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override;

 private:
  /// Parse JSON body and deserialize into AudioTranscriptionRequest. Validates required fields.
  std::shared_ptr<OutgoingResponse> ParseAndValidateRequest(const std::string& body,
                                                            AudioTranscriptionRequest& req);

  /// Look up model in catalog and verify it's loaded and supports audio.
  std::shared_ptr<OutgoingResponse> ResolveModel(const std::string& model_name,
                                                 Model*& model, GenAIModelInstance*& loaded);

  /// Build a Request with an OPENAI_JSON-tagged TEXT item from the original body string.
  void BuildOpenAIJsonRequest(const std::string& body, Request& session_request);

  /// Non-streaming inference — processes request, extracts the OPENAI_JSON-tagged TEXT response.
  std::shared_ptr<OutgoingResponse> HandleNonStreaming(AudioSession& session, Request& session_request);

  /// Streaming inference — runs inference in a background thread with SSE output.
  /// Takes ownership of the route ActionTracker so the route action records when
  /// the stream finishes (real duration + terminal status).
  std::shared_ptr<OutgoingResponse> HandleStreaming(AudioSession&& session, Request session_request,
                                                    std::unique_ptr<ActionTracker> route_tracker);

  ServiceContext& ctx_;
};

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateAudioTranscriptionsHandler(ServiceContext& ctx);

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
