// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "contracts/chat_completions.h"
#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "util/key_value_pairs.h"

#include <foundry_local/foundry_local_c.h>

#include <cstdint>
#include <string>

namespace fl {
namespace chat_completions {

/// Generate a random completion ID (e.g. "chatcmpl-abc123def").
std::string GenerateCompletionId();

/// Apply catalog model defaults to request fields the user didn't set.
/// Reads directly from the model's model_settings map.
void ApplyCatalogDefaults(ChatCompletionRequest& req, const KeyValuePairs& model_settings);

/// Map flFinishReason to OpenAI finish_reason string ("stop", "length", "tool_calls").
std::string MapFinishReason(flFinishReason reason);

/// Convert ChatCompletionRequest messages to internal MessageItem/ToolResultItem items.
void BuildRequestItems(const ChatCompletionRequest& req, Request& session_request);

/// Extract tool definitions from the request. Applies tool_choice filtering.
/// Sets tool_choice, tool_call_start, tool_call_end in session_request.options.
/// Returns the tools JSON string (empty if no tools).
std::string ExtractToolDefinitions(ChatCompletionRequest& req, Request& session_request);

/// Map ChatCompletionRequest parameters (temperature, top_p, etc.) to session option keys.
void MapRequestParameters(const ChatCompletionRequest& req, Request& session_request);

/// Translate response_format into guidance session options.
void MapGuidance(const ChatCompletionRequest& req, Request& session_request);

/// Map stop sequences to early_stopping option.
void MapStopSequences(const ChatCompletionRequest& req, Request& session_request);

/// Build a ChatCompletionResponse from an internal Response.
/// Extracts assistant messages and tool calls from response items,
/// populates usage from response.usage.
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
