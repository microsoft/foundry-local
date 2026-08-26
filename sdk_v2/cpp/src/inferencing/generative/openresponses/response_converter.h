// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "contracts/responses.h"
#include "inferencing/session/session.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace fl {

struct ToolCallItem;

/// Shared utilities for converting between Responses API format and internal
/// session types. Used by both the web service handler and the direct
/// ResponsesClient API.
namespace ResponseConverter {

using namespace fl::responses;

/// Generate a unique ID with prefix (e.g. "resp", "msg", "fc").
std::string GenerateId(const std::string& prefix);

/// Build an internal session Request from typed Responses API parameters.
/// Handles: instructions → system message, string/array input parsing,
/// function_call_output → tool result, parameter mapping.
///
/// @param params            The typed Responses API request parameters.
/// @param previous_input    Input items from a previous response (JSON, for chaining).
/// @param previous_output   Output items from a previous response (JSON, for chaining).
/// @return  A session Request ready for ChatSession::Run().
Request ToSessionRequest(const ResponseCreateParams& params,
                         const nlohmann::json* previous_input = nullptr,
                         const nlohmann::json* previous_output = nullptr);

/// Extract tool definitions from the Responses request, mirroring the chat-completions
/// `ExtractToolDefinitions` helper. Returns a pre-serialized JSON array of tools in the
/// chat-template (OpenAI nested) format, and sets `session_request.options["tool_choice"]`
/// from `params.tool_choice` so that `SearchOptions::ParseToolChoice` picks it up.
///
/// For ForcedFunction tool_choice, the returned tools array is filtered to just the named
/// function and tool_choice is set to "required" (matches chat-completions behavior).
///
/// The caller is expected to attach the returned JSON to the session via
/// `session->AddToolDefinition({{}, {}, std::move(tools_json)})` — the empty-name entry is
/// recognized by `ChatSession::BuildToolCallContext` as a pre-serialized full tools array.
std::string ExtractResponsesToolDefinitions(const ResponseCreateParams& params, Request& session_request);

/// Convert an internal session Response into typed Responses API output items
/// and output_text string.
///
/// Handles: text messages → ResponseOutputMessage, tool calls → FunctionCallOutputItem
///
/// @param session_response  The session response from ChatSession::Run().
/// @param msg_id_prefix     Prefix to use for generated message IDs.
/// @return  A pair of (typed output items, output_text string).
std::pair<std::vector<ResponseOutputItem>, std::string> FromSessionResponse(const fl::Response& session_response,
                                                                            const std::string& msg_id_prefix = "msg");

/// Build the complete typed Responses API response object.
///
/// @param response_id    The response ID.
/// @param created_at     Unix timestamp.
/// @param model_name     The model name.
/// @param params         The original typed request parameters (to echo back).
/// @param output         The typed output items from FromSessionResponse.
/// @param output_text    The output text from FromSessionResponse.
/// @param usage          Token usage from the session response.
/// @return  Complete typed ResponseObject.
ResponseObject BuildResponseObject(const std::string& response_id,
                                   int64_t created_at,
                                   const std::string& model_name,
                                   const ResponseCreateParams& params,
                                   std::vector<ResponseOutputItem> output,
                                   const std::string& output_text,
                                   const TokenUsage& usage);

/// Build a failed typed Responses API response object.
ResponseObject BuildFailedResponseObject(const std::string& response_id,
                                         int64_t created_at,
                                         const std::string& model_name,
                                         const ResponseCreateParams& params,
                                         const std::string& error_code,
                                         const std::string& error_message);

/// Build the initial in-progress typed response object for streaming.
ResponseObject BuildInitialResponseObject(const std::string& response_id,
                                          int64_t created_at,
                                          const std::string& model_name,
                                          const ResponseCreateParams& params);

struct FunctionCallStreamOutput {
  std::vector<StreamEvent> events;
  FunctionCallOutputItem completed_item;
};

/// Build the complete Responses streaming event sequence for an atomically parsed function call.
FunctionCallStreamOutput BuildFunctionCallStreamOutput(const ToolCallItem& call,
                                                       int output_index,
                                                       int& next_sequence_number);

/// Convert input items from the request JSON to a storable form.
/// Returns a JSON array of input items (instructions as system message + input).
nlohmann::json ToInputItems(const nlohmann::json& req_json);

}  // namespace ResponseConverter
}  // namespace fl
