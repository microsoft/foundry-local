// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "contracts/chat_completions.h"
#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "inferencing/session/types.h"
#include "util/key_value_pairs.h"

#include <foundry_local/foundry_local_c.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fl {
namespace chat_completions {

/// Generate a random completion ID (e.g. "chatcmpl-abc123def").
std::string GenerateCompletionId();

/// Apply catalog model defaults to request fields the user didn't set.
/// Reads directly from the model's model_settings map.
void ApplyCatalogDefaults(ChatCompletionRequest& req, const KeyValuePairs& model_settings);

/// Map flFinishReason to OpenAI finish_reason string ("stop", "length", "tool_calls").
std::string MapFinishReason(flFinishReason reason);

/// Convert ChatCompletionRequest messages to internal request items: messages, the tool calls a
/// prior assistant turn produced, and tool results. A prior call keeps its wire call id so a result
/// sent alongside it stays correlated with it.
void BuildRequestItems(const ChatCompletionRequest& req, Request& session_request);

/// Convert the declared tools into core tool definitions, in declaration order, and translate
/// `tool_choice` into `session_request.options["tool_choice"]`.
///
/// A forced tool_choice narrows the set to the named tool of the matching kind. The returned
/// definitions are what the caller registers on the session; the session's registry — not this
/// converter — serializes them for the prompt.
std::vector<ToolDefinition> ExtractToolDefinitions(const ChatCompletionRequest& req, Request& session_request);

/// Build the Chat Completions wire form of one produced tool call.
///
/// `payload` is the call's arguments as the session resolved them: JSON text for a function tool,
/// the raw model payload for a custom tool. It is never re-parsed or re-encoded here — a custom
/// payload must reach the client byte for byte. The core call id is carried through unchanged so
/// streaming chunks, the final message and any result sent back all name the same call.
ChatCompletionToolCall MakeToolCall(std::string call_id, std::string name, std::string payload, ToolKind kind);

/// Map ChatCompletionRequest parameters (temperature, top_p, etc.) to session option keys.
void MapRequestParameters(const ChatCompletionRequest& req, Request& session_request);

/// Translate response_format into guidance session options.
void MapGuidance(const ChatCompletionRequest& req, Request& session_request);

/// Normalize OpenAI stop strings and store them in the internal request-options channel.
void MapStopSequences(const ChatCompletionRequest& req, Request& session_request);

/// Build a ChatCompletionResponse from an internal Response.
/// Extracts assistant messages and tool calls from response items,
/// populates usage from response.usage.
///
ChatCompletionResponse BuildResponse(const Response& response,
                                     const std::string& completion_id,
                                     int64_t created,
                                     const std::string& model_name);

/// Format a streaming chunk with delta content as JSON string.
std::string FormatStreamingChunk(const std::string& content,
                                 const std::string& completion_id,
                                 int64_t created,
                                 const std::string& model_name);

/// Format a streaming chunk with reasoning_content in the delta as JSON string.
std::string FormatReasoningStreamingChunk(const std::string& reasoning_content,
                                          const std::string& completion_id,
                                          int64_t created,
                                          const std::string& model_name);

/// Format a streaming chunk with tool call data in delta.tool_calls.
std::string FormatToolCallStreamingChunk(const std::vector<ChatCompletionToolCall>& tool_calls,
                                         const std::string& completion_id,
                                         int64_t created,
                                         const std::string& model_name);

/// Format the initial streaming chunk (role=assistant, empty content) as JSON string.
std::string FormatInitialStreamingChunk(const std::string& completion_id,
                                        int64_t created,
                                        const std::string& model_name);

/// Format the final streaming chunk with finish_reason as JSON string.
std::string FormatFinalStreamingChunk(flFinishReason reason,
                                      const std::string& completion_id,
                                      int64_t created,
                                      const std::string& model_name);

}  // namespace chat_completions
}  // namespace fl
