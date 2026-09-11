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
  ctx.tool_call_start_token_id = 200;
  ctx.tool_call_end_token_id = 201;

  std::string grammar = BuildLarkGrammar(ctx, R"({"type":"array"})");
  EXPECT_NE(grammar.find("start: toolcall"), std::string::npos);
  EXPECT_NE(grammar.find("functioncall"), std::string::npos);
  EXPECT_NE(grammar.find("<[200]>"), std::string::npos);
  EXPECT_NE(grammar.find("<[201]>"), std::string::npos);
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
// RenderLarkMarker / EscapeLarkLiteral — exact-token-vs-literal rendering
// ========================================================================

TEST(EscapeLarkLiteralTest, WrapsPlainTextInQuotes) {
  EXPECT_EQ(EscapeLarkLiteral("<tool_call>"), "\"<tool_call>\"");
}

TEST(EscapeLarkLiteralTest, EscapesBackslashAndQuote) {
  EXPECT_EQ(EscapeLarkLiteral("a\\b\"c"), "\"a\\\\b\\\"c\"");
}

TEST(EscapeLarkLiteralTest, EscapesNewlineCarriageReturnAndTab) {
  EXPECT_EQ(EscapeLarkLiteral("a\nb\rc\td"), "\"a\\nb\\rc\\td\"");
}

TEST(EscapeLarkLiteralTest, EscapesRemainingControlCharacters) {
  EXPECT_EQ(EscapeLarkLiteral(std::string("a\b\f") + static_cast<char>(0x01) + "b"),
            "\"a\\b\\f\\u0001b\"");
}

TEST(EscapeLarkLiteralTest, EmptyTextProducesEmptyLiteral) {
  EXPECT_EQ(EscapeLarkLiteral(""), "\"\"");
}

TEST(RenderLarkMarkerTest, AuthoritativeIdRendersExactNumericToken) {
  EXPECT_EQ(RenderLarkMarker("<tool_call>", 248068), "<[248068]>");
}

TEST(RenderLarkMarkerTest, UnresolvedMarkerRendersEscapedLiteral) {
  EXPECT_EQ(RenderLarkMarker("<tool_call>", std::nullopt), "\"<tool_call>\"");
}

TEST(RenderLarkMarkerTest, NegativeTokenIdRendersEscapedLiteral) {
  EXPECT_EQ(RenderLarkMarker("<tool_call>", -1), "\"<tool_call>\"");
}

TEST(BuildLarkGrammarTest, UnresolvedToolMarkersRenderAsEscapedLiterals) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";

  std::string grammar = BuildLarkGrammar(ctx, R"({"type":"array"})");
  EXPECT_NE(grammar.find("toolcall: \"<tool_call>\" functioncall \"</tool_call>\""), std::string::npos);
  EXPECT_EQ(grammar.find("toolcall: <tool_call>"), std::string::npos);
}

TEST(BuildLarkGrammarTest, ToolMarkersMixNumericAndLiteralBoundaries) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";
  ctx.tool_call_start_token_id = 248068;

  std::string grammar = BuildLarkGrammar(ctx, R"({"type":"array"})");
  EXPECT_NE(grammar.find("toolcall: <[248068]> functioncall \"</tool_call>\""), std::string::npos);
}

TEST(BuildLarkGrammarTest, MultiTokenReasoningMarkersFallBackToLiterals) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = false;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";

  std::string grammar = BuildLarkGrammar(ctx, "{}");
  EXPECT_NE(grammar.find(R"(cot: "<think>" THINK_TEXT "</think>" "\n")"), std::string::npos);
  EXPECT_EQ(grammar.find("cot: <think>"), std::string::npos);
}

TEST(BuildLarkGrammarTest, Phi4MiniReasoningWithoutPublishedIdsUsesLiteralMarkers) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = false;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";

  const std::string grammar = BuildLarkGrammar(ctx, "{}");

  EXPECT_NE(grammar.find(R"(cot: "<think>" THINK_TEXT "</think>" "\n")"), std::string::npos);
  EXPECT_NE(grammar.find("THINK_TEXT: /[^<]+/"), std::string::npos);
  EXPECT_EQ(grammar.find("<["), std::string::npos);
}

TEST(BuildLarkGrammarTest, ToolAndReasoningMarkerIdsAreIndependent) {
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";
  ctx.tool_call_start_token_id = 248068;
  ctx.tool_call_end_token_id = 248069;

  std::string grammar = BuildLarkGrammar(ctx, R"({"type":"array"})");
  EXPECT_NE(grammar.find(R"(cot: "<think>" THINK_TEXT "</think>" "\n")"), std::string::npos);
  EXPECT_NE(grammar.find("toolcall: <[248068]> functioncall <[248069]>"), std::string::npos);
}

TEST(BuildLarkGrammarTest, NoMarkersUseModelIndependentLiteralFallbacks) {
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;

  std::string grammar = BuildLarkGrammar(ctx, R"({"type":"array"})");
  EXPECT_NE(grammar.find(R"(cot: "<think>" THINK_TEXT "</think>" "\n")"), std::string::npos);
  EXPECT_NE(grammar.find("output: TEXT | functioncall"), std::string::npos);
  EXPECT_EQ(grammar.find("toolcall:"), std::string::npos);
}

TEST(BuildLarkGrammarTest, PromptOpensReasoningOmitsCotOpener) {
  // The rendered prompt already opened reasoning (see PromptOpensReasoning), so the cot rule must not require the
  // model to re-emit the opener — only the closer remains.
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = false;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";
  ctx.reasoning_start_token_id = 248058;
  ctx.reasoning_end_token_id = 248059;
  std::string grammar = BuildLarkGrammar(ctx, "{}", /*prompt_opens_reasoning=*/true);

  std::string expected = R"(start: cot TEXT
cot: THINK_TEXT <[248059]> "\n"
THINK_TEXT: /[^<]+/
TEXT: /[^{<](.|\n)*/
)";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, PromptOpensReasoningOmitsCotOpenerWithToolCall) {
  // Same shortening applies when the turn's output is tool-call-only — only the cot body changes; the surrounding
  // start/toolcall productions are unaffected by prompt_opens_reasoning.
  ToolCallContext ctx;
  ctx.text_output = false;
  ctx.tool_output = true;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";
  ctx.reasoning_start_token_id = 248058;
  ctx.reasoning_end_token_id = 248059;
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";
  ctx.tool_call_start_token_id = 248068;
  ctx.tool_call_end_token_id = 248069;
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","parameters":{"type":"object"}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema, /*prompt_opens_reasoning=*/true);

  std::string expected =
      R"(start: cot toolcall
cot: THINK_TEXT <[248059]> "\n"
THINK_TEXT: /[^<]+/
toolcall: <[248068]> functioncall <[248069]>
functioncall: %json )" +
      json_schema + "\n";

  EXPECT_EQ(grammar, expected);
}

TEST(BuildLarkGrammarTest, PromptDoesNotOpenReasoningKeepsCotOpener) {
  // Sanity check for the default (false) case: the opener is present when the prompt did not already open
  // reasoning.
  ToolCallContext ctx;
  ctx.text_output = true;
  ctx.tool_output = false;
  ctx.supports_reasoning = true;
  ctx.reasoning_start = "<think>";
  ctx.reasoning_end = "</think>";
  ctx.reasoning_start_token_id = 248058;
  ctx.reasoning_end_token_id = 248059;
  std::string grammar = BuildLarkGrammar(ctx, "{}");
  EXPECT_NE(grammar.find("cot: <[248058]> THINK_TEXT <[248059]> \"\\n\""), std::string::npos);
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
  EXPECT_FALSE(ctx.tool_call_start_token_id.has_value());
  EXPECT_FALSE(ctx.tool_call_end_token_id.has_value());
  EXPECT_FALSE(ctx.reasoning_start_token_id.has_value());
  EXPECT_FALSE(ctx.reasoning_end_token_id.has_value());
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
  ctx.reasoning_start_token_id = 248058;
  ctx.reasoning_end_token_id = 248059;

  std::string grammar = BuildLarkGrammar(ctx, "{}");

  std::string expected = R"(start: cot TEXT
cot: <[248058]> THINK_TEXT <[248059]> "\n"
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
  ctx.reasoning_start_token_id = 248058;
  ctx.reasoning_end_token_id = 248059;
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";
  ctx.tool_call_start_token_id = 248068;
  ctx.tool_call_end_token_id = 248069;
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot toolcall
cot: <[248058]> THINK_TEXT <[248059]> "\n"
THINK_TEXT: /[^<]+/
toolcall: <[248068]> functioncall <[248069]>
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
  ctx.tool_call_start_token_id = 248068;
  ctx.tool_call_end_token_id = 248069;
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot toolcall
cot: "<think>" THINK_TEXT "</think>" "\n"
THINK_TEXT: /[^<]+/
toolcall: <[248068]> functioncall <[248069]>
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
  ctx.reasoning_start_token_id = 248058;
  ctx.reasoning_end_token_id = 248059;
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot functioncall
cot: <[248058]> THINK_TEXT <[248059]> "\n"
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
  ctx.reasoning_start_token_id = 248058;
  ctx.reasoning_end_token_id = 248059;
  ctx.tool_call_start = "<tool_call>";
  ctx.tool_call_end = "</tool_call>";
  ctx.tool_call_start_token_id = 248068;
  ctx.tool_call_end_token_id = 248069;
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot output
cot: <[248058]> THINK_TEXT <[248059]> "\n"
THINK_TEXT: /[^<]+/
output: TEXT | toolcall
TEXT: /[^{<](.|\n)*/
toolcall: <[248068]> functioncall <[248069]>
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
  ctx.tool_call_start_token_id = 248068;
  ctx.tool_call_end_token_id = 248069;
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot output
cot: "<think>" THINK_TEXT "</think>" "\n"
THINK_TEXT: /[^<]+/
output: TEXT | toolcall
TEXT: /[^{<](.|\n)*/
toolcall: <[248068]> functioncall <[248069]>
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
  ctx.reasoning_start_token_id = 248058;
  ctx.reasoning_end_token_id = 248059;
  ctx.tools_json = R"([{"type":"function","function":{"name":"get_weather","description":"Get weather","parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}}}])";

  std::string json_schema = BuildToolJsonSchema(ctx);
  std::string grammar = BuildLarkGrammar(ctx, json_schema);

  std::string expected =
      R"(start: cot output
cot: <[248058]> THINK_TEXT <[248059]> "\n"
THINK_TEXT: /[^<]+/
output: TEXT | functioncall
TEXT: /[^{<](.|\n)*/
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
TEXT: /[^{<](.|\n)*/
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
