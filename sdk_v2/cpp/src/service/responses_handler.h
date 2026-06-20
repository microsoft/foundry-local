// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "service/handler_utils.h"

#include <memory>
#include <string>

namespace fl {

struct ServiceContext;
struct Request;
class ChatSession;
class Model;
class GenAIModelInstance;
class ActionTracker;

namespace responses {
struct ResponseCreateParams;
}  // namespace responses

// ========================================================================
// Handler: POST /v1/responses — OpenAI Responses API
// ========================================================================

class ResponsesHandler : public HttpRequestHandler {
 public:
  explicit ResponsesHandler(ServiceContext& ctx);

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override;

 private:
  // --- Extracted steps from handle() ---

  /// Parse JSON body, validate required fields, and deserialize into ResponseCreateParams.
  /// Returns an error response on failure, nullptr on success.
  std::shared_ptr<OutgoingResponse> ParseAndValidateRequest(const std::string& body,
                                                            nlohmann::json& req_json,
                                                            responses::ResponseCreateParams& params);

  /// Look up model in catalog and verify it's loaded. Sets output pointers.
  /// Returns an error response on failure, nullptr on success.
  std::shared_ptr<OutgoingResponse> ResolveModel(const std::string& model_name,
                                                 Model*& model, GenAIModelInstance*& loaded);

  /// Load previous response context when chaining via previous_response_id.
  /// The json storage objects are passed by reference because the output pointers alias into them.
  void LoadPreviousContext(const responses::ResponseCreateParams& params,
                           const nlohmann::json*& previous_input,
                           const nlohmann::json*& previous_output,
                           nlohmann::json& prev_input_storage,
                           nlohmann::json& prev_output_storage);

  // --- Inference dispatch ---

  std::shared_ptr<OutgoingResponse> HandleNonStreaming(std::unique_ptr<ChatSession> session, Request& session_request,
                                                       const std::string& model_name, const std::string& response_id,
                                                       int64_t created_at,
                                                       const responses::ResponseCreateParams& params,
                                                       const nlohmann::json& req_json);

  std::shared_ptr<OutgoingResponse> HandleStreaming(std::unique_ptr<ChatSession> session, Request session_request,
                                                    const std::string& model_name, const std::string& response_id,
                                                    int64_t created_at,
                                                    const responses::ResponseCreateParams& params,
                                                    const nlohmann::json& req_json,
                                                    std::unique_ptr<ActionTracker> route_tracker);

  ServiceContext& ctx_;
};

// ========================================================================
// Handler: GET /v1/responses/{id} — Retrieve a stored response
// ========================================================================

class GetResponseHandler : public HttpRequestHandler {
 public:
  explicit GetResponseHandler(ServiceContext& ctx);

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override;

 private:
  ServiceContext& ctx_;
};

// ========================================================================
// Handler: GET /v1/responses — List stored responses
// ========================================================================

class ListResponsesHandler : public HttpRequestHandler {
 public:
  explicit ListResponsesHandler(ServiceContext& ctx);

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override;

 private:
  ServiceContext& ctx_;
};

// ========================================================================
// Handler: DELETE /v1/responses/{id} — Delete a stored response
// ========================================================================

class DeleteResponseHandler : public HttpRequestHandler {
 public:
  explicit DeleteResponseHandler(ServiceContext& ctx);

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override;

 private:
  ServiceContext& ctx_;
};

// ========================================================================
// Handler: GET /v1/responses/{id}/input_items — Get input items for a response
// ========================================================================

class GetInputItemsHandler : public HttpRequestHandler {
 public:
  explicit GetInputItemsHandler(ServiceContext& ctx);

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest>& request) override;

 private:
  ServiceContext& ctx_;
};

// --- Factory functions ---

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateResponsesHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateGetResponseHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateListResponsesHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateDeleteResponseHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateGetInputItemsHandler(ServiceContext& ctx);

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
