// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>

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

  /// The raw tools JSON string for the chat template (passed to ApplyChatTemplate).
  std::string tools_json;

  /// User-specified guidance type from response_format (e.g., "lark_grammar", "json_schema").
  /// Empty means no explicit guidance — the generator may still apply auto-generated tool guidance.
  std::string guidance_type;

  /// User-specified guidance data (the LARK grammar string, JSON schema, etc.).
  std::string guidance_data;

  bool operator==(const ToolCallContext&) const = default;

  /// Whether any tools were provided in the request.
  bool HasTools() const { return !tools_json.empty(); }

  /// Whether the model has known tool call marker tokens.
  bool HasToolCallTokens() const {
    return !tool_call_start.empty() && !tool_call_end.empty();
  }
};

}  // namespace fl
