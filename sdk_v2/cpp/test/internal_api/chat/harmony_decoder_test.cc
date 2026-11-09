// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "exception.h"
#include "contracts/tool_definitions.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/chat/harmony_decoder.h"
#include "inferencing/generative/openresponses/response_converter.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

using namespace fl;

namespace {

const HarmonyProtocol kProtocol{
    10, 11, 12, 13, 14, 15, 16, {10, 17}};

constexpr const char* kTools = R"([
  {"type":"function","function":{
    "name":"lookup",
    "parameters":{
      "type":"object",
      "properties":{"city":{"type":"string"}},
      "required":["city"]
    }
  }},
  {"type":"function","function":{
    "name":"patch",
    "parameters":{
      "type":"object",
      "properties":{"input":{"type":"string"}},
      "required":["input"]
    }
  }}
])";

PreparedChatPrompt MakePrompt() {
  PreparedChatPrompt prompt;
  prompt.prompt = "prefix<|start|>assistant";
  prompt.token_ids = {99, kProtocol.start, 17};
  prompt.harmony_continuation = HarmonyContinuation{
      "assistant", kProtocol.assistant_prefix_tokens.size(),
      std::char_traits<char>::length(HarmonyProtocol::kAssistantPrefix)};
  return prompt;
}

void PushHeader(HarmonyDecoder& decoder, std::string recipient, bool json) {
  EXPECT_FALSE(decoder.Push(100, " to=functions.").malformed);
  EXPECT_FALSE(decoder.Push(101, std::move(recipient)).malformed);
  EXPECT_FALSE(decoder.Push(kProtocol.channel, {}).malformed);
  EXPECT_FALSE(decoder.Push(102, "commentary").malformed);
  if (json) {
    EXPECT_FALSE(decoder.Push(kProtocol.format, {}).malformed);
    EXPECT_FALSE(decoder.Push(103, "json").malformed);
  }
  EXPECT_FALSE(decoder.Push(kProtocol.message, {}).malformed);
}

const ParsedToolCall& OnlyCall(const HarmonyDecoder::Output& output) {
  EXPECT_FALSE(output.malformed) << output.error;
  EXPECT_EQ(output.events.size(), 1u);
  const auto* call = output.events.empty() ? nullptr : std::get_if<ParsedToolCall>(&output.events.front());
  EXPECT_NE(call, nullptr);
  static const ParsedToolCall empty;
  return call == nullptr ? empty : *call;
}

}  // namespace

TEST(HarmonyDecoderTest, UsesControlIdsWithEmptyDecodedTextAndAcceptsSplitJsonHeader) {
  auto prompt = MakePrompt();
  HarmonyDecoder decoder(kProtocol, ToolCallPlan::Create(kTools), prompt);

  PushHeader(decoder, "lookup", true);
  constexpr std::string_view arguments = R"({"city": "Paris"})";
  EXPECT_FALSE(decoder.Push(104, std::string(arguments)).malformed);
  const auto output = decoder.Finish(kProtocol.handoff, true);
  const auto& call = OnlyCall(output);

  EXPECT_EQ(call.name, "lookup");
  EXPECT_EQ(call.arguments, arguments);
  EXPECT_EQ(call.argument_source, arguments);
  EXPECT_FALSE(call.id.empty());
}

TEST(HarmonyDecoderTest, LiteralControlSpellingsCannotManufactureHeaderMetadata) {
  auto prompt = MakePrompt();
  HarmonyDecoder decoder(kProtocol, ToolCallPlan::Create(kTools), prompt);

  EXPECT_FALSE(decoder.Push(100, "<|channel|>commentary").malformed);
  const auto output = decoder.Push(kProtocol.message, {});

  EXPECT_TRUE(output.malformed);
  EXPECT_NE(output.error.find("exactly one channel"), std::string::npos);
}

TEST(HarmonyDecoderTest, TerminalMetadataCompletesToolAndFinalMessages) {
  auto tool_prompt = MakePrompt();
  HarmonyDecoder tool_decoder(kProtocol, ToolCallPlan::Create(kTools), tool_prompt);
  PushHeader(tool_decoder, "lookup", true);
  EXPECT_FALSE(tool_decoder.Push(104, R"({"city":"Paris"})").malformed);
  EXPECT_EQ(OnlyCall(tool_decoder.Finish(kProtocol.handoff, true)).name, "lookup");

  auto final_prompt = MakePrompt();
  HarmonyDecoder final_decoder(kProtocol, nullptr, final_prompt);
  EXPECT_FALSE(final_decoder.Push(kProtocol.channel, {}).malformed);
  EXPECT_FALSE(final_decoder.Push(105, "final").malformed);
  EXPECT_FALSE(final_decoder.Push(kProtocol.message, {}).malformed);
  EXPECT_FALSE(final_decoder.Push(106, "done").malformed);
  const auto final = final_decoder.Finish(kProtocol.answer, true);
  EXPECT_FALSE(final.malformed) << final.error;
  EXPECT_TRUE(final.events.empty());
}

TEST(HarmonyDecoderTest, InterruptedOrMalformedCallsFailWithoutPublishing) {
  auto interrupted_prompt = MakePrompt();
  HarmonyDecoder interrupted(kProtocol, ToolCallPlan::Create(kTools), interrupted_prompt);
  PushHeader(interrupted, "lookup", true);
  EXPECT_FALSE(interrupted.Push(104, R"({"city":"Paris"})").malformed);
  const auto interrupted_output = interrupted.Finish(std::nullopt, true);
  EXPECT_TRUE(interrupted_output.malformed);
  EXPECT_TRUE(interrupted_output.events.empty());

  auto unauthorized_prompt = MakePrompt();
  HarmonyDecoder unauthorized(kProtocol, ToolCallPlan::Create(kTools), unauthorized_prompt);
  PushHeader(unauthorized, "missing", true);
  EXPECT_FALSE(unauthorized.Push(104, R"({"city":"Paris"})").malformed);
  const auto unauthorized_output = unauthorized.Push(kProtocol.handoff, {});
  EXPECT_TRUE(unauthorized_output.malformed);
  EXPECT_TRUE(unauthorized_output.events.empty());
}

TEST(HarmonyDecoderTest, SelectedRecipientCapturesARecoveryCheckpointBeforeItsHeader) {
  auto prompt = MakePrompt();
  HarmonyDecoder decoder(kProtocol, ToolCallPlan::Create(kTools), prompt);

  EXPECT_FALSE(decoder.Push(100, " to=func").malformed);
  EXPECT_TRUE(decoder.HasRecoveryCheckpoint());
  EXPECT_FALSE(decoder.Push(101, "tions.lookup").malformed);
  ASSERT_TRUE(decoder.HasRecoveryCheckpoint());

  const auto recovered = decoder.TakeRecoveryPrompt();
  EXPECT_EQ(recovered.prompt, "prefix");
  EXPECT_EQ(recovered.token_ids, (std::vector<int32_t>{99}));
}

TEST(HarmonyDecoderTest, NativeRawBodyUsesInputEnvelopeWithoutLosingSourceBytes) {
  auto prompt = MakePrompt();
  HarmonyDecoder decoder(kProtocol, ToolCallPlan::Create(kTools), prompt, {"patch"});

  PushHeader(decoder, "patch", false);
  const std::string body = "*** Begin Patch\n+literal <|call|>\n*** End Patch";
  EXPECT_FALSE(decoder.Push(104, body).malformed);
  const auto output = decoder.Push(kProtocol.handoff, {});
  const auto& call = OnlyCall(output);
  const auto expected = nlohmann::json{{"input", body}}.dump();

  EXPECT_EQ(call.name, "patch");
  EXPECT_EQ(call.arguments, expected);
  EXPECT_EQ(call.argument_source, expected);
}

TEST(HarmonyDecoderTest, PromptProjectionPreservesNativeJsonRawAndResultProvenance) {
  ToolCallContext context;
  context.harmony = kProtocol;
  context.tools_json = kTools;
  context.tool_kinds = {{"lookup", ToolKind::kFunction}, {"patch", ToolKind::kCustom}};
  context.custom_lark_grammars = {{"patch", "start: /[\\s\\S]+/"}};

  TranscriptMessage raw_assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
  raw_assistant.AppendToolCall(
      MakeGeneratedToolCall("call_raw", "patch", "raw <|call|> text",
                            ToolKind::kCustom, GeneratedCallEncoding::kNativeRaw)
          .call);
  TranscriptMessage json_assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
  const std::string json_arguments = R"({"path":"a\\b","text":"<|call|>"})";
  json_assistant.AppendToolCall(
      MakeGeneratedToolCall("call_json", "lookup", json_arguments,
                            ToolKind::kFunction, GeneratedCallEncoding::kNativeJson)
          .call);
  const std::vector<TranscriptMessage> messages{
      std::move(raw_assistant),
      TranscriptMessage::ToolResult("call_raw", "raw result"),
      std::move(json_assistant),
      TranscriptMessage::ToolResult("call_json", "json result"),
  };

  const auto projection = BuildHarmonyPromptProjection(messages, context, [](std::string value) {
    return value;
  });
  const auto projected = nlohmann::json::parse(projection.messages_json);
  ASSERT_EQ(projected.size(), 5u);
  EXPECT_EQ(projected[1]["tool_calls"][0]["function"]["arguments_json"], "raw <|call|> text");
  EXPECT_TRUE(projected[1]["tool_calls"][0]["function"]["native_freeform"]);
  EXPECT_EQ(projected[2]["content_json"], nlohmann::json("raw result").dump());
  EXPECT_EQ(projected[3]["tool_calls"][0]["function"]["arguments_json"], json_arguments);
  EXPECT_FALSE(projected[3]["tool_calls"][0]["function"]["native_freeform"]);
  EXPECT_EQ(projected[4]["content_json"], nlohmann::json("json result").dump());

  const auto projected_tools = nlohmann::json::parse(projection.tools_json);
  EXPECT_FALSE(projected_tools[0]["function"].contains("native_freeform"));
  EXPECT_TRUE(projected_tools[1]["function"]["native_freeform"].get<bool>());
}

TEST(HarmonyDecoderTest, StoredInputAndOutputReplayRetainHistoricalCustomBodyEncoding) {
  const std::string body = R"({"input":"literal <|call|> and a\\b"})";
  for (const auto encoding : {GeneratedCallEncoding::kStructured, GeneratedCallEncoding::kNativeJson,
                              GeneratedCallEncoding::kNativeRaw}) {
    for (const bool current_raw : {false, true}) {
      for (const bool stored_input : {false, true}) {
        SCOPED_TRACE(static_cast<int>(encoding));
        SCOPED_TRACE(current_raw);
        SCOPED_TRACE(stored_input);
        const auto call = nlohmann::json{{"type", "custom_tool_call"}, {"call_id", "call_history"}, {"name", "patch"}, {"input", body}};
        auto input = nlohmann::json::array({{{"role", "user"}, {"content", "patch"}}});
        if (stored_input) {
          input.push_back(call);
        }
        const auto output = stored_input ? nlohmann::json::array() : nlohmann::json::array({call});
        ResponseStore store;
        store.Store("history", {{"output", output}}, input, "harmony",
                    ToolCallEncodings{{"call_history", encoding}});
        const auto chain = store.BuildChainContext("history");
        ASSERT_TRUE(chain.has_value());
        responses::ResponseCreateParams params;
        params.model = "harmony";
        params.input = "continue";
        auto request = ResponseConverter::ToSessionRequest(params, &*chain);
        const auto replay = IngestRequestItems(request.items, request.item_segment_starts);
        std::size_t call_count = 0;
        for (const auto& message : replay.messages) {
          for (const auto* historical : message.ToolCalls()) {
            ++call_count;
            EXPECT_EQ(historical->generated_encoding, encoding);
            EXPECT_TRUE(historical->has_recorded_encoding);
            EXPECT_EQ(historical->kind, ToolKind::kCustom);
            EXPECT_EQ(historical->arguments, body);
          }
        }
        EXPECT_EQ(call_count, 1u);

        ToolCallContext context;
        context.harmony = kProtocol;
        context.tools_json = kTools;
        context.tool_kinds = {{"patch", ToolKind::kCustom}};
        if (current_raw) {
          context.custom_lark_grammars = {{"patch", "start: /[\\s\\S]+/"}};
        }
        const auto projected = nlohmann::json::parse(
            BuildHarmonyPromptProjection(replay.messages, context, [](std::string value) { return value; })
                .messages_json);
        call_count = 0;
        for (const auto& message : projected) {
          if (!message.contains("tool_calls")) {
            continue;
          }
          for (const auto& historical : message["tool_calls"]) {
            ++call_count;
            const auto& function = historical["function"];
            const bool raw = encoding == GeneratedCallEncoding::kNativeRaw;
            const auto expected = raw ? body : nlohmann::json{{"input", body}}.dump();
            EXPECT_EQ(function["native_freeform"], raw);
            EXPECT_EQ(function["arguments_json"], expected);
          }
        }
        EXPECT_EQ(call_count, 1u);
      }
    }
  }
}

TEST(HarmonyDecoderTest, UnannotatedCallerHistoryStillUsesTheCurrentCustomBodyDeclaration) {
  for (const bool current_raw : {false, true}) {
    ToolCallContext context;
    context.harmony = kProtocol;
    context.tools_json = kTools;
    context.tool_kinds = {{"patch", ToolKind::kCustom}};
    if (current_raw) {
      context.custom_lark_grammars = {{"patch", "start: /[\\s\\S]+/"}};
    }
    const std::string body = R"({"input":"caller text"})";
    TranscriptMessage assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
    assistant.AppendToolCall(MakeSuppliedToolCall("caller", "patch", body, ToolKind::kCustom));
    const auto projected = nlohmann::json::parse(
        BuildHarmonyPromptProjection({assistant}, context, [](std::string value) { return value; })
            .messages_json);
    const auto& function = projected.back()["tool_calls"][0]["function"];
    EXPECT_EQ(function["native_freeform"], current_raw);
    const auto expected = current_raw ? body : nlohmann::json{{"input", body}}.dump();
    EXPECT_EQ(function["arguments_json"], expected);
  }
}

TEST(HarmonyGrammarTest, ChoicesKeepAuthoritativeHeadersBoundariesAndAllowedRecipients) {
  for (const bool reasoning : {false, true}) {
    for (const std::string choice : {"auto", "required", "forced", "none"}) {
      SCOPED_TRACE(reasoning);
      SCOPED_TRACE(choice);
      ToolCallContext context;
      context.harmony = kProtocol;
      context.supports_reasoning = reasoning;
      context.text_output = choice == "auto" || choice == "none";
      context.tool_output = choice != "none";
      context.eos_token_ids = {99, kProtocol.handoff, 99};
      if (context.tool_output) {
        const auto forced = choice == "forced"
                                ? std::optional<ForcedToolChoice>{{"lookup", ToolKind::kFunction}}
                                : std::nullopt;
        context.tool_plan = ToolCallPlan::Create(kTools, true, forced);
      }
      const auto grammar = BuildLarkGrammar(context);
      const std::string mode = choice == "none"   ? "commentary* final"
                               : choice == "auto" ? "commentary* (final | tool)"
                                                  : "tool";
      EXPECT_TRUE(grammar.starts_with("start: " + std::string(reasoning ? "reasoning* " : "") + mode + "\n"));
      EXPECT_NE(grammar.find("plain_token: <[^10,11,12,13,14,15,16,99]>"), std::string::npos);
      EXPECT_NE(grammar.find("final: <[11]> \"final\" <[12]> plain_token* <[15]>"), std::string::npos);
      if (context.tool_output) {
        EXPECT_NE(grammar.find("metadata <[12]> arguments <[14]>"), std::string::npos);
        EXPECT_NE(grammar.find("\" to=\" \"functions.lookup\" <[11]> \"commentary\""), std::string::npos);
        EXPECT_NE(grammar.find("<[11]> \"commentary to=\" \"functions.lookup\""), std::string::npos);
        EXPECT_EQ(grammar.find("\"functions.patch\"") != std::string::npos, choice != "forced");
        EXPECT_NE(grammar.find("arguments:"), std::string::npos);
      } else {
        EXPECT_EQ(grammar.find("tool:"), std::string::npos);
        EXPECT_EQ(grammar.find("arguments:"), std::string::npos);
      }
    }
  }
}

TEST(HarmonyGrammarTest, NativePatchBodiesUseTheirGrammarAndUnsupportedFormatsFailExplicitly) {
  ToolCallContext context;
  context.harmony = kProtocol;
  context.tool_output = true;
  context.text_output = false;
  EXPECT_THROW(BuildLarkGrammar(context), fl::Exception);
  context.tool_plan = ToolCallPlan::Create(R"([{"type":"function","function":{
    "name":"apply_patch","parameters":{"type":"object","properties":{"input":{"type":"string"}}}}}])");
  context.tool_kinds = {{"apply_patch", ToolKind::kCustom}};
  context.custom_lark_grammars = {{"apply_patch", std::string(tools::kStockGhcpApplyPatchLarkGrammar)}};
  const auto grammar = BuildLarkGrammar(context);
  EXPECT_NE(grammar.find("tool_0: header_0 <[12]> native_patch <[14]>"), std::string::npos);
  EXPECT_NE(grammar.find("native_patch:"), std::string::npos);
  context.custom_lark_grammars["apply_patch"] = "start: /[\\s\\S]+/";
  EXPECT_THROW(BuildLarkGrammar(context), fl::Exception);
}

TEST(HarmonyDecoderTest, PromptProjectionRejectsNonLeadingInstructionRoles) {
  for (const auto* tools : {"", kTools}) {
    ToolCallContext context;
    context.harmony = kProtocol;
    context.tools_json = tools;
    for (const auto role : {FOUNDRY_LOCAL_ROLE_SYSTEM, FOUNDRY_LOCAL_ROLE_DEVELOPER}) {
      const auto protect = [](std::string value) { return value; };
      EXPECT_NO_THROW(BuildHarmonyPromptProjection(
          {{role, "Follow these instructions."}, {FOUNDRY_LOCAL_ROLE_USER, "Hello"}}, context, protect));
      try {
        BuildHarmonyPromptProjection(
            {{FOUNDRY_LOCAL_ROLE_USER, "Hello"}, {role, "Do not omit this instruction."}, {FOUNDRY_LOCAL_ROLE_USER, "Continue"}},
            context, protect);
        FAIL() << "Expected non-leading instructions to be rejected";
      } catch (const fl::Exception& error) {
        EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
      }
    }
  }
}

TEST(HarmonyDecoderTest, PromptProjectionRejectsHeaderSeparatorsInToolAndHistoryNames) {
  for (const char separator : std::string(" \t\r\n\v\f\x01\x02")) {
    const std::string name = "lookup" + std::string(1, separator) + "city";
    for (const bool history : {false, true}) {
      ToolCallContext context;
      context.harmony = kProtocol;
      context.tools_json = history ? kTools : nlohmann::json::array({{{"type", "function"}, {"function", {{"name", name}, {"parameters", {{"type", "object"}}}}}}}).dump();
      std::vector<TranscriptMessage> messages{{FOUNDRY_LOCAL_ROLE_USER, "Hello"}};
      if (history) {
        TranscriptMessage assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
        assistant.AppendToolCall(MakeSuppliedToolCall("call_bad", name, "{}"));
        messages.push_back(std::move(assistant));
      }
      try {
        BuildHarmonyPromptProjection(messages, context, [](std::string value) { return value; });
        FAIL() << "Expected an unrepresentable Harmony recipient to be rejected";
      } catch (const fl::Exception& error) {
        EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
      }
    }
  }
}
