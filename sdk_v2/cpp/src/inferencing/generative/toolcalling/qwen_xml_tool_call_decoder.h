// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_payload_parser.h"
#include "inferencing/session/types.h"

#include <string>
#include <unordered_map>

namespace fl {

/// Creates the exact Qwen native XML decoder for one request's declared OpenAI-format tools.
ToolCallPayloadParser CreateQwenXmlToolCallPayloadParser(
    std::string tools_json, std::unordered_map<std::string, ToolKind> tool_kinds);

}  // namespace fl
