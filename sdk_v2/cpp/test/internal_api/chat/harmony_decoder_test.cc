// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/chat/harmony_decoder.h"
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
