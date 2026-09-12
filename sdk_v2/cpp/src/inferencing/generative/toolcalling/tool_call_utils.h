// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/session.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fl {

/// A single parsed tool call extracted from generated text.
struct ParsedToolCall {
  ParsedToolCall() = default;
  ParsedToolCall(std::string id_in, std::string name_in, std::string arguments_in)
      : id(std::move(id_in)), name(std::move(name_in)), arguments(std::move(arguments_in)) {}

  std::string id;         // unique call ID (e.g., "call_abc123")
  std::string name;       // function name
  std::string arguments;  // Existing semantic form: decoded string or compact JSON.
  /// Exact source bytes for valid JSON; canonical JSON for repaired calls.
  std::string argument_source;
};

/// Find a marker outside JSON string literals. Quote and escape state is
/// tracked from start_pos, which must point at the beginning of the JSON-like
/// payload being scanned.
size_t FindMarkerOutsideJsonString(std::string_view text,
                                   std::string_view marker,
                                   size_t start_pos = 0);

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
/// @param tools_json      Advertised OpenAI-format tools used to validate repaired calls
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
