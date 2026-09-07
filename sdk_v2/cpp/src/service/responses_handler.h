// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "inferencing/generative/openresponses/response_converter.h"
#include "service/handler_utils.h"

#include "inferencing/generative/openresponses/response_chain.h"
#include "inferencing/generative/openresponses/response_store.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace fl {

struct ServiceContext;
struct Request;
class ChatSession;
class Model;
class GenAIModelInstance;

namespace responses {
struct ResponseCreateParams;
}  // namespace responses

/// Identity of one response being produced.
struct ResponseTurn {
  std::string response_id;
  int64_t created_at = 0;
  /// The model as the caller named it. Echoed in the response object.
  std::string model_name;
  /// The resolved catalog model id. Bound to the stored response so a later continuation must resolve to the same
  /// model whichever alias it uses.
  std::string model_id;
};

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
                                                            responses::ResponseCreateParams& params,
                                                            Request& prepared_request,
                                                            std::vector<fl::ToolDefinition>& tool_definitions);

  /// Look up model in catalog and verify it's loaded. Sets output pointers.
  /// Returns an error response on failure, nullptr on success.
  std::shared_ptr<OutgoingResponse> ResolveModel(const std::string& model_name,
                                                 Model*& model, GenAIModelInstance*& loaded);

  /// Open the store lease for this request, validating the conversation named by `previous_response_id` against the
  /// model that will run it. Returns an error response when the chain is gone or belongs to another model.
  std::shared_ptr<OutgoingResponse> BeginResponse(const responses::ResponseCreateParams& params,
                                                  const std::string& model_id, ResponseLease& lease);

  /// Reconstruct the full replay context for a chained request by walking `previous_response_id` to the root.
  /// Only called when no cached session is available — a live session already holds the conversation.
  /// Returns an error response when the chain cannot be reconstructed, nullptr on success.
  std::shared_ptr<OutgoingResponse> LoadPreviousContext(const responses::ResponseCreateParams& params,
                                                        ResponseChainContext& context_storage,
                                                        const ResponseChainContext*& previous_context);

  /// Check out the session cached for this request's conversation, or nullptr when there is none.
  std::unique_ptr<ChatSession> CheckOutCachedSession(const responses::ResponseCreateParams& params);

  // --- Inference dispatch ---

  std::shared_ptr<OutgoingResponse> HandleNonStreaming(std::unique_ptr<ChatSession> session, Request& session_request,
                                                       const ResponseTurn& turn, ResponseLease lease,
                                                       const responses::ResponseCreateParams& params,
                                                       const nlohmann::json& req_json);

  std::shared_ptr<OutgoingResponse> HandleStreaming(std::unique_ptr<ChatSession> session, Request session_request,
                                                    const ResponseTurn& turn, ResponseLease lease,
                                                    const responses::ResponseCreateParams& params,
                                                    const nlohmann::json& req_json);

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
