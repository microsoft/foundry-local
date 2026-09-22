// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "contracts/responses.h"
#include "inferencing/generative/openresponses/response_chain.h"
#include "inferencing/session/session.h"
#include "inferencing/session/types.h"

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
/// Handles: instructions → request-scoped system prefix option, string/array input parsing,
/// function_call_output → tool result, parameter mapping.
///
/// @param params            The typed Responses API request parameters.
/// @param previous_context  Fully reconstructed chained context, oldest hop first. Each hop's own input items are
///                          replayed, then its output items as exactly one assistant turn. Every hop becomes a
///                          replay segment so ingestion cannot merge two recorded turns together.
/// @return  A session Request ready for ChatSession::Run().
Request ToSessionRequest(const ResponseCreateParams& params,
                         const ResponseChainContext* previous_context = nullptr);

/// Convert the declared tools into core tool definitions, in declaration order, and translate
/// `tool_choice` into `session_request.options["tool_choice"]` so `SearchOptions::ParseToolChoice`
/// picks it up.
///
/// A forced tool_choice narrows the set to the named tool of the matching kind and forces
/// "required"; `allowed_tools` then intersects what is left. The returned definitions are what the
/// caller registers on the session; the session's registry — not this converter — serializes them
/// for the prompt.
std::vector<fl::ToolDefinition> ExtractResponsesToolDefinitions(const ResponseCreateParams& params,
                                                                Request& session_request);

/// Convert an internal session Response into typed Responses API output items
/// and output_text string.
///
/// Handles: text messages → ResponseOutputMessage, tool calls → FunctionCallOutputItem or
/// CustomToolCallOutputItem, according to the kind the call's tool was registered under.
///
/// @param session_response  The session response from ChatSession::Run().
/// @param msg_id_prefix     Prefix to use for generated message IDs.
/// @return  A pair of (typed output items, output_text string).
std::pair<std::vector<ResponseOutputItem>, std::string> FromSessionResponse(
    const fl::Response& session_response, const std::string& msg_id_prefix = "msg");

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

struct ToolCallStreamOutput {
  std::vector<StreamEvent> events;
  ResponseOutputItem completed_item;
};

/// Build the complete Responses streaming event sequence for an atomically parsed tool call.
///
/// A call is parsed only once its closing marker arrives, so there is no partial payload to stream:
/// the lifecycle is added → payload delta → payload done → item done, emitted back to back. A
/// custom call reports the raw payload through the custom_tool_call_input events; a function call
/// reports JSON arguments through the function_call_arguments events.
ToolCallStreamOutput BuildToolCallStreamOutput(const ToolCallItem& call,
                                               int output_index,
                                               int& next_sequence_number);

/// Convert input items from the request JSON to a storable form.
///
/// Returns a JSON array of exactly what the caller put in `input`, with IDs filled in and function-call arguments
/// canonicalized to a string. `instructions` is request-scoped state and is deliberately not stored: /input_items
/// reports what the caller sent, and a replayed chain takes its instructions from the current request.
nlohmann::json ToInputItems(const nlohmann::json& req_json);

}  // namespace ResponseConverter
}  // namespace fl
