// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/tool_call_plan.h"

#include "exception.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <regex>
#include <string>

namespace fl {
namespace {

using Json = nlohmann::json;

Json Tool(const std::string& name, const Json& parameters) {
  return {{"type", "function"},
          {"function", {{"name", name}, {"description", "test tool"}, {"parameters", parameters}, {"strict", false}}}};
}

std::shared_ptr<const ToolCallPlan> Plan(const Json& parameters) {
  return ToolCallPlan::Create(Json::array({Tool("edit", parameters)}).dump());
}

std::string Batch(const Json& parameters, const std::string& name = "edit") {
  return Json::array({{{"name", name}, {"parameters", parameters}}}).dump();
}

std::regex GrammarTerminal(const ToolCallPlan& plan, const std::string& name) {
  const auto grammar = plan.Grammar();
  const auto marker = name + ": /";
  const auto start = grammar.find(marker) + marker.size();
  const auto end = grammar.find("/\n", start);
  return std::regex(grammar.substr(start, end - start));
}

void ExpectError(const std::function<void()>& action, flErrorCode code, const std::string& detail) {
  try {
    action();
    FAIL() << "Expected fl::Exception";
  } catch (const Exception& e) {
    EXPECT_EQ(e.code(), code) << e.what();
    EXPECT_NE(std::string(e.what()).find(detail), std::string::npos) << e.what();
  }
}

TEST(ToolCallPlanTest, EnforcesMinLengthMaximumAndReportsToolInstanceAndKeyword) {
  const auto plan = Plan({{"type", "object"},
                          {"properties", {{"text", {{"type", "string"}, {"minLength", 3}}},
                                          {"count", {{"type", "number"}, {"maximum", 5}}}}},
                          {"required", {"text", "count"}}});
  const auto calls = plan->ParseCalls(Batch({{"text", "abc"}, {"count", 5}}));
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "edit");
  EXPECT_EQ(Json::parse(calls[0].arguments), (Json{{"text", "abc"}, {"count", 5}}));
  EXPECT_TRUE(calls[0].id.starts_with("call_"));

  ExpectError([&] { plan->ParseCalls(Batch({{"text", "ab"}, {"count", 5}})); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "minLength");
  ExpectError([&] { plan->ParseCalls(Batch({{"text", "abc"}, {"count", 6}})); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "/count");
  ExpectError([&] { plan->ParseCalls(Batch({{"text", "abc"}, {"count", 6}})); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "edit");
}

TEST(ToolCallPlanTest, PreservesOptionalNullableDefaultsAndOpenAdditionalProperties) {
  const Json arguments_schema = {
      {"type", "object"},
      {"properties", {{"optional", {{"type", {"string", "null"}}, {"default", "not inserted"}}},
                      {"required", {{"type", "integer"}}}}},
      {"required", {"required"}}};
  const auto plan = Plan(arguments_schema);
  const auto calls = plan->ParseCalls(Batch({{"required", 1}, {"extra", true}}));
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(Json::parse(calls[0].arguments), (Json{{"required", 1}, {"extra", true}}));
  EXPECT_EQ(plan->ParseCalls(Batch({{"required", 1}, {"optional", nullptr}})).size(), 1u);
  ExpectError([&] { plan->ParseCalls(Batch({{"optional", "x"}})); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "required");
  ExpectError([&] { plan->ParseCalls(Batch({{"required", "1"}})); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "type");

  EXPECT_EQ(plan->Grammar().find("%json"), std::string::npos);
}

TEST(ToolCallPlanTest, StructuralGrammarEmbedsUnderCallerRootWithoutSchemaOrGlobalWhitespaceDirectives) {
  const auto plan = Plan({{"properties", {{"input", {{"minLength", 100}}}}}});
  const auto grammar = plan->Grammar("functioncall");
  EXPECT_TRUE(grammar.starts_with(
      "functioncall: tool_json_ws \"[\" tool_json_ws tool_json_call "
      "(tool_json_comma tool_json_call)* tool_json_ws \"]\" tool_json_ws\n"));
  const std::string envelope =
      R"(tool_json_call: "{" tool_json_ws "\"name\"" tool_json_colon tool_json_name tool_json_comma )"
      R"("\"parameters\"" tool_json_colon tool_json_object tool_json_ws "}")";
  EXPECT_NE(grammar.find(envelope), std::string::npos);
  EXPECT_NE(grammar.find("tool_json_member: TOOL_JSON_STRING tool_json_colon tool_json_value"), std::string::npos);
  EXPECT_EQ(grammar.find("%json"), std::string::npos);
  EXPECT_EQ(grammar.find("%ignore"), std::string::npos);
  EXPECT_EQ(grammar.find("minLength"), std::string::npos);
  for (const std::string root : {"", "UPPER", "bad-name", "x\nstart", "tool_json_value"}) {
    ExpectError([&] { plan->Grammar(root); }, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "root rule");
  }
}

TEST(ToolCallPlanTest, StructuralWhitespaceIsExplicitOnlyAtJsonBoundariesAndUsesNonemptyAsciiTerminal) {
  const auto plan = Plan(Json::object());
  const auto grammar = plan->Grammar();
  EXPECT_NE(grammar.find("tool_json_ws: TOOL_JSON_WS?\n"), std::string::npos);
  EXPECT_NE(grammar.find("tool_json_comma: tool_json_ws \",\" tool_json_ws\n"), std::string::npos);
  EXPECT_NE(grammar.find("tool_json_colon: tool_json_ws \":\" tool_json_ws\n"), std::string::npos);
  EXPECT_NE(grammar.find("tool_json_object: \"{\" tool_json_ws "
                         "(tool_json_member (tool_json_comma tool_json_member)*)? tool_json_ws \"}\"\n"),
            std::string::npos);
  EXPECT_NE(grammar.find("tool_json_array: \"[\" tool_json_ws "
                         "(tool_json_value (tool_json_comma tool_json_value)*)? tool_json_ws \"]\"\n"),
            std::string::npos);
  const auto whitespace = GrammarTerminal(*plan, "TOOL_JSON_WS");
  for (const std::string text : {" ", "\t", "\r", "\n", " \t\r\n "}) {
    EXPECT_TRUE(std::regex_match(text, whitespace));
  }

  for (const std::string text : {"", "\v", "\f", "\xC2\xA0", "x", " ,", "1 "}) {
    EXPECT_FALSE(std::regex_match(text, whitespace));
  }
}

TEST(ToolCallPlanTest, SpacedNestedCallsPreserveStringDataAndRejectNonJsonWhitespace) {
  const auto plan = Plan({{"properties", {{"input", {{"type", "string"}}}}}, {"required", {"input"}}});
  const std::string batch = " \t\r\n" + std::string(R"json([
    { "name" : "edit" , "parameters" : {
      "input" : "  code \t\"quoted\"\n\\u003c  " ,
      "nested" : [ true , null , { "n" : -1.25e+2 } ] ,
      "empty" : { } , "array" : [ ]
    } }
  ])json") + "\t\r\n ";
  const auto calls = plan->ParseCalls(batch);
  ASSERT_EQ(calls.size(), 1u);
  const auto arguments = Json::parse(calls[0].arguments);
  EXPECT_EQ(arguments.at("input"), "  code \t\"quoted\"\n\\u003c  ");
  EXPECT_EQ(arguments.at("nested"), Json::array({true, nullptr, {{"n", -125.0}}}));
  EXPECT_EQ(arguments.at("empty"), Json::object());
  EXPECT_EQ(arguments.at("array"), Json::array());
  EXPECT_EQ(Json::parse(calls[0].argument_source), arguments);
  for (const std::string whitespace : {"\v", "\f", "\xC2\xA0"}) {
    const auto invalid = "[{\"name\"" + whitespace + ":\"edit\",\"parameters\":{\"input\":\"x\"}}]";
    ExpectError([&] { plan->ParseCalls(invalid); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "");
  }
}

TEST(ToolCallPlanTest, StructuralGrammarQuotesExactToolNamesAsLarkStringLiterals) {
  const std::string name = "quote\"\\/\n\xE9\x9B\xAA";
  const auto plan = ToolCallPlan::Create(Json::array({Tool(name, Json::object()), Tool("other", true)}).dump());
  const auto expected = "tool_json_name: " + Json(Json(name).dump()).dump() + " | \"\\\"other\\\"\"\n";
  EXPECT_NE(plan->Grammar().find(expected), std::string::npos);
}

TEST(ToolCallPlanTest, StructuralStringTerminalAcceptsPrintableUnicodeEscapesAndPairedSurrogates) {
  const auto plan = Plan(Json::object());
  const auto strings = GrammarTerminal(*plan, "TOOL_JSON_STRING");
  const std::string valid[] = {
      R"("")", R"("code <tag>\n\\path\"quoted\"\/")", R"("\u003c\u003E\u0026\u0041\u005c")",
      R"("\u0000\u001f\uD000\uD7ff\uE000\uFFFF")", R"("\uD83D\uDE00")", R"("\ud800\udc00")",
      "\"\xE9\x9B\xAA \xF0\x9F\x98\x80\""};
  for (const auto& text : valid) {
    EXPECT_TRUE(std::regex_match(text, strings)) << text;
  }

  const std::string invalid[] = {
      R"("\uD800")", R"("\uDC00")", R"("\uD800\uD800")", R"("\uD800\u0000")", R"("\u003")",
      R"("\uZZZZ")", R"("\x41")", R"("\v")", "\"raw\nnewline\"", "\"raw\tcontrol\"", "missing quotes"};
  for (const auto& text : invalid) {
    EXPECT_FALSE(std::regex_match(text, strings)) << text;
  }

  const auto calls = plan->ParseCalls(
      R"([{"name":"edit","parameters":{"input":"\u003c\u003E\u0026\u005c\u0022\u0041\uD83D\uDE00"}}])");
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(Json::parse(calls[0].arguments).at("input"), "<>&\\\"A\xF0\x9F\x98\x80");
}

TEST(ToolCallPlanTest, StructuralNumberTerminalUsesJsonNumberSyntaxWithoutCoercion) {
  const auto plan = Plan(Json::object());
  const auto numbers = GrammarTerminal(*plan, "TOOL_JSON_NUMBER");
  for (const std::string text : {"0", "-0", "123", "-123", "0.1", "1e3", "-1.25E+10", "1E-2"}) {
    EXPECT_TRUE(std::regex_match(text, numbers)) << text;
  }

  for (const std::string text : {"+1", "01", "-01", ".5", "1.", "1e", "NaN", "Infinity", " 1", "1 "}) {
    EXPECT_FALSE(std::regex_match(text, numbers)) << text;
  }
}

TEST(ToolCallPlanTest, PreservesArgumentSourceOrderWithoutRequiringOptionalOrClosingAdditionalProperties) {
  const std::string tools = R"([{"type":"function","function":{"name":"edit","parameters":{
    "type":"object",
    "properties":{"path":{"type":"string"},"old_str":{"type":"string"},"new_str":{"type":"string"}},
    "required":["path"]
  }}}])";
  const auto plan = ToolCallPlan::Create(tools);
  const std::string arguments = R"({"new_str":"line one\nline two","path":"file.cc","extra":null,"old_str":"old"})";
  const auto calls = plan->ParseCalls("[{\"name\":\"edit\",\"parameters\":" + arguments + "}]");
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].arguments, arguments);
  EXPECT_EQ(calls[0].argument_source, arguments);
  EXPECT_EQ(plan->ParseCalls(R"([{"name":"edit","parameters":{"path":"file.cc","extra":null}}])").size(), 1u);
  ExpectError([&] { plan->ParseCalls(R"([{"name":"edit","parameters":{"new_str":"text"}}])"); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "required");
}

TEST(ToolCallPlanTest, OneOfIsExclusiveAndAnyOfAllowsOverlappingMatches) {
  const Json choices = {{{"type", "number"}}, {{"type", "integer"}}};
  const auto exclusive = Plan({{"properties", {{"value", {{"oneOf", choices}}}}}});
  const auto inclusive = Plan({{"properties", {{"value", {{"anyOf", choices}}}}}});
  ExpectError([&] { exclusive->ParseCalls(Batch({{"value", 1}})); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "oneOf");
  EXPECT_EQ(exclusive->ParseCalls(Batch({{"value", 1.5}})).size(), 1u);
  EXPECT_EQ(inclusive->ParseCalls(Batch({{"value", 1}})).size(), 1u);
  ExpectError([&] { inclusive->ParseCalls(Batch({{"value", "1"}})); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "anyOf");
}

TEST(ToolCallPlanTest, LocalReferencesRemainIsolatedAcrossOriginalToolSchemas) {
  const Json first = {{"$defs", {{"value", {{"type", "string"}, {"minLength", 3}}}}},
                      {"properties", {{"x", {{"$ref", "#/$defs/value"}}}}}};
  const Json second = {{"$defs", {{"value", {{"type", "integer"}, {"maximum", 2}}}}},
                       {"properties", {{"x", {{"$ref", "#/$defs/value"}}}}}};
  const auto plan = ToolCallPlan::Create(Json::array({Tool("first", first), Tool("second", second)}).dump());
  EXPECT_EQ(plan->ParseCalls(Batch({{"x", "abc"}}, "first")).size(), 1u);
  EXPECT_EQ(plan->ParseCalls(Batch({{"x", 2}}, "second")).size(), 1u);
  ExpectError([&] { plan->ParseCalls(Batch({{"x", 3}}, "second")); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "maximum");
}

TEST(ToolCallPlanTest, Draft7LocalDefinitionsPreserveIgnoredRefSiblings) {
  const Json arguments = {{"$schema", "http://json-schema.org/draft-07/schema#"},
                          {"definitions", {{"text", {{"type", "string"}, {"minLength", 2}}}}},
                          {"properties", {{"x", {{"$ref", "#/definitions/text"}, {"maxLength", 1}}}}}};
  const auto plan = Plan(arguments);
  EXPECT_EQ(plan->ParseCalls(Batch({{"x", "long"}}, "edit")).size(), 1u);
  ExpectError([&] { plan->ParseCalls(Batch({{"x", "a"}})); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "minLength");
}

TEST(ToolCallPlanTest, Draft7ReferencesPreserveEscapedAndKeywordPropertyNames) {
  const Json arguments = {
      {"$schema", "http://json-schema.org/draft-07/schema#"},
      {"definitions", {{"alias", {{"type", "object"},
                                  {"properties", {{"a/b~c", {{"type", "integer"}, {"minimum", 2}}}}}}}}},
      {"properties", {{"definitions", {{"$ref", "#/definitions/alias"}}},
                      {"number", {{"$ref", "#/definitions/alias/properties/a~1b~0c"}}}}}};
  const auto plan = Plan(arguments);
  const auto valid = Batch({{"definitions", {{"a/b~c", 3}}}, {"number", 2}});
  const auto invalid = Batch({{"definitions", {{"a/b~c", 3}}}, {"number", 1}});
  EXPECT_EQ(plan->ParseCalls(valid).size(), 1u);
  ExpectError([&] { plan->ParseCalls(invalid); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "minimum");
}

TEST(ToolCallPlanTest, UnicodeMultilineAndLiteralMarkersRoundTripUnchanged) {
  const auto plan = Plan({{"properties", {{"input", {{"type", "string"}}}}}, {"required", {"input"}}});
  const std::string text =
      "line one\n\"quoted\" \\ slash\n\xE9\x9B\xAA \xF0\x9F\x98\x80"
      "\n<tool_call></tool_call><|im_end|>";
  const auto calls = plan->ParseCalls(Batch({{"input", text}}));
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(Json::parse(calls[0].arguments).at("input"), text);
  EXPECT_FALSE(calls[0].raw_envelope);
}

TEST(ToolCallPlanTest, RejectsDuplicateKeysUnknownMembersNamesAndMissingOrNonObjectParameters) {
  const auto plan = Plan({{"type", "object"}});
  const std::string invalid[] = {
      R"([{"name":"edit","name":"edit","parameters":{}}])",
      R"([{"name":"edit","parameters":{"x":1,"x":2}}])",
      R"([{"name":"edit","parameters":{"x":{"y":1,"y":2}}}])",
      R"([{"name":"edit","parameters":{},"extra":0}])",
      R"([{"name":"absent","parameters":{}}])",
      R"([{"name":"edit"}])",
      R"([{"name":"edit","parameters":null}])",
      R"([{"name":"edit","parameters":"text"}])",
      R"([{"name":"edit","parameters":[]}])",
      R"({"name":"edit","parameters":{}})",
      R"([{"name":"edit","parameters":{}},])"};
  for (const auto& body : invalid) {
    SCOPED_TRACE(body);
    ExpectError([&] { plan->ParseCalls(body); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "");
  }

  ExpectError([&] { plan->ParseCalls("[]"); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "non-empty");
  EXPECT_EQ(plan->ParseCalls(Batch(Json::object())).size(), 1u);
}

TEST(ToolCallPlanTest, EmptyBatchesFailParsingAndGrammarRequiresACallRegardlessOfParallelOrForcedChoice) {
  const auto tools = Json::array({Tool("edit", Json::object())}).dump();
  for (const bool parallel : {false, true}) {
    for (const bool forced : {false, true}) {
      SCOPED_TRACE("parallel=" + std::to_string(parallel) + ", forced=" + std::to_string(forced));
      const auto choice = forced ? std::optional<ForcedToolChoice>{ForcedToolChoice{"edit"}} : std::nullopt;
      const auto plan = ToolCallPlan::Create(tools, parallel, choice);
      const auto expected = parallel
                                ? "start: tool_json_ws \"[\" tool_json_ws tool_json_call "
                                  "(tool_json_comma tool_json_call)* tool_json_ws \"]\" tool_json_ws\n"
                                : "start: tool_json_ws \"[\" tool_json_ws tool_json_call "
                                  "tool_json_ws \"]\" tool_json_ws\n";
      EXPECT_TRUE(plan->Grammar().starts_with(expected));
      ExpectError([&] { plan->ParseCalls("[]"); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "non-empty");
      EXPECT_EQ(plan->ParseCalls(Batch(Json::object())).size(), 1u);
    }
  }
}

TEST(ToolCallPlanTest, ForcedNameAndParallelLimitConstrainParsingAndStructuralGrammar) {
  const auto tools = Json::array({Tool("first", Json::object()), Tool("second", Json::object())}).dump();
  const auto plan = ToolCallPlan::Create(tools, false, ForcedToolChoice{"second"});
  EXPECT_EQ(plan->ParseCalls(Batch(Json::object(), "second")).size(), 1u);
  ExpectError([&] { plan->ParseCalls(Batch(Json::object(), "first")); },
              FOUNDRY_LOCAL_ERROR_INTERNAL, "first");
  const auto two = Json::array({{{"name", "second"}, {"parameters", Json::object()}},
                                {{"name", "second"}, {"parameters", Json::object()}}})
                       .dump();
  ExpectError([&] { plan->ParseCalls(two); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "parallel");
  EXPECT_NE(plan->Grammar().find("tool_json_name: \"\\\"second\\\"\"\n"), std::string::npos);
  EXPECT_EQ(plan->Grammar().find("first"), std::string::npos);
  ExpectError([&] { ToolCallPlan::Create(tools, true, ForcedToolChoice{"missing"}); },
              FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "forced tool");
}

TEST(ToolCallPlanTest, RejectsMalformedSchemasUnsupportedFeaturesExternalAndRecursiveReferences) {
  const Json invalid[] = {
      {{"properties", {{"x", {{"minLength", -1}}}}}},
      {{"required", "x"}},
      {{"type", "typo"}},
      {{"oneOf", Json::array()}},
      {{"$schema", "https://json-schema.org/draft/2019-09/schema"}},
      {{"$ref", "https://example.org/schema"}},
      {{"$ref", "file:///schema.json"}},
      {{"$ref", "#/missing"}},
      {{"$ref", "#"}},
      {{"$id", "https://example.org/local"}},
      {{"$anchor", "a"}},
      {{"$vocabulary", Json::object()}},
      {{"$vocabulary", {{"https://json-schema.org/draft/2020-12/vocab/format-assertion", true}}}},
      {{"format", 1}},
      {{"$dynamicRef", "#node"}},
      {{"dependencies", {{"x", {"y"}}}}},
      {{"contentEncoding", "base64"}},
      {{"$defs", {{"loop", {{"$ref", "#/$defs/loop"}}}}}, {"$ref", "#/$defs/loop"}},
      {{"default", {{"$ref", "https://example.org/escape"}}}, {"$ref", "#/default"}},
      {{"$defs", {{"nested", {{"$schema", "http://json-schema.org/draft-07/schema#"}}}}}},
      {{"$schema", "http://json-schema.org/draft-07/schema#"}, {"items", Json::array({Json::object()})}},
      Json::array(),
      "not a schema"};
  for (const auto& document : invalid) {
    SCOPED_TRACE(document.dump());
    ExpectError([&] { Plan(document); }, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "parameters");
  }
}

TEST(ToolCallPlanTest, AnnotationObjectsAreNotMisinterpretedAsSchemas) {
  const Json literal = {{"$ref", "https://example.org/literal"}, {"unknown", "literal"}};
  const auto plan = Plan({{"properties", {{"x", {{"const", literal}, {"default", literal}}}}}});
  EXPECT_EQ(plan->ParseCalls(Batch({{"x", literal}})).size(), 1u);
}

TEST(ToolCallPlanTest, ExtensionAndFormatAnnotationsDoNotWeakenAssertionsOrResolveMetadata) {
  for (const auto& dialect : {"https://json-schema.org/draft/2020-12/schema",
                              "http://json-schema.org/draft-07/schema#"}) {
    SCOPED_TRACE(dialect);
    const Json parameters = {
        {"$schema", dialect},
        {"type", "object"},
        {"uiMetadata", {{"$ref", "https://example.invalid/annotation"}, {"minLength", -1}}},
        {"properties", {{"text", {{"type", "string"}, {"minLength", 3}, {"format", "email"},
                                  {"x-ui-widget", "textarea"}}}}},
        {"required", {"text"}}};
    const auto plan = Plan(parameters);
    const auto calls = plan->ParseCalls(Batch({{"text", "not an email"}}));
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(Json::parse(calls.front().arguments), (Json{{"text", "not an email"}}));
    ExpectError([&] { plan->ParseCalls(Batch({{"text", "ab"}})); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "minLength");
    ExpectError([&] { plan->ParseCalls(Batch({{"text", 123}})); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "type");
    ExpectError([&] { plan->ParseCalls(Batch(Json::object())); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "required");
  }
}

TEST(ToolCallPlanTest, ValidateCallRejectsInvalidArgumentsUtf8AndAbiNulWithoutCoercion) {
  const auto plan = Plan({{"properties", {{"n", {{"type", "integer"}}}}}, {"required", {"n"}}});
  const ParsedToolCall valid{"existing", "edit", R"({"n":1})"};
  EXPECT_NO_THROW(plan->ValidateCall(valid));
  const ParsedToolCall wrong{"existing", "edit", R"({"n":"1"})"};
  ExpectError([&] { plan->ValidateCall(wrong); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "type");
  const ParsedToolCall missing{"existing", "edit", "{}"};
  ExpectError([&] { plan->ValidateCall(missing); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "required");
  const ParsedToolCall duplicate{"existing", "edit", R"({"n":1,"n":2})"};
  ExpectError([&] { plan->ValidateCall(duplicate); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "Duplicate");
  const ParsedToolCall nul{"existing", "edit", std::string("{}\0", 3)};
  ExpectError([&] { plan->ValidateCall(nul); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "NUL");
  const ParsedToolCall utf8{"existing", "edit", std::string("\xFF")};
  ExpectError([&] { plan->ValidateCall(utf8); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "UTF-8");
}

TEST(ToolCallPlanTest, BoundsSchemaArgumentBytesAndNesting) {
  EXPECT_EQ(ToolCallPlan::kMaxSchemaBytes, 1024u * 1024u);
  EXPECT_EQ(ToolCallPlan::kMaxArgumentBytes, 4u * 1024u * 1024u);
  EXPECT_EQ(ToolCallPlan::kMaxBatchBytes, 16u * 1024u * 1024u);
  EXPECT_EQ(ToolCallPlan::kMaxDepth, 64);
  const std::string oversized_schema(ToolCallPlan::kMaxSchemaBytes + 1, ' ');
  ExpectError([&] { ToolCallPlan::Create(oversized_schema); }, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "byte limit");
  const auto plan = Plan(Json::object());
  const ParsedToolCall oversized{"existing", "edit", std::string(ToolCallPlan::kMaxArgumentBytes + 1, ' ')};
  ExpectError([&] { plan->ValidateCall(oversized); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "byte limit");
  const std::string oversized_batch(ToolCallPlan::kMaxBatchBytes + 1, ' ');
  ExpectError([&] { plan->ParseCalls(oversized_batch); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "byte limit");
  const auto deep = std::string(ToolCallPlan::kMaxDepth + 6, '[') + "0" +
                    std::string(ToolCallPlan::kMaxDepth + 6, ']');
  ExpectError([&] { plan->ParseCalls(deep); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "nesting");
  const std::string payload(1024 * 1024 + 1, 'x');
  const auto calls = plan->ParseCalls(Batch({{"input", payload}}));
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(Json::parse(calls[0].arguments).at("input").get<std::string>().size(), payload.size());
}

TEST(ToolCallPlanTest, SourceProvenanceRemainsAuthoritativeAfterCustomArgumentUnwrapping) {
  const auto plan = Plan({{"properties", {{"input", {{"type", "string"}, {"minLength", 3}}}}},
                          {"required", {"input"}},
                          {"additionalProperties", false}});
  const auto calls = plan->ParseCalls(Batch({{"input", "line one\n<tool_call>\xE9\x9B\xAA"}}));
  ASSERT_EQ(calls.size(), 1u);
  auto unwrapped = calls[0];
  unwrapped.arguments = Json::parse(unwrapped.arguments).at("input").get<std::string>();
  EXPECT_NO_THROW(plan->ValidateCall(unwrapped));
  EXPECT_NO_THROW(plan->ValidateCall(unwrapped));
  EXPECT_EQ(Json::parse(unwrapped.argument_source).at("input"), unwrapped.arguments);

  unwrapped.argument_source = R"({"input":"ab"})";
  ExpectError([&] { plan->ValidateCall(unwrapped); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "minLength");
  unwrapped.argument_source = R"({"input":3})";
  ExpectError([&] { plan->ValidateCall(unwrapped); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "type");
  unwrapped.argument_source.clear();
  ExpectError([&] { plan->ValidateCall(unwrapped); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "");
}

TEST(ToolCallPlanTest, OmittedParametersDeclareParameterlessToolsRequiringExplicitEmptyObjectArguments) {
  const std::string declarations[] = {
      R"([{"type":"function","function":{"name":"ping"}}])",
      R"([{"type":"function","name":"ping"}])"};
  for (const auto& tools : declarations) {
    SCOPED_TRACE(tools);
    const auto plan = ToolCallPlan::Create(tools);
    const auto calls = plan->ParseCalls(R"([{"name":"ping","parameters":{}}])");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].name, "ping");
    EXPECT_EQ(calls[0].arguments, "{}");
    EXPECT_EQ(calls[0].argument_source, "{}");
    EXPECT_NO_THROW(plan->ValidateCall(calls[0]));
    ExpectError([&] { plan->ParseCalls(R"([{"name":"ping","parameters":{"extra":null}}])"); },
                FOUNDRY_LOCAL_ERROR_INTERNAL, "additionalProperties");
    ExpectError([&] { plan->ValidateCall(ParsedToolCall{"existing", "ping", R"({"extra":1})"}); },
                FOUNDRY_LOCAL_ERROR_INTERNAL, "additionalProperties");
    ExpectError([&] { plan->ParseCalls(R"([{"name":"ping"}])"); },
                FOUNDRY_LOCAL_ERROR_INTERNAL, "parameters");
    ExpectError([&] { plan->ParseCalls(R"([{"name":"ping","parameters":null}])"); },
                FOUNDRY_LOCAL_ERROR_INTERNAL, "object");
  }
}

TEST(ToolCallPlanTest, SuppliedInvalidParameterSchemasAreNeverTreatedAsParameterlessOmissions) {
  const Json invalid[] = {nullptr, "object", Json::array(), 1, {{"type", "typo"}}, {{"additionalProperties", nullptr}}};
  for (const auto& parameters : invalid) {
    SCOPED_TRACE(parameters.dump());
    ExpectError([&] { ToolCallPlan::Create(Json::array({Tool("ping", parameters)}).dump()); },
                FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "parameters");
  }
}

TEST(ToolCallPlanTest, DuplicateParameterlessNamesReportDuplicateNameRatherThanMissingParameters) {
  ExpectError([] {
    ToolCallPlan::Create(
        R"([{"type":"function","function":{"name":"ping"}},{"type":"function","name":"ping"}])");
  },
              FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "duplicate tool name");
}

TEST(ToolCallPlanTest, RejectsMalformedToolDeclarationsAndDuplicateToolNames) {
  ExpectError([] { ToolCallPlan::Create("[]"); }, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "non-empty");
  ExpectError([] { ToolCallPlan::Create("{}"); }, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "array");
  ExpectError([] { ToolCallPlan::Create(R"([{"type":"function","function":{"name":"edit","parameters":null}}])"); },
              FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "parameters");
  const auto tool = Tool("edit", Json::object());
  ExpectError([&] { ToolCallPlan::Create(Json::array({tool, tool}).dump()); },
              FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "duplicate");
  ExpectError([] { ToolCallPlan::Create(Json::array({Tool("", Json::object())}).dump()); },
              FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "empty");
  ExpectError([] { ToolCallPlan::Create(R"([{"type":"function","name":"a","name":"b","parameters":{}}])"); },
              FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Duplicate");
}

TEST(ToolCallPlanTest, EntireBatchFailsIfLaterCallIsInvalid) {
  const auto plan = Plan({{"properties", {{"n", {{"type", "integer"}}}}}, {"required", {"n"}}});
  const auto batch = Json::array({{{"name", "edit"}, {"parameters", {{"n", 1}}}},
                                  {{"name", "edit"}, {"parameters", {{"n", "bad"}}}}})
                         .dump();
  ExpectError([&] { plan->ParseCalls(batch); }, FOUNDRY_LOCAL_ERROR_INTERNAL, "/n");
}

}  // namespace
}  // namespace fl
