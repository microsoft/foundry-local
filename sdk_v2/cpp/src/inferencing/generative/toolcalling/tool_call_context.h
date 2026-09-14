// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace fl {

/// Request-scoped context for tool calling behavior.
/// Built from catalog metadata + request fields (tool_choice, response_format).
/// Flows through ChatSession → OnnxChatGenerator to control guidance and detection.
struct ToolCallContext {
  /// Whether the model supports tool calling (controlled by catalog metadata and/or request option)
  bool supports_tool_calling = false;

  /// The special token that marks the start of a tool call in generated text.
  /// Empty if the model does not advertise tool call tokens.
  std::string tool_call_start;

  /// The special token that marks the end of a tool call in generated text.
  std::string tool_call_end;

  /// Whether the model may produce plain text output (controlled by tool_choice).
  bool text_output = true;

  /// Whether the model may produce tool call output (controlled by tool_choice + tools presence).
  bool tool_output = false;

  /// Whether the model supports chain-of-thought reasoning (e.g., DeepSeek R1 Distilled).
  /// When true, grammar guidance is applied for text-only output to produce correct <think> tags.
  bool supports_reasoning = false;

  /// The special token that marks the start of reasoning/thinking output.
  /// Empty if the model does not advertise reasoning tokens.
  std::string reasoning_start;

  /// The special token that marks the end of reasoning/thinking output.
  std::string reasoning_end;

  /// Whether the model has known chain-of-thought marker tokens.
  bool HasReasoningTokens() const {
    return !reasoning_start.empty() && !reasoning_end.empty();
  }

  /// Exact token IDs for reasoning boundaries when each marker resolves authoritatively to one nonnegative ID.
  /// llguidance renders these as numeric `<[ID]>` terminals. Unresolved or multi-token markers remain literals.
  std::optional<int32_t> reasoning_start_token_id;
  std::optional<int32_t> reasoning_end_token_id;

  /// The raw tools JSON string for the chat template (passed to ApplyChatTemplate).
  std::string tools_json;

  /// Kind of each named tool, snapshotted from the session's registry at the same moment
  /// `tools_json` was built. Generation resolves a produced call's name through this copy rather
  /// than through the session's registry, which another thread may change mid-turn.
  std::unordered_map<std::string, ToolKind> tool_kinds;

  /// The kind registered for `name` when this context was built. Names the context does not know
  /// resolve to kFunction, which leaves their arguments untouched.
  ToolKind KindOf(const std::string& name) const {
    auto it = tool_kinds.find(name);
    return it == tool_kinds.end() ? ToolKind::kFunction : it->second;
  }

  /// Whether the named tool takes a raw text payload rather than JSON arguments.
  bool IsCustomTool(const std::string& name) const { return KindOf(name) == ToolKind::kCustom; }

  /// User-specified guidance type from response_format (e.g., "lark_grammar", "json_schema").
  /// Empty means no explicit guidance — the generator may still apply auto-generated tool guidance.
  std::string guidance_type;

  /// User-specified guidance data (the LARK grammar string, JSON schema, etc.).
  std::string guidance_data;

  /// Whether any tools were provided in the request.
  bool HasTools() const { return !tools_json.empty(); }

  /// Whether two turns expose the same definitions to the model.
  ///
  /// Kinds are compared as well as the rendered JSON: a custom tool is normalized into a function-shaped schema
  /// before it reaches `tools_json`, so two tool sets can render byte-identical JSON and still differ in how the
  /// calls they produce must be read back.
  bool HasSameTools(const ToolCallContext& other) const {
    return tools_json == other.tools_json && tool_kinds == other.tool_kinds;
  }

  /// Whether the model has known tool call marker tokens.
  bool HasToolCallTokens() const {
    return !tool_call_start.empty() && !tool_call_end.empty();
  }

  /// Exact token IDs for tool-call boundaries when each marker resolves authoritatively to one nonnegative ID.
  /// Start and end are resolved independently; unresolved or multi-token markers remain quoted literals.
  std::optional<int32_t> tool_call_start_token_id;
  std::optional<int32_t> tool_call_end_token_id;
};

}  // namespace fl
