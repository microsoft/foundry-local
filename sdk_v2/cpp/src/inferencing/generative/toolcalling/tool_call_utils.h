// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/session.h"

#include <string>
#include <vector>

namespace fl {

/// A single parsed tool call extracted from generated text.
struct ParsedToolCall {
  std::string id;         // unique call ID (e.g., "call_abc123")
  std::string name;       // function name
  std::string arguments;  // JSON string of arguments
};

/// Parse tool calls from generated text using start/end marker tokens.
///
/// The model produces text like:
///   <tool_call_start>[{"name":"get_weather","arguments":{"city":"Seattle"}}]<tool_call_end>
///
/// This function extracts all tool call blocks between markers, deserializes the JSON,
/// and returns structured ParsedToolCall objects. If no markers are found or JSON is
/// invalid, returns an empty vector.
///
/// @param text            The full generated text (may contain mixed text and tool calls)
/// @param tool_call_start The start marker token string
/// @param tool_call_end   The end marker token string
/// @param tools_json      Advertised OpenAI-format tools, used to resolve legacy aliases
/// @return                Parsed tool calls, empty if none found
std::vector<ParsedToolCall> ParseToolCalls(const std::string& text,
                                           const std::string& tool_call_start,
                                           const std::string& tool_call_end,
                                           const std::string& tools_json = {});

/// Generate a unique tool call ID (e.g., "call_abc123def").
std::string GenerateToolCallId();

/// Convert parsed tool calls into Item objects of type kToolCall
/// suitable for inclusion in a Response.
std::vector<std::unique_ptr<Item>> ToolCallsToItems(const std::vector<ParsedToolCall>& calls);

}  // namespace fl
