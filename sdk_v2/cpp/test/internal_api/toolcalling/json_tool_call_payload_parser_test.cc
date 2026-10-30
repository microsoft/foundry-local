// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/toolcalling/json_tool_call_payload_parser.h"
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"
#include "util/json_helpers.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace fl {
namespace {

TEST(StrictToolJsonIngressTest, RejectsDuplicateAndEscapedEquivalentMembersBeforeTheyCanBeLost) {
  for (const auto source : {R"({"model":"first","model":"second"})",
                            R"({"tools":[{"parameters":{"properties":{"x":{},"\u0078":false}}}]})"}) {
    EXPECT_THROW(ParseJsonStrict(source), nlohmann::json::parse_error) << source;
  }

  const auto parsed = ParseJsonStrict(R"({"z":{"key":1},"a":[{"key":2},{"key":3}]})");
  EXPECT_EQ(parsed.begin().key(), "z");
  EXPECT_EQ(parsed.at("a").at(1).at("key"), 3);
}

constexpr auto kTools = R"([{"type":"function","function":{"name":"edit","parameters":{
  "type":"object","properties":{"input":{"type":"string","minLength":1}},
  "required":["input"],"additionalProperties":false}}}])";

ToolCallStreamAccumulator MakeAccumulator(bool parallel = true) {
  auto plan = ToolCallPlan::Create(kTools, parallel);
  return {"<tool_call>", "</tool_call>", kTools, "</think>",
          CreateJsonToolCallPayloadParser(std::move(plan), "<tool_call>", "</tool_call>", parallel), true};
}

struct Collected {
  std::string visible;
  std::string error;
  std::vector<ParsedToolCall> calls;
  bool malformed = false;

  void Append(ToolCallStreamAccumulator::Output output) {
    malformed |= output.malformed;
    if (output.malformed) {
      error = std::move(output.error);
    }

    for (auto& event : output.events) {
      if (auto* text = std::get_if<std::string>(&event)) {
        visible += *text;
      } else {
        calls.push_back(std::move(std::get<ParsedToolCall>(event)));
      }
    }
  }
};

std::string Frame(const nlohmann::json& calls) {
  return "<tool_call>" + calls.dump() + "</tool_call>";
}

TEST(JsonToolCallPayloadParserTest, FoundryWrapperSeparatesAutomaticExamplesFromToolOnlyFailures) {
  const auto plan = ToolCallPlan::Create(kTools);
  const std::string invalid =
      "```json\n{\"foundry_tool_calls\":[{\"name\":\"edit\",\"parameters\":{\"input\":\"\"}}]}\n```";
  const std::string valid =
      "```json\n{\"foundry_tool_calls\":[{\"name\":\"edit\",\"parameters\":{\"input\":\"ok\"}}]}\n```";

  const auto automatic = CreateJsonToolCallPayloadParser(
      plan, "```json", "```", true, true, true);
  const auto rejected = automatic(invalid, true);
  EXPECT_EQ(rejected.disposition, ToolCallPayloadDisposition::kRejected);
  EXPECT_EQ(rejected.consumed_size, invalid.size());
  const auto parsed = automatic(valid, true);
  EXPECT_EQ(parsed.disposition, ToolCallPayloadDisposition::kParsed);
  ASSERT_EQ(parsed.calls.size(), 1u);

  const auto tool_only = CreateJsonToolCallPayloadParser(
      plan, "```json", "```", true, true, false);
  EXPECT_EQ(tool_only(invalid, true).disposition,
            ToolCallPayloadDisposition::kMalformed);
}

TEST(JsonToolCallPayloadParserTest, EveryByteSplitPreservesCodeUnicodeAndLiteralMarkers) {
  const std::string input =
      "[scratch\nprint(\"quoted\\\\path\")\n"
      "<tool_call></tool_call><think></think>\n\xc3\xa9\xf0\x9f\x98\x80";
  const auto source = Frame(nlohmann::json::array({{{"name", "edit"}, {"parameters", {{"input", input}}}}}));
  for (size_t split = 0; split <= source.size(); ++split) {
    auto accumulator = MakeAccumulator();
    Collected result;
    result.Append(accumulator.Push(source.substr(0, split)));
    result.Append(accumulator.Push(source.substr(split)));
    result.Append(accumulator.Flush());

    ASSERT_FALSE(result.malformed) << split << ": " << result.error;
    EXPECT_TRUE(result.visible.empty()) << split;
    ASSERT_EQ(result.calls.size(), 1u) << split;
    EXPECT_EQ(nlohmann::json::parse(result.calls.front().arguments).at("input"), input) << split;
  }
}

TEST(JsonToolCallPayloadParserTest, OnlyTheAuthoritativeOpeningTokenStartsAToolEnvelope) {
  ToolCallContext context;
  context.tool_plan = ToolCallPlan::Create(kTools);
  context.tool_call_start_token_id = 101;
  context.tool_call_end_token_id = 102;
  auto accumulator = MakeAccumulator();
  Collected result;
  auto push = [&](std::string text, int32_t token_id) {
    result.Append(chat_session_internal::PushToolOutputSegment(
        {std::move(text), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT, token_id}, context, nullptr, accumulator));
  };

  push("Literal <tool_call> is ordinary text.", 1);
  EXPECT_FALSE(accumulator.InsideToolCall());
  push("<tool_call>", 101);
  EXPECT_TRUE(accumulator.InsideToolCall());
  push(R"([{"name":"edit","parameters":{"input":"</think><tool_call>"}}])", 2);
  push("</tool_call>", 102);
  result.Append(accumulator.Flush());

  ASSERT_FALSE(result.malformed) << result.error;
  EXPECT_EQ(result.visible, "Literal <tool_call> is ordinary text.");
  ASSERT_EQ(result.calls.size(), 1u);
  EXPECT_EQ(nlohmann::json::parse(result.calls.front().arguments).at("input"), "</think><tool_call>");
}

TEST(JsonToolCallPayloadParserTest, ByteAtATimeDoesNotPublishUntilBatchIsKnown) {
  const auto call = Frame(nlohmann::json::array({{{"name", "edit"}, {"parameters", {{"input", "one"}}}}}));
  auto accumulator = MakeAccumulator();
  Collected result;
  for (const auto byte : call + "\n" + call) {
    result.Append(accumulator.Push(std::string(1, byte)));
    EXPECT_TRUE(result.calls.empty());
  }

  result.Append(accumulator.Flush());
  ASSERT_FALSE(result.malformed) << result.error;
  EXPECT_TRUE(result.visible.empty());
  EXPECT_EQ(result.calls.size(), 2u);
}

TEST(JsonToolCallPayloadParserTest, InvalidSiblingRejectsWholeAdjacentBatchWithoutLeakingArguments) {
  const auto good = Frame(nlohmann::json::array({{{"name", "edit"}, {"parameters", {{"input", "good"}}}}}));
  const auto bad = Frame(nlohmann::json::array({{{"name", "edit"}, {"parameters", {{"input", ""}}}}}));
  const auto source = "visible prefix" + good + "\n" + bad + "untrusted tail";
  for (size_t split = 0; split <= source.size(); ++split) {
    auto accumulator = MakeAccumulator();
    Collected result;
    result.Append(accumulator.Push(source.substr(0, split)));
    result.Append(accumulator.Push(source.substr(split)));
    result.Append(accumulator.Flush());

    EXPECT_TRUE(result.malformed) << split;
    EXPECT_FALSE(result.error.empty()) << split;
    EXPECT_EQ(result.visible, "visible prefix") << split;
    EXPECT_TRUE(result.calls.empty()) << split;
  }
}

TEST(JsonToolCallPayloadParserTest, InterruptedOrNonJsonCallsNeverBecomeVisibleText) {
  for (const auto source : {"<tool_call>[{\"name\":\"edit\"",
                            "<tool_call><function=edit>not JSON</function></tool_call>",
                            "<tool_call>[{\"name\":\"edit\",\"parameters\":{\"input\":\"secret\"}}]"}) {
    auto accumulator = MakeAccumulator();
    Collected result;
    result.Append(accumulator.Push(source));
    result.Append(accumulator.RejectPendingSelectedPayload());

    EXPECT_TRUE(result.malformed) << source;
    EXPECT_TRUE(result.visible.empty()) << source;
    EXPECT_TRUE(result.calls.empty()) << source;
  }
}

TEST(JsonToolCallPayloadParserTest, SerialChoiceRejectsAdjacentCallsAtomically) {
  auto accumulator = MakeAccumulator(false);
  const auto call = Frame(nlohmann::json::array({{{"name", "edit"}, {"parameters", {{"input", "one"}}}}}));
  Collected result;
  result.Append(accumulator.Push(call + call));
  result.Append(accumulator.Flush());
  EXPECT_TRUE(result.malformed);
  EXPECT_TRUE(result.calls.empty());
  EXPECT_TRUE(result.visible.empty());
}

TEST(JsonToolCallPayloadParserTest, LargeEditsWorkAndOversizedBatchesFailExplicitly) {
  for (const auto bytes : {size_t{128 * 1024}, ToolCallPlan::kMaxBatchBytes}) {
    auto accumulator = MakeAccumulator();
    const auto source = Frame(
        nlohmann::json::array({{{"name", "edit"}, {"parameters", {{"input", std::string(bytes, 'x')}}}}}));
    Collected result;
    result.Append(accumulator.Push(source));
    result.Append(accumulator.Flush());
    EXPECT_TRUE(result.visible.empty());
    if (bytes < ToolCallPlan::kMaxBatchBytes) {
      ASSERT_FALSE(result.malformed) << result.error;
      ASSERT_EQ(result.calls.size(), 1u);
      EXPECT_EQ(nlohmann::json::parse(result.calls.front().arguments).at("input").get<std::string>().size(), bytes);
    } else {
      EXPECT_TRUE(result.malformed);
      EXPECT_FALSE(result.error.empty());
      EXPECT_TRUE(result.calls.empty());
    }
  }
}

TEST(JsonToolCallPayloadParserTest, ReasoningMarkersInsideToolDataAreNotControlTokens) {
  ReasoningStreamSplitter splitter("<think>", "</think>");
  std::vector<ReasoningStreamSplitter::Segment> segments;
  const auto collect = [&](const auto& output) {
    segments.insert(segments.end(), output.begin(), output.end());
  };
  const std::string data = R"({"input":"<think>literal code</think>"})";
  EXPECT_FALSE(chat_session_internal::PushDecodedFragment(
      data, std::nullopt, nullptr, splitter, collect, /*inside_tool_payload=*/true));
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments.front().text, data);
  EXPECT_EQ(segments.front().type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_FALSE(splitter.InsideReasoning());
  EXPECT_EQ(splitter.ReasoningTokenCount(), 0);
}

}  // namespace
}  // namespace fl
