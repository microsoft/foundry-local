// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/grammar.h"

#include "exception.h"
#include "contracts/tool_definitions.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>

namespace fl {

namespace {

std::string BuildHarmonyGrammar(const ToolCallContext& context) {
  if (context.tool_output && !context.tool_plan) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Harmony tool guidance requires an original-schema plan");
  }
  const auto& protocol = *context.harmony;
  const auto token = [](int32_t id) { return "<[" + std::to_string(id) + "]>"; };
  auto excluded = context.eos_token_ids;
  for (const auto id : {protocol.start, protocol.channel, protocol.message, protocol.end,
                        protocol.handoff, protocol.answer, protocol.format}) excluded.push_back(id);
  std::sort(excluded.begin(), excluded.end());
  excluded.erase(std::unique(excluded.begin(), excluded.end()), excluded.end());
  std::ostringstream grammar;
  grammar << "start: " << (context.supports_reasoning ? "reasoning* " : "");
  if (!context.tool_output) {
    grammar << "commentary* final\n";
  } else {
    grammar << (context.text_output ? "commentary* (final | tool)\n" : "tool\n");
  }
  grammar << "reasoning: " << token(protocol.channel) << " \"analysis\" " << token(protocol.message)
          << " plain_token* " << token(protocol.end) << " " << token(protocol.start) << " \"assistant\"\n";
  grammar << "commentary: " << token(protocol.channel) << " \"commentary\" " << token(protocol.message)
          << " plain_token* " << token(protocol.end) << " " << token(protocol.start) << " \"assistant\"\n";
  grammar << "final: " << token(protocol.channel) << " \"final\" " << token(protocol.message)
          << " plain_token* " << token(protocol.answer) << "\n";
  if (context.tool_output) {
    const auto names = context.tool_plan->AllowedNames();
    grammar << "tool: ";
    for (size_t i = 0; i < names.size(); ++i) grammar << (i ? " | " : "") << "tool_" << i;
    grammar << "\n";
    for (size_t i = 0; i < names.size(); ++i) {
      const auto custom = context.UsesNativeRawBody(names[i]);
      grammar << "tool_" << i << ": header_" << i << (custom ? " " : " metadata ")
              << token(protocol.message) << " ";
      const auto format = context.custom_lark_grammars.find(names[i]);
      if (custom && format != context.custom_lark_grammars.end()) {
        if (names[i] != "apply_patch" || format->second != tools::kStockGhcpApplyPatchLarkGrammar) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Unsupported native custom-tool input grammar");
        }
        grammar << "native_patch";
      } else {
        grammar << "arguments";
      }
      grammar << " " << token(protocol.handoff) << "\n";
      const auto recipient = EscapeLarkLiteral("functions." + names[i]);
      grammar << "header_" << i << ": \" to=\" " << recipient << " " << token(protocol.channel)
              << " \"commentary\" | " << token(protocol.channel) << " \"commentary to=\" " << recipient << "\n";
    }
    grammar << "metadata: (\" json\" | " << token(protocol.format) << " \"json\" | \" \" "
            << token(protocol.format) << " \"json\")?\n";
    if (std::any_of(names.begin(), names.end(), [&](const auto& name) {
          return context.UsesNativeRawBody(name);
        })) {
      auto patch = std::string(tools::kStockGhcpApplyPatchLarkGrammar);
      patch.replace(0, std::string_view("start").size(), "native_patch");
      grammar << patch << "\n";
    }
  }
  grammar << "plain_token: <[^";
  for (size_t i = 0; i < excluded.size(); ++i) grammar << (i ? "," : "") << excluded[i];
  grammar << "]>\n";
  if (context.tool_output) grammar << context.tool_plan->ObjectGrammar("arguments");
  return grammar.str();
}

}  // namespace

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
  if (ctx.harmony) return BuildHarmonyGrammar(ctx);
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
