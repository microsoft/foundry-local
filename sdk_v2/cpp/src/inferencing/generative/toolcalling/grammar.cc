// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/grammar.h"

#include "exception.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>

namespace fl {

std::string EscapeLarkLiteral(const std::string& text) {
  return nlohmann::json(text).dump();
}

std::string RenderLarkMarker(const std::string& marker_text, std::optional<int32_t> token_id) {
  if (token_id && *token_id >= 0) {
    return "<[" + std::to_string(*token_id) + "]>";
  }

  return EscapeLarkLiteral(marker_text);
}

std::string BuildLarkGrammar(const ToolCallContext& ctx, bool prompt_opens_reasoning) {
  if (!ctx.text_output && !ctx.tool_output) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "neither text output nor tool calling output is enabled");
  }
  if (!ctx.tool_output) {
    return {};
  }
  if (!ctx.tool_plan || !ctx.HasToolCallTokens()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "structured tool guidance requires a plan and tool boundary markers");
  }

  const auto valid_id = [](std::optional<int32_t> id) { return id && *id >= 0; };
  if (ctx.supports_reasoning &&
      (!valid_id(ctx.reasoning_start_token_id) || !valid_id(ctx.reasoning_end_token_id) ||
       ctx.reasoning_start_token_id == ctx.reasoning_end_token_id)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "reasoning tool calls require distinct model-published reasoning boundary tokens");
  }
  if (ctx.text_output &&
      (!valid_id(ctx.tool_call_start_token_id) || !valid_id(ctx.tool_call_end_token_id) ||
       ctx.tool_call_start_token_id == ctx.tool_call_end_token_id)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "automatic tool calls require distinct model-published tool boundary tokens");
  }

  auto excluded = ctx.eos_token_ids;
  for (const auto id : {ctx.tool_call_start_token_id, ctx.tool_call_end_token_id,
                        ctx.reasoning_start_token_id, ctx.reasoning_end_token_id}) {
    if (valid_id(id)) {
      excluded.push_back(*id);
    }
  }
  std::sort(excluded.begin(), excluded.end());
  excluded.erase(std::unique(excluded.begin(), excluded.end()), excluded.end());

  std::ostringstream grammar;
  grammar << "start: " << (ctx.supports_reasoning ? "cot " : "") << "output\n";
  if (ctx.text_output) {
    grammar << "output: text toolcall | text | toolcall\ntext: "
            << EscapeLarkLiteral(std::string(ToolCallContext::kAssistantTextPrefix)) << " plain_token+\n";
  } else {
    grammar << "output: toolcall\n";
  }
  if (ctx.supports_reasoning) {
    grammar << "cot: ";
    if (!prompt_opens_reasoning) {
      grammar << RenderLarkMarker(ctx.reasoning_start, ctx.reasoning_start_token_id) << " ";
    }
    grammar << "plain_token* " << RenderLarkMarker(ctx.reasoning_end, ctx.reasoning_end_token_id) << " \"\\n\"\n";
  }
  if (ctx.text_output || ctx.supports_reasoning) {
    // Added tokens need not be special. Exclude control identities, not their ordinary byte spellings.
    grammar << "plain_token: <[^";
    for (size_t i = 0; i < excluded.size(); ++i) {
      grammar << (i == 0 ? "" : ",") << excluded[i];
    }
    grammar << "]>\n";
  }
  grammar << "toolcall: " << RenderLarkMarker(ctx.tool_call_start, ctx.tool_call_start_token_id)
          << " functioncall " << RenderLarkMarker(ctx.tool_call_end, ctx.tool_call_end_token_id) << "\n";
  grammar << ctx.tool_plan->Grammar("functioncall");
  return grammar.str();
}

}  // namespace fl
