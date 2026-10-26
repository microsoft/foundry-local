// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_context.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fl {

/// Build whole-turn structural guidance from the same plan that validates generated calls.
/// Non-tool turns need no automatic guidance. Explicit caller guidance is selected by ResolveTurnGuidanceOptions.
std::string BuildLarkGrammar(const ToolCallContext& ctx, bool prompt_opens_reasoning = false);

/// Quote literal bytes without allowing marker text to introduce Lark productions.
std::string EscapeLarkLiteral(const std::string& text);

/// Prefer a model-published control identity; unresolved markers use quoted literal bytes, not named tokens.
std::string RenderLarkMarker(const std::string& marker_text, std::optional<int32_t> token_id);

}  // namespace fl
