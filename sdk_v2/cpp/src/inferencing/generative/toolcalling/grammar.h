// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_context.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fl {

/// Build a Lark grammar string for ORT GenAI's SetGuidance.
///
/// Grammar controls whether the model produces text, tool calls, chain-of-thought
/// reasoning, or combinations thereof, using exact token IDs or quoted marker literals when available.
///
/// Legend:
///   cot          = chain-of-thought output with newline at the end
///   THINK_TEXT   = chain-of-thought text output
///   output       = output row (text and/or tool call)
///   TEXT         = text output
///   toolcall     = tool call output (with configured boundary markers)
///   functioncall = JSON schemas for each registered tool
///
/// | Case | Description                                                                                        |
/// |------|----------------------------------------------------------------------------------------------------|
/// |  1   | Return text only                                                                                   |
/// |  2   | Return tool call only (configured tool-call markers)                                               |
/// |  3   | Return tool call only (no configured tool-call markers)                                            |
/// |  4   | Return text or tool call (configured tool-call markers)                                            |
/// |  5   | Return text or tool call (no configured tool-call markers)                                         |
/// |  6   | Return chain-of-thought + text only (configured reasoning markers)                                 |
/// |  7   | Return chain-of-thought + text only (no configured reasoning markers)                              |
/// |  8   | Return chain-of-thought + tool call only (both marker pairs configured)                            |
/// |  9   | Return chain-of-thought + tool call only (only tool-call markers configured)                       |
/// |  10  | Return chain-of-thought + tool call only (only reasoning markers configured)                       |
/// |  11  | Return chain-of-thought + tool call only (no marker pairs configured)                              |
/// |  12  | Return chain-of-thought + text or tool call (both marker pairs configured)                         |
/// |  13  | Return chain-of-thought + text or tool call (only tool-call markers configured)                    |
/// |  14  | Return chain-of-thought + text or tool call (only reasoning markers configured)                    |
/// |  15  | Return chain-of-thought + text or tool call (no marker pairs configured)                           |
///
/// See grammar.cc for the full grammar pattern for each case.
///
/// @param ctx                    Tool calling context with flags and marker metadata
/// @param json_schema            JSON schema string for the tool definitions (may be empty)
/// @param prompt_opens_reasoning Whether the rendered prompt ends inside an open reasoning block
/// @return                       Lark grammar string suitable for SetGuidance("lark_grammar", ...)
std::string BuildLarkGrammar(const ToolCallContext& ctx,
                             const std::string& json_schema,
                             bool prompt_opens_reasoning = false);

/// Build a JSON schema string for tool call guidance.
///
/// Constructs a JSON schema array with anyOf entries for each tool definition.
/// The schema constrains the model's output to valid tool call JSON.
///
/// If no tools or tool_output is false, returns "{}".
///
/// @param ctx  Tool calling context (reads tools_json for tool definitions)
/// @return     JSON schema string suitable for SetGuidance("json_schema", ...)
///             or for embedding in Lark grammar as %json directive
std::string BuildToolJsonSchema(const ToolCallContext& ctx);

/// Escape `text` as a Lark string literal: wraps it in double quotes and escapes the characters that would
/// otherwise break out of the literal or corrupt the rendered grammar — backslash, double quote, and the
/// whitespace control characters (newline, carriage return, tab) that a marker string can plausibly contain.
///
/// @param text  Raw marker text (e.g. "<tool_call>")
/// @return      A double-quoted, escaped Lark string literal (e.g. "\"<tool_call>\"")
std::string EscapeLarkLiteral(const std::string& text);

/// Render one boundary marker for splicing into a Lark grammar production.
///
/// An authoritative single token ID is emitted with llguidance's exact numeric-token syntax (`<[ID]>`).
/// Otherwise the marker text is emitted as an escaped quoted literal, which represents normal bytes and may
/// tokenize to multiple IDs. Named `<token_name>` syntax is reserved for tokenizer special-token names.
///
/// @param marker_text  Raw marker text (e.g. "<tool_call>")
/// @param token_id     Authoritative exact token ID, or nullopt for literal rendering
/// @return             Grammar-ready numeric token terminal or escaped literal
std::string RenderLarkMarker(const std::string& marker_text, std::optional<int32_t> token_id);

}  // namespace fl
