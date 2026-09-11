// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for tool call parsing utilities in toolcalling/tool_call_utils.h.
//
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/session/tool_registry.h"
#include "items/tool_call_item.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace fl;

namespace {

std::string AdvertisedTool(const std::string& name) {
  return nlohmann::json::array({{
      {"type", "function"},
      {"name", name},
      {"parameters", {{"type", "object"}}},
  }}).dump();
}

}  // namespace

// ========================================================================
// GenerateToolCallId tests
// ========================================================================

TEST(GenerateToolCallIdTest, StartsWithCallPrefix) {
  std::string id = GenerateToolCallId();
  EXPECT_EQ(id.substr(0, 5), "call_");
}

TEST(GenerateToolCallIdTest, HasCorrectLength) {
  std::string id = GenerateToolCallId();
  // "call_" (5) + 9 random characters = 14
  EXPECT_EQ(id.size(), 14u);
}

TEST(GenerateToolCallIdTest, UniqueAcrossCalls) {
  std::string id1 = GenerateToolCallId();
  std::string id2 = GenerateToolCallId();
  EXPECT_NE(id1, id2);
}

// ========================================================================
// ParseToolCalls tests
// ========================================================================

TEST(ParseToolCallsTest, EmptyTextReturnsEmpty) {
  auto calls = ParseToolCalls("", "<tool_call>", "</tool_call>");
  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, EmptyMarkersReturnsEmpty) {
  auto calls = ParseToolCalls("some text <tool_call>{}</tool_call>", "", "");
  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, NoMarkersInTextReturnsEmpty) {
  auto calls = ParseToolCalls("just regular text", "<tool_call>", "</tool_call>");
  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, SingleToolCallArray) {
  std::string text =
      R"(Some text <tool_call>[{"name":"get_weather","arguments":{"city":"Seattle"}}]</tool_call>)";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>");

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "get_weather");
  EXPECT_FALSE(calls[0].id.empty());
  EXPECT_EQ(calls[0].id.substr(0, 5), "call_");

  // Arguments should be the JSON string representation
  EXPECT_NE(calls[0].arguments.find("Seattle"), std::string::npos);
}

TEST(ParseToolCallsTest, SingleToolCallObject) {
  std::string text =
      R"(<tool_call>{"name":"search","arguments":{"query":"hello"}}</tool_call>)";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>");

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "search");
  EXPECT_NE(calls[0].arguments.find("hello"), std::string::npos);
}

TEST(ParseToolCallsTest, MultipleToolCallBlocks) {
  std::string text =
      R"(<tc>{"name":"fn1","arguments":{}}</tc> text <tc>{"name":"fn2","arguments":{}}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].name, "fn1");
  EXPECT_EQ(calls[1].name, "fn2");
}

TEST(ParseToolCallsTest, ArrayWithMultipleToolCalls) {
  std::string text =
      R"(<tool_call>[{"name":"fn1","arguments":{}},{"name":"fn2","arguments":{"x":1}}]</tool_call>)";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>");

  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].name, "fn1");
  EXPECT_EQ(calls[1].name, "fn2");
}

TEST(ParseToolCallsTest, ParametersKeyWorksAsAlternative) {
  std::string text =
      R"(<tc>{"name":"fn","parameters":{"a":"b"}}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>", AdvertisedTool("fn"));

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "fn");
  EXPECT_NE(calls[0].arguments.find("b"), std::string::npos);
}

TEST(ParseToolCallsTest, SingleKeyToolCall) {
  std::string text =
      R"(<tc>{"exec_command":{"cmd":"grep -n test file.py"}}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>", AdvertisedTool("exec_command"));

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "exec_command");
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"grep -n test file.py"})");
}

TEST(ParseToolCallsTest, SingleKeyToolCallWithMissingArgumentsBrace) {
  std::string text =
      R"(<tc>{"update_plan":"explanation":"Done","plan":[{"step":"verify","status":"completed"}]}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>", AdvertisedTool("update_plan"));

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "update_plan");
  EXPECT_EQ(calls[0].arguments,
            R"({"explanation":"Done","plan":[{"status":"completed","step":"verify"}]})");
}

TEST(ParseToolCallsTest, NestedRecoveryIsLeftToOrderedStreamParser) {
  std::string text = R"(<tool_call><exec_command","arguments":{"cmd":"ls"})"
                     R"(<tool_call>{"name":"exec_command","args":{"cmd":"pwd"}}</tool_call>)";
  auto calls = ParseToolCalls(
      text, "<tool_call>", "</tool_call>", AdvertisedTool("exec_command"));

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, MarkerInsideArgumentDoesNotTriggerNestedRecovery) {
  std::string text =
      R"(<tool_call>{"name":"shell","arguments":{"cmd":"grep '<tool_call>' output.txt"}}</tool_call>)";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>");

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "shell");
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"grep '<tool_call>' output.txt"})");
}

TEST(ParseToolCallsTest, EndMarkerInsideArgumentDoesNotTruncateCall) {
  std::string text =
      R"(<tool_call>{"name":"shell","arguments":{"cmd":"grep '</tool_call>' output.txt"}}</tool_call>)";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>");

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "shell");
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"grep '</tool_call>' output.txt"})");
}

TEST(ParseToolCallsTest, RecoversMissingNameObjectPrefix) {
  std::string text =
      R"(<tool_call><exec_command","arguments":{"cmd":"ls /testbed","workdir":"/testbed"}}</tool_call>)";
  auto calls = ParseToolCalls(
      text, "<tool_call>", "</tool_call>", AdvertisedTool("exec_command"));

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "exec_command");
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"ls /testbed","workdir":"/testbed"})");
}

TEST(ParseToolCallsTest, RecoversMultilineMissingNamePrefixWithDirectArguments) {
  std::string text = R"(<tool_call>
<exec_command","cmd":"ls /testbed && git -C /testbed log --oneline -3"}
</tool_call>)";
  auto calls = ParseToolCalls(
      text, "<tool_call>", "</tool_call>", AdvertisedTool("exec_command"));

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "exec_command");
  EXPECT_EQ(calls[0].arguments,
            R"({"cmd":"ls /testbed && git -C /testbed log --oneline -3"})");
}

TEST(ParseToolCallsTest, RecoversCommaAfterToolName) {
  std::string text =
      R"(<tool_call>{"exec_command","cmd":"ls /testbed","workdir":"/testbed"}</tool_call>)";
  auto calls = ParseToolCalls(
      text, "<tool_call>", "</tool_call>", AdvertisedTool("exec_command"));

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "exec_command");
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"ls /testbed","workdir":"/testbed"})");
}

TEST(ParseToolCallsTest, DoesNotAliasCanonicalExecCommandToShell) {
  std::string text =
      R"(<tool_call>{"name":"exec_command","arguments":{"cmd":"git diff"}}</tool_call>)";
  std::string tools =
    R"([{"type":"function","name":"shell","parameters":{"type":"object"}}])";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>", tools);

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "exec_command");
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"git diff"})");
}

TEST(ParseToolCallsTest, RepairedNameMustMatchAdvertisedTool) {
  std::string text =
      R"(<tool_call><"exec_command","arguments":{"cmd":"git diff"}</tool_call>)";
  std::string tools =
    R"([{"type":"function","name":"shell","parameters":{"type":"object"}}])";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>", tools);

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, RejectsRepairWhenToolMetadataIsEmpty) {
  std::string text =
      R"(<tool_call><"exec_command","arguments":{"cmd":"git diff"}</tool_call>)";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>", "[]");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, PreservesAdvertisedExecCommand) {
  std::string text =
      R"(<tool_call>{"name":"exec_command","arguments":{"cmd":"git diff"}}</tool_call>)";
  std::string tools =
      R"([{"type":"function","function":{"name":"exec_command","parameters":{"type":"object"}}}])";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>", tools);

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "exec_command");
}

TEST(ParseToolCallsTest, CleansFunctionPrefixOnlyForAdvertisedExactName) {
  std::string tools = AdvertisedTool("exec_command");
  const std::vector<std::string> texts = {
      R"(<tool_call>{"name":"function=\"exec_command","arguments":{"cmd":"pwd"}}</tool_call>)",
      R"(<tool_call>{"name":"function=exec_command","arguments":{"cmd":"pwd"}}</tool_call>)"};

  for (const auto& text : texts) {
    auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>", tools);

    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].name, "exec_command");
    EXPECT_EQ(calls[0].arguments, R"({"cmd":"pwd"})");
  }
}

TEST(ParseToolCallsTest, DoesNotAliasSingletonCmdToShell) {
  std::string text = R"(<tool_call>{"cmd":"pwd"}</tool_call>)";
  std::string tools =
      R"([{"type":"function","name":"shell","parameters":{"type":"object"}}])";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>", tools);

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, DoesNotAliasCanonicalCommandArgumentOrToolName) {
  std::string text =
      R"(<tool_call>{"name":"exec_command","arguments":{"command":"pwd"}}</tool_call>)";
  std::string tools =
      R"([{"type":"function","name":"shell","parameters":{"type":"object","properties":{"cmd":{"type":"string"}}}}])";
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>", tools);

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "exec_command");
  EXPECT_EQ(calls[0].arguments, R"({"command":"pwd"})");
}

TEST(ParseToolCallsTest, RecoversFunctionKeyAndMissingOuterBrace) {
  std::string text =
      R"(<tool_call>{"function":"exec_command","arguments":{"cmd":"pwd"}</tool_call>)";
  std::string tools = AdvertisedTool("exec_command");
  auto calls = ParseToolCalls(text, "<tool_call>", "</tool_call>", tools);

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "exec_command");
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"pwd"})");
}

TEST(ParseToolCallsTest, InvalidJsonReturnsEmpty) {
  std::string text = R"(<tc>not valid json</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, MissingEndMarkerReturnsEmpty) {
  std::string text = R"(<tc>{"name":"fn","arguments":{}})";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, StringArguments) {
  std::string text =
      R"(<tc>{"name":"fn","arguments":"{\"key\": \"value\"}"}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "fn");
  // String arguments are kept as-is
  EXPECT_NE(calls[0].arguments.find("key"), std::string::npos);
  EXPECT_EQ(calls[0].argument_source, R"("{\"key\": \"value\"}")");
  ASSERT_TRUE(calls[0].parsed_arguments.has_value());
  EXPECT_TRUE(calls[0].parsed_arguments->is_string());
}

TEST(ParseToolCallsTest, PreservesExactArgumentValueSourceBytes) {
  const std::string arguments =
      R"({ "z" : [1, {"escaped":"a\\\"b"}], "z":2, "input" : "\u0061" })";
  const auto calls =
      ParseToolCalls("<tc>{\"name\":\"fn\",\"arguments\":" + arguments + "}</tc>", "<tc>", "</tc>");

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].argument_source, arguments);
  ASSERT_TRUE(calls[0].parsed_arguments.has_value());
  EXPECT_EQ(calls[0].parsed_arguments->at("input"), "a");
  EXPECT_NE(calls[0].arguments, arguments);
}

TEST(ParseToolCallsTest, ParametersPreserveExactSourceBytes) {
  const auto calls =
      ParseToolCalls(R"(<tc>{"name":"fn","parameters": { "b":2, "a":1 } }</tc>)", "<tc>", "</tc>");

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].argument_source, R"({ "b":2, "a":1 })");
  EXPECT_EQ(calls[0].arguments, R"({"a":1,"b":2})");
}

TEST(ParseToolCallsTest, CustomLoneInputUnwrapsDecodedTextEndToEnd) {
  const auto calls = ParseToolCalls(
      R"(<tc>{"name":"run","arguments": { "input" : "line\u000a\u00e9" } }</tc>)", "<tc>", "</tc>");

  ASSERT_EQ(calls.size(), 1u);
  const auto payload = ExtractCustomToolInput(calls[0].argument_source);
  auto generated = MakeGeneratedToolCall(calls[0].id, calls[0].name, payload, ToolKind::kCustom);
  EXPECT_EQ(generated.call.arguments, "line\né");
  EXPECT_EQ(generated.call.normalized_arguments,
            nlohmann::ordered_json({{kCustomToolInputParameter, "line\né"}}));
}

TEST(ParseToolCallsTest, CustomNonWrapperKeepsExactSourceEndToEnd) {
  const std::string arguments = R"({ "input":"first", "input":"second", "z" : 1 })";
  const auto calls =
      ParseToolCalls("<tc>{\"name\":\"run\",\"arguments\":" + arguments + "}</tc>", "<tc>", "</tc>");

  ASSERT_EQ(calls.size(), 1u);
  const auto payload = ExtractCustomToolInput(calls[0].argument_source);
  auto generated = MakeGeneratedToolCall(calls[0].id, calls[0].name, payload, ToolKind::kCustom);
  EXPECT_EQ(generated.call.arguments, arguments);
  EXPECT_EQ(generated.call.normalized_arguments,
            nlohmann::ordered_json({{kCustomToolInputParameter, arguments}}));
}

// ========================================================================
// Atomic validation: malformed shape/type in any item rejects the whole block.
// ========================================================================

TEST(ParseToolCallsTest, NumericNameReturnsEmpty) {
  std::string text = R"(<tc>{"name":123,"arguments":{}}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, NullNameReturnsEmpty) {
  std::string text = R"(<tc>{"name":null,"arguments":{}}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, ObjectNameReturnsEmpty) {
  std::string text = R"(<tc>{"name":{"first":"fn"},"arguments":{}}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, EmptyStringNameReturnsEmpty) {
  std::string text = R"(<tc>{"name":"","arguments":{}}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, NonObjectArrayElementReturnsEmpty) {
  std::string text = R"(<tc>[{"name":"fn1"},123]</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, MixedValidAndInvalidArrayReturnsEmpty) {
  std::string text = R"(<tc>[{"name":"fn1","arguments":{}},{"name":123}]</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, MixedValidAndMissingNameArrayReturnsEmpty) {
  std::string text = R"(<tc>[{"name":"fn1"},{"arguments":{"x":1}}]</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  EXPECT_TRUE(calls.empty());
}

TEST(ParseToolCallsTest, ValidSingleObjectStillAccepted) {
  std::string text = R"(<tc>{"name":"fn","arguments":{"x":1}}</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "fn");
  EXPECT_FALSE(calls[0].id.empty());
}

TEST(ParseToolCallsTest, ValidArrayOfMultipleObjectsStillAccepted) {
  std::string text =
      R"(<tc>[{"name":"fn1","arguments":{}},{"name":"fn2","arguments":{"x":1}}]</tc>)";
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].name, "fn1");
  EXPECT_EQ(calls[1].name, "fn2");
}

// ========================================================================
// ToolCallsToItems tests
// ========================================================================

TEST(ToolCallsToItemsTest, EmptyInputReturnsEmpty) {
  auto items = ToolCallsToItems({});
  EXPECT_TRUE(items.empty());
}

TEST(ToolCallsToItemsTest, ConvertsToCorrectItemType) {
  std::vector<ParsedToolCall> calls = {
      {"call_abc", "get_weather", R"({"city":"Seattle"})"},
  };

  auto items = ToolCallsToItems(calls);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  const ToolCallItem& tool_call_item = static_cast<const ToolCallItem&>(*items[0]);
  EXPECT_EQ(tool_call_item.call_id, "call_abc");
  EXPECT_EQ(tool_call_item.name, "get_weather");
  EXPECT_EQ(tool_call_item.arguments, R"({"city":"Seattle"})");
}

TEST(ToolCallsToItemsTest, MultipleCalls) {
  std::vector<ParsedToolCall> calls = {
      {"call_1", "fn_a", "{}"},
      {"call_2", "fn_b", R"({"x":1})"},
  };

  auto items = ToolCallsToItems(calls);
  ASSERT_EQ(items.size(), 2u);
  const ToolCallItem& item0 = static_cast<const ToolCallItem&>(*items[0]);
  const ToolCallItem& item1 = static_cast<const ToolCallItem&>(*items[1]);
  EXPECT_EQ(item0.name, "fn_a");
  EXPECT_EQ(item1.name, "fn_b");
}
