// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for tool call parsing utilities in toolcalling/tool_call_utils.h.
//
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "items/tool_call_item.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace fl;

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
  auto calls = ParseToolCalls(text, "<tc>", "</tc>");

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "fn");
  EXPECT_NE(calls[0].arguments.find("b"), std::string::npos);
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
