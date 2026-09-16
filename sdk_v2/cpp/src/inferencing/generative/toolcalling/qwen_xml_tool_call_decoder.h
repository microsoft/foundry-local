// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_payload_parser.h"
#include "inferencing/session/types.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace fl {

inline constexpr std::string_view kQwenXmlToolCallStartMarker = "<tool_call>";
inline constexpr std::string_view kQwenXmlToolCallEndMarker = "</tool_call>";

/// Creates the exact Qwen native XML decoder for one request's declared OpenAI-format tools.
/// Returns an empty parser when any declaration is outside the decoder's supported schema subset.
ToolCallPayloadParser CreateQwenXmlToolCallPayloadParser(
    std::string tools_json, std::unordered_map<std::string, ToolKind> tool_kinds);

}  // namespace fl
