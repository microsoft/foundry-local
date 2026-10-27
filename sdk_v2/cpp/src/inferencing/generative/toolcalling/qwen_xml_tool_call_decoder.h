// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_payload_parser.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/session/types.h"

#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fl {

struct ToolCallContext;
inline constexpr std::string_view kQwenXmlToolCallStartMarker = "<tool_call>";
inline constexpr std::string_view kQwenXmlToolCallEndMarker = "</tool_call>";

/// Creates the exact Qwen native XML decoder for one request's declared OpenAI-format tools.
/// An offered set with malformed or undecodable declarations retains a fail-closed parser; only an absent
/// tool offer returns an empty parser. Unsupported schemas remain ineligible for admission.
/// Calls that violate a declared schema are withheld from visible text and signal recovery or an error.
/// recovery_aware additionally allows structural failures to trigger guided recovery.
ToolCallPayloadParser CreateQwenXmlToolCallPayloadParser(
    std::string tools_json, std::unordered_map<std::string, ToolKind> tool_kinds,
    bool recovery_aware = false);

/// Body after the opening token and before the closing token. Empty means the entire offered set is ineligible.
std::optional<std::string> BuildQwenXmlToolBodyGrammar(
    const std::string& tools_json, const std::unordered_map<std::string, ToolKind>& tool_kinds);

/// Returns internal guidance only for native Qwen XML automatic text-and-tools Engine turns.
std::optional<std::string> PlanQwenXmlToolBodyGuidance(
    const ToolCallContext& context, bool native_qwen_xml, ChatBackendKind backend_kind);

/// Parses the canonical JSON payload produced by guided tool-call recovery.
///
/// Unlike the general tool-call parser, this performs no repair: every call in the batch must
/// exactly match its effective Qwen schema.
std::vector<ParsedToolCall> ParseQwenGuidedToolCalls(
    std::string_view payload,
    const std::string& tools_json,
    const std::unordered_map<std::string, ToolKind>& tool_kinds);

}  // namespace fl
