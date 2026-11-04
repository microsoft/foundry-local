// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "exception.h"
#include "inferencing/generative/toolcalling/assistant_text_framer.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"

#include <gtest/gtest.h>

using namespace fl;

namespace {

ToolCallContext MakeContext() {
  ToolCallContext context;
  context.tools_json = R"([{"type":"function","function":{"name":"lookup","parameters":{
    "type":"object","properties":{"city":{"type":"string","minLength":1}},"required":["city"]}}}])";
  context.tool_plan = ToolCallPlan::Create(context.tools_json);
  context.tool_output = true;
  context.tool_call_start = "<tool_call>";
  context.tool_call_end = "</tool_call>";
  context.tool_call_start_token_id = 101;
  context.tool_call_end_token_id = 102;
  context.reasoning_start = "<think>";
  context.reasoning_end = "</think>";
  context.reasoning_start_token_id = 103;
  context.reasoning_end_token_id = 104;
  context.eos_token_ids = {2, 1, 2};
  return context;
}

TEST(BuildLarkGrammarTest, AutomaticOutputSeparatesControlIdentitiesFromLiteralData) {
  const auto grammar = BuildLarkGrammar(MakeContext());
  EXPECT_NE(grammar.find("output: text toolcall | text | toolcall"), std::string::npos);
  EXPECT_NE(grammar.find("text: \"MESSAGE\\n\" plain_token+"), std::string::npos);
  EXPECT_NE(grammar.find("plain_token: <[^1,2,101,102,103,104]>"), std::string::npos);
  EXPECT_NE(grammar.find("toolcall: <[101]> functioncall <[102]>"), std::string::npos);
  EXPECT_NE(grammar.find("functioncall:"), std::string::npos);
  EXPECT_EQ(grammar.find("%json"), std::string::npos);
}

TEST(BuildLarkGrammarTest, RequiredOutputCannotFallBackToText) {
  auto context = MakeContext();
  context.text_output = false;
  const auto grammar = BuildLarkGrammar(context);
  EXPECT_TRUE(grammar.starts_with("start: output\noutput: toolcall\n"));
  EXPECT_EQ(grammar.find("plain_token:"), std::string::npos);
}

TEST(BuildLarkGrammarTest, ReasoningHasTokenBoundariesAndDoesNotForbidLiteralAngleBrackets) {
  auto context = MakeContext();
  context.supports_reasoning = true;
  const auto closed = BuildLarkGrammar(context);
  const auto open = BuildLarkGrammar(context, true);
  EXPECT_NE(closed.find("cot: <[103]> plain_token* <[104]>"), std::string::npos);
  EXPECT_NE(open.find("cot: plain_token* <[104]>"), std::string::npos);
  EXPECT_EQ(open.find("cot: <[103]>"), std::string::npos);
  EXPECT_EQ(closed.find("[^<]"), std::string::npos);
}

TEST(BuildLarkGrammarTest, MissingPlansAndAmbiguousOrUnresolvedAutomaticBoundariesAreExplicitErrors) {
  auto context = MakeContext();
  context.tool_plan.reset();
  EXPECT_THROW(BuildLarkGrammar(context), Exception);
  context = MakeContext();
  context.tool_call_start_token_id.reset();
  EXPECT_THROW(BuildLarkGrammar(context), Exception);
  context = MakeContext();
  context.tool_call_end_token_id = context.tool_call_start_token_id;
  EXPECT_THROW(BuildLarkGrammar(context), Exception);
  context = MakeContext();
  context.supports_reasoning = true;
  context.reasoning_start_token_id = -1;
  EXPECT_THROW(BuildLarkGrammar(context), Exception);
}

TEST(BuildLarkGrammarTest, RequiredOutputCanUseQuotedMultitokenToolBoundaries) {
  auto context = MakeContext();
  context.text_output = false;
  context.tool_call_start_token_id.reset();
  context.tool_call_end_token_id.reset();
  context.tool_call_start = "BEGIN\\\"\n";
  context.tool_call_end = "END\t";
  const auto grammar = BuildLarkGrammar(context);
  EXPECT_NE(grammar.find("toolcall: \"BEGIN\\\\\\\"\\n\" functioncall \"END\\t\""), std::string::npos);
}

TEST(BuildLarkGrammarTest, TextOnlyTurnsNeedNoAutomaticMaskAndDisabledOutputIsRejected) {
  ToolCallContext context;
  EXPECT_TRUE(BuildLarkGrammar(context).empty());
  context.text_output = false;
  EXPECT_THROW(BuildLarkGrammar(context), Exception);
}

TEST(EscapeLarkLiteralTest, EscapesQuotesBackslashesAndAllWhitespaceControls) {
  EXPECT_EQ(EscapeLarkLiteral("a\\b\"c\n\r\t\b\f\1"), "\"a\\\\b\\\"c\\n\\r\\t\\b\\f\\u0001\"");
}

TEST(RenderLarkMarkerTest, OnlyNonnegativePublishedIdsOverrideLiteralBytes) {
  EXPECT_EQ(RenderLarkMarker("<tool_call>", 101), "<[101]>");
  EXPECT_EQ(RenderLarkMarker("<tool_call>", std::nullopt), "\"<tool_call>\"");
  EXPECT_EQ(RenderLarkMarker("<tool_call>", -1), "\"<tool_call>\"");
}

TEST(AssistantTextFramerTest, PreservesEveryContentByteAndReasoningSeparatorAcrossAllChunkBoundaries) {
  auto context = MakeContext();
  context.supports_reasoning = true;
  const std::string content = "first\nMESSAGE\n<tool_call>{\"x\":\"[literal\"}</tool_call>\\path";
  const std::string wire = "\nMESSAGE\n" + content;
  for (size_t split = 0; split <= wire.size(); ++split) {
    AssistantTextFramer framer(context);
    auto visible = framer.Push(std::string_view(wire).substr(0, split));
    visible += framer.Push(std::string_view(wire).substr(split));
    EXPECT_EQ(visible, "\n" + content) << split;
    EXPECT_NO_THROW(framer.ValidateBoundary()) << split;
  }
}

TEST(AssistantTextFramerTest, RejectsUnframedTextAndInterruptedOrEmptyFrames) {
  const auto context = MakeContext();
  AssistantTextFramer unframed(context);
  EXPECT_THROW(unframed.Push(R"([{"name":"lookup","parameters":{"city":"Paris"}}])"), Exception);
  AssistantTextFramer partial(context);
  EXPECT_TRUE(partial.Push("MESS").empty());
  EXPECT_THROW(partial.ValidateBoundary(), Exception);
  AssistantTextFramer empty(context);
  EXPECT_TRUE(empty.Push("MESSAGE\n").empty());
  EXPECT_THROW(empty.ValidateBoundary(), Exception);
}

TEST(AssistantTextFramerTest, DirectCallsNeedNoTextFrameAndExplicitGuidanceOwnsItsTextFormat) {
  auto context = MakeContext();
  AssistantTextFramer direct(context);
  EXPECT_NO_THROW(direct.ValidateBoundary());
  EXPECT_NO_THROW(direct.ValidateCompletion(true));
  EXPECT_THROW(direct.ValidateCompletion(false), Exception);
  context.guidance_type = "lark_grammar";
  context.guidance_data = "start: \"READY\"";
  AssistantTextFramer explicit_guidance(context);
  EXPECT_EQ(explicit_guidance.Push("READY"), "READY");
  EXPECT_NO_THROW(explicit_guidance.ValidateBoundary());
  context.guidance_type.clear();
  context.guidance_data.clear();
  context.tool_plan.reset();
  AssistantTextFramer no_tools(context);
  EXPECT_EQ(no_tools.Push("MESSAGE\nis literal data"), "MESSAGE\nis literal data");
}

}  // namespace
