// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "service/handler_utils.h"

#include <memory>
#include <string>

namespace fl {

struct ServiceContext;
struct ChatCompletionRequest;
class ChatSession;
class Model;
class GenAIModelInstance;
struct Request;

// ========================================================================
// Handler: POST /v1/chat/completions — OpenAI chat completions
// ========================================================================

class ChatCompletionsHandler : public HttpRequestHandler {
 public:
  explicit ChatCompletionsHandler(ServiceContext& ctx);

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override;

 private:
  /// Parse JSON body and deserialize into ChatCompletionRequest. Validates required fields.
  /// Returns an error response on failure, nullptr on success.
  std::shared_ptr<OutgoingResponse> ParseAndValidateRequest(const std::string& body,
                                                            ChatCompletionRequest& req,
                                                            Request& prepared_request);

  /// Look up model in catalog and verify it's loaded. Sets output pointers.
  /// Returns an error response on failure, nullptr on success.
  std::shared_ptr<OutgoingResponse> ResolveModel(const std::string& model_name,
                                                 Model*& model, GenAIModelInstance*& loaded);

  /// Build a Request with an OPENAI_JSON-tagged TEXT item from the original body string.
  /// Populates catalog defaults and model-specific options (tool_call_start/end).
  void BuildOpenAIJsonRequest(const std::string& body, const ChatCompletionRequest& req,
                              const Model& model, Request& session_request);

  /// Non-streaming inference — processes request, extracts the OPENAI_JSON-tagged TEXT response.
  std::shared_ptr<OutgoingResponse> HandleNonStreaming(ChatSession& session, Request& session_request);

  /// Streaming inference — runs inference in background thread with SSE output.
  std::shared_ptr<OutgoingResponse> HandleStreaming(ChatSession&& session, Request session_request,
                                                    bool include_usage);

  ServiceContext& ctx_;
};

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateChatCompletionsHandler(ServiceContext& ctx);

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
