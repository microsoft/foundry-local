// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_payload_parser.h"
#include "inferencing/session/types.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fl {

inline constexpr std::string_view kQwenXmlToolCallStartMarker = "<tool_call>";
inline constexpr std::string_view kQwenXmlToolCallEndMarker = "</tool_call>";

/// Creates the exact Qwen native XML decoder for one request's declared OpenAI-format tools.
/// Returns an empty parser when any declaration is outside the decoder's supported schema subset.
ToolCallPayloadParser CreateQwenXmlToolCallPayloadParser(
    std::string tools_json, std::unordered_map<std::string, ToolKind> tool_kinds,
    bool recovery_aware = false);

/// Parses the canonical JSON payload produced by guided tool-call recovery.
///
/// Unlike the general tool-call parser, this performs no repair: every call in the batch must
/// exactly match its effective Qwen schema.
std::vector<ParsedToolCall> ParseQwenGuidedToolCalls(
    std::string_view payload,
    const std::string& tools_json,
    const std::unordered_map<std::string, ToolKind>& tool_kinds);

}  // namespace fl
