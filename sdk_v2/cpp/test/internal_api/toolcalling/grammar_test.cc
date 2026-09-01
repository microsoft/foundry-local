// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for grammar construction in toolcalling/grammar.h.
//
#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

using namespace fl;

// ========================================================================
// BuildToolJsonSchema tests
// ========================================================================

TEST(BuildToolJsonSchemaTest, NoToolsReturnsEmptyObject) {
  ToolCallContext ctx;
  ctx.tool_output = true;
  // tools_json empty
  EXPECT_EQ(BuildToolJsonSchema(ctx), "{}");
}

TEST(BuildToolJsonSchemaTest, ToolOutputFalseReturnsEmptyObject) {
  ToolCallContext ctx;
  ctx.tool_output = false;
  ctx.tools_json = R"([{"type":"function","function":{"name":"fn","parameters":{"type":"object"}}}])";
  EXPECT_EQ(BuildToolJsonSchema(ctx), "{}");
}

TEST(BuildToolJsonSchemaTest, InvalidJsonReturnsEmptyObject) {
  ToolCallContext ctx;
  ctx.tool_output = true;
  ctx.tools_json = "not json";
  EXPECT_EQ(BuildToolJsonSchema(ctx), "{}");
}

TEST(BuildToolJsonSchemaTest, SingleToolProducesSchema) {
  ToolCallContext ctx;
  ctx.tool_output = true;
  ctx.tools_json = R"([{
    "type": "function",
    "function": {
      "name": "get_weather",
      "description": "Get weather info",
      "parameters": {
        "type": "object",
        "properties": { "city": { "type": "string" } },
        "required": ["city"]
      }
    }
  }])";

  std::string schema_str = BuildToolJsonSchema(ctx);
  ASSERT_NE(schema_str, "{}");

  auto schema = nlohmann::json::parse(schema_str);
  EXPECT_EQ(schema["type"], "array");
  EXPECT_TRUE(schema.contains("items"));
  EXPECT_TRUE(schema["items"].contains("anyOf"));

  auto& any_of = schema["items"]["anyOf"];
  ASSERT_EQ(any_of.size(), 1u);
  EXPECT_EQ(any_of[0]["properties"]["name"]["const"], "get_weather");
}

TEST(BuildToolJsonSchemaTest, MultipleToolsProducesAnyOf) {
  ToolCallContext ctx;
  ctx.tool_output = true;
  ctx.tools_json = R"([
    {"type":"function","function":{"name":"fn_a","parameters":{"type":"object"}}},
    {"type":"function","function":{"name":"fn_b","parameters":{"type":"object"}}}
  ])";

  std::string schema_str = BuildToolJsonSchema(ctx);
  auto schema = nlohmann::json::parse(schema_str);

  auto& any_of = schema["items"]["anyOf"];
  ASSERT_EQ(any_of.size(), 2u);
  EXPECT_EQ(any_of[0]["properties"]["name"]["const"], "fn_a");
  EXPECT_EQ(any_of[1]["properties"]["name"]["const"], "fn_b");
}

TEST(BuildToolJsonSchemaTest, DirectNameStyleWorks) {
  ToolCallContext ctx;
  ctx.tool_output = true;
  ctx.tools_json = R"([{"name":"search","parameters":{"type":"object","properties":{"q":{"type":"string"}}}}])";

  std::string schema_str = BuildToolJsonSchema(ctx);
  auto schema = nlohmann::json::parse(schema_str);

  auto& any_of = schema["items"]["anyOf"];
  ASSERT_EQ(any_of.size(), 1u);
  EXPECT_EQ(any_of[0]["properties"]["name"]["const"], "search");
}

// ========================================================================
// BuildLarkGrammar tests
// ========================================================================

TEST(BuildLarkGrammarTest, TextOnlyGrammar) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = false;

  std::string grammar = BuildLarkGrammar(ctx, "{}");
  EXPECT_NE(grammar.find("start: TEXT"), std::string::npos);
  EXPECT_EQ(grammar.find("functioncall"), std::string::npos);
  EXPECT_EQ(grammar.find("toolcall"), std::string::npos);
}

TEST(BuildLarkGrammarTest, ToolOnlyWithMarkersGrammar) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  ctx.tool_call_start = "<tool>";
  ctx.tool_call_end = "</tool>";

  std::string grammar = BuildLarkGrammar(ctx, R"({"type":"array"})");
  EXPECT_NE(grammar.find("start: toolcall"), std::string::npos);
  EXPECT_NE(grammar.find("functioncall"), std::string::npos);
  EXPECT_NE(grammar.find("<tool>"), std::string::npos);
  EXPECT_NE(grammar.find("</tool>"), std::string::npos);
}

TEST(BuildLarkGrammarTest, ToolOnlyWithoutMarkersGrammar) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  // No tool_call_start/end

  std::string grammar = BuildLarkGrammar(ctx, R"({"type":"array"})");
  EXPECT_NE(grammar.find("start: functioncall"), std::string::npos);
  EXPECT_EQ(grammar.find("toolcall"), std::string::npos);
}

TEST(BuildLarkGrammarTest, BothTextAndToolWithMarkersGrammar) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = true;
  ctx.tool_call_start = "<tc>";
  ctx.tool_call_end = "</tc>";

  std::string grammar = BuildLarkGrammar(ctx, R"({"type":"array"})");
  EXPECT_NE(grammar.find("start: TEXT | toolcall"), std::string::npos);
  EXPECT_NE(grammar.find("TEXT"), std::string::npos);
  EXPECT_NE(grammar.find("functioncall"), std::string::npos);
}

TEST(BuildLarkGrammarTest, BothTextAndToolWithoutMarkersGrammar) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = true;

  std::string grammar = BuildLarkGrammar(ctx, R"({"type":"array"})");
  EXPECT_NE(grammar.find("start: TEXT | functioncall"), std::string::npos);
}

TEST(BuildLarkGrammarTest, NeitherTextNorToolThrows) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = false;

  EXPECT_THROW(BuildLarkGrammar(ctx, "{}"), std::exception);
}

TEST(BuildLarkGrammarTest, GrammarContainsJsonDirective) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;

  std::string schema = R"({"type":"array","items":{"anyOf":[]}})";
  std::string grammar = BuildLarkGrammar(ctx, schema);
  EXPECT_NE(grammar.find("%json"), std::string::npos);
  EXPECT_NE(grammar.find(schema), std::string::npos);
}

// ========================================================================
// ToolCallContext tests
// ========================================================================

TEST(ToolCallContextTest, DefaultsAreCorrect) {
  ToolCallContext ctx;
  EXPECT_TRUE(ctx.text_output);
  EXPECT_FALSE(ctx.tool_output);
  EXPECT_TRUE(ctx.tool_call_start.empty());
  EXPECT_TRUE(ctx.tool_call_end.empty());
  EXPECT_TRUE(ctx.tools_json.empty());
}

TEST(ToolCallContextTest, HasToolsWhenJsonPresent) {
  ToolCallContext ctx;
  ctx.tools_json = "[{}]";
  EXPECT_TRUE(ctx.HasTools());
}

TEST(ToolCallContextTest, NoToolsWhenJsonEmpty) {
  ToolCallContext ctx;
  EXPECT_FALSE(ctx.HasTools());
}

TEST(ToolCallContextTest, HasToolCallTokensWhenBothPresent) {
  ToolCallContext ctx;
  ctx.tool_call_start = "<tc>";
  ctx.tool_call_end = "</tc>";
  EXPECT_TRUE(ctx.HasToolCallTokens());
}

TEST(ToolCallContextTest, NoToolCallTokensWhenStartMissing) {
  ToolCallContext ctx;
  ctx.tool_call_end = "</tc>";
  EXPECT_FALSE(ctx.HasToolCallTokens());
}

TEST(ToolCallContextTest, NoToolCallTokensWhenEndMissing) {
  ToolCallContext ctx;
  ctx.tool_call_start = "<tc>";
  EXPECT_FALSE(ctx.HasToolCallTokens());
}

// ========================================================================
// Chain-of-thought BuildLarkGrammar tests
// (Ported from C# GrammarTests cases 6-15)
// ========================================================================

TEST(BuildLarkGrammarTest, CoT_TextOnly_KnownThinkIds) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = false;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";

  std::string grammar = BuildLarkGrammar(ctx, "{}");

  std::string expected = R"(start: cot TEXT
cot: <think> THINK_TEXT </think> "\n"
THINK_TEXT: /[^<]+/
TEXT: /[^{<](.|\n)*/
)";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, CoT_TextOnly_UnknownThinkIds) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = false;
  ctx.supports_reasoning = true;
  // Leave reasoning_start/end empty to trigger literal string fallback

  std::string grammar = BuildLarkGrammar(ctx, "{}");

  std::string expected = R"(start: cot TEXT
cot: "<think>" THINK_TEXT "</think>" "\n"
THINK_TEXT: /[^<]+/
TEXT: /[^{<](.|\n)*/
)";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, CoT_ToolOnly_KnownThinkIds_KnownToolIds) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot toolcall
cot: <think> THINK_TEXT </think> "\n"
THINK_TEXT: /[^<]+/
toolcall: <tool_call> functioncall </tool_call>
functioncall: %json )" +
      json_schema + "\n";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, CoT_ToolOnly_UnknownThinkIds_KnownToolIds) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot toolcall
cot: "<think>" THINK_TEXT "</think>" "\n"
THINK_TEXT: /[^<]+/
toolcall: <tool_call> functioncall </tool_call>
functioncall: %json )" +
      json_schema + "\n";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, CoT_ToolOnly_KnownThinkIds_UnknownToolIds) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot functioncall
cot: <think> THINK_TEXT </think> "\n"
THINK_TEXT: /[^<]+/
functioncall: %json )" +
      json_schema + "\n";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, CoT_ToolOnly_UnknownThinkIds_UnknownToolIds) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot functioncall
cot: "<think>" THINK_TEXT "</think>" "\n"
THINK_TEXT: /[^<]+/
functioncall: %json )" +
      json_schema + "\n";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, CoT_TextOrTool_KnownThinkIds_KnownToolIds) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot output
cot: <think> THINK_TEXT </think> "\n"
THINK_TEXT: /[^<]+/
output: TEXT | toolcall
TEXT: /[^{<][^<]*/
toolcall: <tool_call> functioncall </tool_call>
functioncall: %json )" +
      json_schema + "\n";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, CoT_TextOrTool_UnknownThinkIds_KnownToolIds) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot output
cot: "<think>" THINK_TEXT "</think>" "\n"
THINK_TEXT: /[^<]+/
output: TEXT | toolcall
TEXT: /[^{<][^<]*/
toolcall: <tool_call> functioncall </tool_call>
functioncall: %json )" +
      json_schema + "\n";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, CoT_TextOrTool_KnownThinkIds_UnknownToolIds) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot output
cot: <think> THINK_TEXT </think> "\n"
THINK_TEXT: /[^<]+/
output: TEXT | functioncall
TEXT: /[^{<][^{]*/
functioncall: %json )" +
      json_schema + "\n";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, CoT_TextOrTool_UnknownThinkIds_UnknownToolIds) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot output
cot: "<think>" THINK_TEXT "</think>" "\n"
THINK_TEXT: /[^<]+/
output: TEXT | functioncall
TEXT: /[^{<][^{]*/
functioncall: %json )" +
      json_schema + "\n";

  EXPECT_EQ(grammar, expected);
}

// ========================================================================
// ToolCallContext reasoning field tests
// ========================================================================

TEST(ToolCallContextTest, HasReasoningTokensWhenBothPresent) {
  ToolCallContext ctx;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";
  EXPECT_TRUE(ctx.HasReasoningTokens());
}

TEST(ToolCallContextTest, NoReasoningTokensWhenStartMissing) {
  ToolCallContext ctx;
  ctx.reasoning_end = "</think>";
  EXPECT_FALSE(ctx.HasReasoningTokens());
}

TEST(ToolCallContextTest, NoReasoningTokensWhenEndMissing) {
  ToolCallContext ctx;
  ctx.reasoning_start = "<think>";
  EXPECT_FALSE(ctx.HasReasoningTokens());
}

TEST(ToolCallContextTest, SupportsReasoningDefaultsFalse) {
  ToolCallContext ctx;
  EXPECT_FALSE(ctx.supports_reasoning);
}
