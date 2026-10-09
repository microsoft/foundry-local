// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for ToolCallStreamAccumulator — the streaming state machine that separates visible assistant text from
// buffered tool-call blocks. These tests pin down the cross-token marker buffering, parse-on-close semantics, and
// EOS draining that the chat streaming paths rely on.
//
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/toolcalling/qwen_xml_tool_call_decoder.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/session/tool_registry.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace fl;

namespace {

// Concatenate text events from a sequence of Push results.
std::string CollectVisible(const std::vector<ToolCallStreamAccumulator::Output>& outs) {
  std::string s;
  for (const auto& o : outs) {
    for (const auto& event : o.events) {
      if (const auto* text = std::get_if<std::string>(&event)) {
        s += *text;
      }
    }
  }
  return s;
}

std::vector<ParsedToolCall> CollectCalls(std::vector<ToolCallStreamAccumulator::Output>& outs) {
  std::vector<ParsedToolCall> calls;
  for (auto& output : outs) {
    for (auto& event : output.events) {
      if (auto* call = std::get_if<ParsedToolCall>(&event)) {
        calls.push_back(std::move(*call));
      }
    }
  }
  return calls;
}

// Run a sequence of chunks through the accumulator, calling Flush at the end. Returns one Output per chunk plus
// the Flush Output appended last.
std::vector<ToolCallStreamAccumulator::Output> RunChunks(ToolCallStreamAccumulator& acc,
                                                         const std::vector<std::string>& chunks) {
  std::vector<ToolCallStreamAccumulator::Output> outs;
  outs.reserve(chunks.size() + 1);
  for (const auto& c : chunks) {
    outs.push_back(acc.Push(c));
  }
  outs.push_back(acc.Flush());
  return outs;
}

const std::string kQwenTools = R"([
  {
    "type": "function",
    "function": {
      "name": "typed",
      "parameters": {
        "type": "object",
        "properties": {
          "text": {"type": "string"},
          "empty": {"type": "string"},
          "number": {"type": "number"},
          "integer": {"type": "integer"},
          "boolean": {"type": "boolean"},
          "array": {"type": "array"},
          "object": {"type": "object"},
          "nothing": {"type": "null"}
        },
        "required": ["text"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "zero",
      "parameters": {"type": "object", "properties": {}}
    }
  }
])";

const std::unordered_map<std::string, ToolKind> kQwenToolKinds = {
    {"typed", ToolKind::kFunction},
    {"zero", ToolKind::kFunction},
};

ToolCallStreamAccumulator MakeQwenAccumulator(
    const std::string& tools = kQwenTools,
    const std::unordered_map<std::string, ToolKind>& tool_kinds = kQwenToolKinds,
    bool recovery_aware = false) {
  return ToolCallStreamAccumulator("<tool_call>", "</tool_call>", tools, "",
                                   CreateQwenXmlToolCallPayloadParser(tools, tool_kinds, recovery_aware));
}

std::string MakeOversizedQwenCall() {
  return "<tool_call>\n<function=typed>\n<parameter=text>\n" + std::string(70 * 1024, 'x') +
         "\n</parameter>\n</function>\n</tool_call>";
}

constexpr size_t kSelectedPayloadBufferLimit = 64 * 1024;

std::string MakeSizedQwenCall(size_t encoded_size) {
  constexpr std::string_view prefix =
      "<tool_call>\n<function=typed>\n<parameter=text>\n";
  constexpr std::string_view suffix =
      "\n</parameter>\n</function>\n</tool_call>";
  EXPECT_GE(encoded_size, prefix.size() + suffix.size());
  return std::string(prefix) +
         std::string(encoded_size - prefix.size() - suffix.size(), 'x') +
         std::string(suffix);
}

const std::string kValidZeroQwenCall =
    "<tool_call>\n<function=zero>\n</function>\n</tool_call>";

const std::string kValidTypedQwenCall =
    "<tool_call>\n"
    "<function=typed>\n"
    "<parameter=text>\n"
    "first\n"
    "</parameter>\n"
    "</function>\n"
    "</tool_call>";

std::vector<std::string> SplitAt(const std::string& source, size_t position) {
  return {source.substr(0, position), source.substr(position)};
}

std::vector<std::string> SplitIntoBytes(const std::string& source) {
  std::vector<std::string> chunks;
  chunks.reserve(source.size());
  for (const char byte : source) {
    chunks.emplace_back(1, byte);
  }

  return chunks;
}

struct AccumulatedOutput {
  std::string visible;
  std::vector<ParsedToolCall> calls;
};

AccumulatedOutput RunQwen(
    const std::vector<std::string>& chunks,
    const std::string& tools = kQwenTools,
    const std::unordered_map<std::string, ToolKind>& tool_kinds = kQwenToolKinds) {
  auto accumulator = MakeQwenAccumulator(tools, tool_kinds);
  auto outputs = RunChunks(accumulator, chunks);
  auto visible = CollectVisible(outputs);
  return {std::move(visible), CollectCalls(outputs)};
}

bool AnyMalformed(const std::vector<ToolCallStreamAccumulator::Output>& outputs) {
  return std::ranges::any_of(outputs, [](const auto& output) { return output.malformed; });
}

void ExpectSchemaFailureWithoutCalls(
    const std::vector<std::string>& chunks,
    const std::string& tools = kQwenTools,
    const std::unordered_map<std::string, ToolKind>& kinds = kQwenToolKinds) {
  auto accumulator = MakeQwenAccumulator(tools, kinds);
  auto outputs = RunChunks(accumulator, chunks);
  EXPECT_TRUE(AnyMalformed(outputs));
  EXPECT_TRUE(CollectVisible(outputs).empty());
  EXPECT_TRUE(CollectCalls(outputs).empty());
}

void ExpectTwoCallsAroundVisibleTail(const std::vector<std::string>& chunks,
                                     const std::string& expected_visible) {
  auto output = RunQwen(chunks);
  EXPECT_EQ(output.visible, expected_visible);
  ASSERT_EQ(output.calls.size(), 2u);
  EXPECT_EQ(output.calls[0].name, "typed");
  EXPECT_EQ(output.calls[0].arguments, R"({"text":"first"})");
  EXPECT_EQ(output.calls[1].name, "zero");
  EXPECT_EQ(output.calls[1].arguments, "{}");
}

}  // namespace

// ========================================================================
// Request-selected exact Qwen XML payload strategy.
// ========================================================================

TEST(QwenXmlToolCallAccumulatorTest, ValidCallIsHiddenAndEmittedOnlyWhenBatchEnds) {
  auto acc = MakeQwenAccumulator();
  const std::string generated =
      "<tool_call>\n"
      "<function=typed>\n"
      "<parameter=text>\n"
      "hello\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";

  const auto pending = acc.Push(generated);
  EXPECT_TRUE(pending.events.empty());
  EXPECT_TRUE(acc.InsideToolCall());

  auto completed = acc.Push("after");
  ASSERT_EQ(completed.events.size(), 2u);
  const auto& call = std::get<ParsedToolCall>(completed.events[0]);
  EXPECT_EQ(call.name, "typed");
  EXPECT_EQ(call.arguments, R"({"text":"hello"})");
  EXPECT_EQ(call.argument_source, call.arguments);
  EXPECT_TRUE(call.id.starts_with("call_"));
  EXPECT_EQ(std::get<std::string>(completed.events[1]), "after");
  EXPECT_FALSE(acc.InsideToolCall());
}

TEST(QwenXmlToolCallAccumulatorTest, UnsupportedSchemaKeywordsRemainUnadmittable) {
  const std::vector<nlohmann::json> schemas = {
      {{"type", "object"},
       {"properties", {{"value", {{"type", "string"}, {"pattern", "^[a-z]+$"}}}}}},
      {{"type", "object"},
       {"properties", {{"value", {{"type", "string"}, {"format", "date-time"}}}}}},
      {{"type", "object"},
       {"properties", {{"value", {{"$ref", "#/$defs/missing"}}}}},
       {"$defs", {{"value", {{"type", "string"}}}}}},
  };

  for (const auto& parameters : schemas) {
    SCOPED_TRACE(parameters.dump());
    const auto tools = nlohmann::json::array(
                           {{{"type", "function"},
                             {"function", {{"name", "fn"}, {"parameters", parameters}}}}})
                           .dump();

    const auto kinds = std::unordered_map<std::string, ToolKind>{{"fn", ToolKind::kFunction}};
    ASSERT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools, kinds)));
    const std::string call =
        "<tool_call>\n<function=fn>\n<parameter=value>\ntext\n</parameter>\n</function>\n</tool_call>";
    ExpectSchemaFailureWithoutCalls({call}, tools, kinds);
    EXPECT_TRUE(ParseQwenGuidedToolCalls(
        R"([{"name":"fn","parameters":{"value":"text"}}])", tools, kinds).empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, WholeSetGrammarIncludesGrepPathsAndPreservesMultilineCode) {
  const std::string tools = R"([
      {"type":"function","function":{"name":"grep","parameters":{"type":"object",
        "properties":{"paths":{"type":"array","items":{"type":"string"}},
                      "query":{"type":"string"}},"required":["paths","query"]}}},
      {"type":"function","function":{"name":"edit","parameters":{"type":"object",
        "properties":{"path":{"type":"string"},"code":{"type":"string"}},"required":["path","code"]}}}
    ])";
  const std::unordered_map<std::string, ToolKind> kinds{
      {"grep", ToolKind::kFunction}, {"edit", ToolKind::kFunction}};
  const auto grammar = BuildQwenXmlToolBodyGrammar(tools, kinds);
  ASSERT_TRUE(grammar.has_value());
  EXPECT_NE(grammar->find("<parameter=paths>"), std::string::npos);
  EXPECT_NE(grammar->find("<parameter=code>"), std::string::npos);
  EXPECT_NE(grammar->find("body[suffix=\"\\n</parameter>\\n\"]"), std::string::npos);
  EXPECT_NE(grammar->find("BODY: /(?s:.*)/ & ~/"), std::string::npos);
  EXPECT_NE(grammar->find("<\\/tool_call>"), std::string::npos);
  EXPECT_EQ(grammar->find("start: \"<tool_call>\""), std::string::npos);

  const auto valid = RunQwen({"<tool_call>\n<function=grep>\n<parameter=query>\nneedle\n</parameter>\n"
                              "<parameter=paths>\n[\"src/a.cc\",\"src/b.cc\"]\n</parameter>\n"
                              "</function>\n</tool_call>"},
                             tools, kinds);
  ASSERT_EQ(valid.calls.size(), 1u);
  EXPECT_EQ(valid.calls.front().arguments, R"({"paths":["src/a.cc","src/b.cc"],"query":"needle"})");
  EXPECT_TRUE(valid.visible.empty());

  const auto code = RunQwen({"<tool_call>\n<function=edit>\n<parameter=code>\nif (x < 3) {\n"
                             "  return \"a\\\\b\";\n}\n</parameter>\n<parameter=path>\n"
                             "src/a.cc\n</parameter>\n</function>\n</tool_call>"},
                            tools, kinds);
  ASSERT_EQ(code.calls.size(), 1u);
  EXPECT_EQ(nlohmann::json::parse(code.calls.front().arguments)["code"],
            "if (x < 3) {\n  return \"a\\\\b\";\n}");
  EXPECT_TRUE(code.visible.empty());

  ExpectSchemaFailureWithoutCalls(
      {"<tool_call>\n<function=grep>\n<parameter=path>\nsrc/a.cc\n</parameter>\n"
       "<parameter=query>\nneedle\n</parameter>\n</function>\n</tool_call>"},
      tools, kinds);
}

TEST(QwenXmlToolCallAccumulatorTest, PlannerUsesLinearStatesForTwelveAndTwentyEightMixedFields) {
  for (const size_t count : {12u, 28u}) {
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    for (size_t index = 0; index < count; ++index) {
      const auto name = "field_" + std::to_string(index);
      properties[name] = {{"type", "string"}};
      if (index % 2 == 0) {
        required.push_back(name);
      }
    }

    const auto tools = nlohmann::json::array(
        {{{"type", "function"}, {"function", {{"name", "large"},
          {"parameters", {{"type", "object"}, {"properties", properties}, {"required", required}}}}}}}).dump();
    const auto kinds = std::unordered_map<std::string, ToolKind>{{"large", ToolKind::kFunction}};
    const auto grammar = BuildQwenXmlToolBodyGrammar(tools, kinds);
    ASSERT_TRUE(grammar) << count;
    EXPECT_NE(grammar->find("state_0_" + std::to_string(count) + ":"), std::string::npos);
    EXPECT_EQ(grammar->find("state_0_" + std::to_string(count + 1) + ":"), std::string::npos);
    if (count == 28) {
      EXPECT_NE(grammar->find("<parameter=field_27>"), std::string::npos);
    }

    nlohmann::json arguments = nlohmann::json::object();
    for (const auto& name : required) {
      arguments[name.get<std::string>()] = "ok";
    }

    const auto accepted = ParseQwenGuidedToolCalls(
        nlohmann::json::array({{{"name", "large"}, {"parameters", arguments}}}).dump(), tools, kinds);
    ASSERT_EQ(accepted.size(), 1u);
    EXPECT_EQ(nlohmann::json::parse(accepted.front().arguments), arguments);
    arguments.erase("field_0");
    EXPECT_TRUE(ParseQwenGuidedToolCalls(
        nlohmann::json::array({{{"name", "large"}, {"parameters", arguments}}}).dump(), tools, kinds).empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, SyntheticSeventyEightToolOfferKeepsDeclaredNamesDistinct) {
  nlohmann::json tools = nlohmann::json::array();
  std::unordered_map<std::string, ToolKind> kinds;
  for (size_t index = 0; index < 78; ++index) {
    const auto name = "tool_" + std::to_string(index);
    nlohmann::json properties = nlohmann::json::object();
    properties["value"] = {{"type", "string"}};
    if (index == 77) {
      properties["paths"] = {{"anyOf", nlohmann::json::array({
          {{"type", "string"}}, {{"type", "array"}, {"items", {{"type", "string"}}}}})}};
    }

    tools.push_back({{"type", "function"}, {"function", {{"name", name},
        {"parameters", {{"type", "object"}, {"properties", properties}, {"required", {"value"}}}}}}});
    kinds.emplace(name, ToolKind::kFunction);
  }

  const auto grammar = BuildQwenXmlToolBodyGrammar(tools.dump(), kinds);
  ASSERT_TRUE(grammar);
  EXPECT_NE(grammar->find("tool_77:"), std::string::npos);
  EXPECT_NE(grammar->find("<function=tool_77>"), std::string::npos);
  EXPECT_NE(grammar->find("<parameter=paths>"), std::string::npos);
  EXPECT_EQ(grammar->find("tool_78:"), std::string::npos);
  EXPECT_EQ(grammar->find("<parameter=path>"), std::string::npos);
}

TEST(QwenXmlToolCallAccumulatorTest, SyntheticGrepPathsUnionAcceptsStringOrArrayButNeverPathAlias) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"grep","parameters":{"type":"object",)"
      R"("properties":{"paths":{"anyOf":[{"type":"string"},{"type":"array","items":{"type":"string"}}]},)"
      R"("pattern":{"type":"string"}},"required":["pattern","paths"]}}}])";
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"grep", ToolKind::kFunction}};
  const auto grammar = BuildQwenXmlToolBodyGrammar(tools, kinds);
  ASSERT_TRUE(grammar);
  EXPECT_NE(grammar->find("<parameter=paths>"), std::string::npos);
  EXPECT_EQ(grammar->find("<parameter=path>"), std::string::npos);

  for (const auto value : {"src/a.cc", R"(["src/a.cc","src/b.cc"])"}) {
    const auto call = "<tool_call>\n<function=grep>\n<parameter=paths>\n" + std::string(value) +
                      "\n</parameter>\n<parameter=pattern>\nneedle\n</parameter>\n</function>\n</tool_call>";
    const auto result = RunQwen({call}, tools, kinds);
    ASSERT_EQ(result.calls.size(), 1u) << value;
    EXPECT_EQ(nlohmann::json::parse(result.calls.front().arguments)["paths"],
              value[0] == '[' ? nlohmann::json::parse(value) : nlohmann::json(value));
  }

  ExpectSchemaFailureWithoutCalls(
      {"<tool_call>\n<function=grep>\n<parameter=path>\nsrc/a.cc\n</parameter>\n"
       "<parameter=pattern>\nneedle\n</parameter>\n</function>\n</tool_call>"}, tools, kinds);
}

TEST(QwenXmlToolCallAccumulatorTest, GrepGrammarEmitsRequiredPatternBeforeOptionalPaths) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"grep","parameters":{"type":"object",)"
      R"("properties":{"paths":{"anyOf":[{"type":"string"},{"type":"array","items":{"type":"string"}}]},)"
      R"("pattern":{"type":"string"},"type":{"type":"string"}},"required":["pattern"]}}}])";
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"grep", ToolKind::kFunction}};
  const auto grammar = BuildQwenXmlToolBodyGrammar(tools, kinds);
  ASSERT_TRUE(grammar);

  const auto required_start = grammar->find("state_0_0:");
  const auto paths_start = grammar->find("state_0_1:");
  const auto type_start = grammar->find("state_0_2:");
  ASSERT_NE(required_start, std::string::npos);
  ASSERT_NE(paths_start, std::string::npos);
  ASSERT_NE(type_start, std::string::npos);
  ASSERT_LT(required_start, paths_start);
  ASSERT_LT(paths_start, type_start);
  const auto required_state = grammar->substr(required_start, paths_start - required_start);
  const auto paths_state = grammar->substr(paths_start, type_start - paths_start);
  EXPECT_NE(required_state.find("<parameter=pattern>"), std::string::npos);
  EXPECT_EQ(required_state.find("| state_0_1"), std::string::npos);
  EXPECT_NE(paths_state.find("<parameter=paths>"), std::string::npos);
  EXPECT_NE(paths_state.find("| state_0_2"), std::string::npos);
  EXPECT_EQ(grammar->find("<parameter=path>"), std::string::npos);

  for (const auto& body : {
           std::string("<parameter=pattern>\nTODO\n</parameter>\n"
                       "<parameter=paths>\nsrc\n</parameter>\n"),
           std::string("<parameter=paths>\nsrc\n</parameter>\n"
                       "<parameter=pattern>\nTODO\n</parameter>\n")}) {
    const auto output = RunQwen({"<tool_call>\n<function=grep>\n" + body +
                                 "</function>\n</tool_call>"}, tools, kinds);
    ASSERT_EQ(output.calls.size(), 1u);
    EXPECT_EQ(output.calls.front().arguments, R"({"paths":"src","pattern":"TODO"})");
    EXPECT_TRUE(output.visible.empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, LocalReferencesAndDuplicateEnumsValidateNestedObjects) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"save_workflow","parameters":{)"
      R"("$defs":{"Mode":{"type":"string","enum":["manual","manual","auto"]}},)"
      R"("type":"object","properties":{"mode":{"anyOf":[{"$ref":"#/$defs/Mode"},{"type":"null"}]},)"
      R"("steps":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"}},)"
      R"("required":["name"],"additionalProperties":false}}},"required":["steps"]}}}])";
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"save_workflow", ToolKind::kFunction}};
  ASSERT_TRUE(BuildQwenXmlToolBodyGrammar(tools, kinds));

  const auto accepted = ParseQwenGuidedToolCalls(
      R"([{"name":"save_workflow","parameters":{"mode":"manual","steps":[{"name":"compile"}]}}])",
      tools, kinds);
  ASSERT_EQ(accepted.size(), 1u);
  EXPECT_EQ(nlohmann::json::parse(accepted.front().arguments)["steps"][0]["name"], "compile");

  for (const auto* parameters : {
           R"({"mode":"unknown","steps":[{"name":"compile"}]})",
           R"({"mode":"manual","steps":[{"name":7}]})",
           R"({"mode":"manual","steps":[{"name":"compile","extra":true}]})",
           R"({"mode":"manual","steps":[{}]})"}) {
    EXPECT_TRUE(ParseQwenGuidedToolCalls(
        std::string("[{\"name\":\"save_workflow\",\"parameters\":") + parameters + "}]", tools, kinds).empty())
        << parameters;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, NullableObjectAndOpenNestedPropertiesRemainTypeChecked) {
  const auto tools = R"([{"type":"function","function":{"name":"run_factory","parameters":{"type":"object",)"
                     R"("properties":{"args":{"type":["object","null"]},"config":{"type":"object",)"
                     R"("properties":{"enabled":{"type":"boolean"}},"required":["enabled"],)"
                     R"("additionalProperties":true}},"required":["config"]}}}])";
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"run_factory", ToolKind::kFunction}};
  ASSERT_TRUE(BuildQwenXmlToolBodyGrammar(tools, kinds));
  const auto accepted = ParseQwenGuidedToolCalls(
      R"([{"name":"run_factory","parameters":{"args":null,"config":{"enabled":true,"extra":42}}}])",
      tools, kinds);
  ASSERT_EQ(accepted.size(), 1u);
  EXPECT_EQ(nlohmann::json::parse(accepted.front().arguments)["args"], nullptr);
  EXPECT_EQ(nlohmann::json::parse(accepted.front().arguments)["config"]["extra"], 42);

  EXPECT_TRUE(ParseQwenGuidedToolCalls(
      R"([{"name":"run_factory","parameters":{"args":[],"config":{"enabled":true}}}])",
      tools, kinds).empty());
  EXPECT_TRUE(ParseQwenGuidedToolCalls(
      R"([{"name":"run_factory","parameters":{"config":{"enabled":"true"}}}])",
      tools, kinds).empty());
}

TEST(QwenXmlToolCallAccumulatorTest, NestedExclusiveUnionAndConstAdmitOnlyOneMatchingShape) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"create","parameters":{"type":"object","properties":{)"
      R"("attachments":{"type":"array","items":{"oneOf":[)"
      R"({"type":"object","properties":{"type":{"type":"string","const":"file"},"path":{"type":"string"}},)"
      R"("required":["type","path"],"additionalProperties":false},)"
      R"({"type":"object","properties":{"type":{"type":"string","const":"blob"},"data":{"type":"string"}},)"
      R"("required":["type","data"],"additionalProperties":false}]}}},)"
      R"("required":["attachments"]}}}])";
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"create", ToolKind::kFunction}};
  ASSERT_TRUE(BuildQwenXmlToolBodyGrammar(tools, kinds));

  const auto make_payload = [](std::string_view item) {
    return std::string(R"([{"name":"create","parameters":{"attachments":[)") +
           std::string(item) + R"(]}}])";
  };
  for (const auto* item : {
           R"({"type":"file","path":"src/a.cc"})",
           R"({"type":"blob","data":"aGVsbG8="})"}) {
    const auto calls = ParseQwenGuidedToolCalls(make_payload(item), tools, kinds);
    ASSERT_EQ(calls.size(), 1u) << item;
    EXPECT_EQ(nlohmann::json::parse(calls.front().arguments)["attachments"][0],
              nlohmann::json::parse(item));
  }

  for (const auto* item : {
           R"({"type":"file","data":"not-a-path"})",
           R"({"type":"blob","data":"ok","path":"unexpected"})",
           R"({"type":"unknown","path":"src/a.cc"})"}) {
    EXPECT_TRUE(ParseQwenGuidedToolCalls(make_payload(item), tools, kinds).empty()) << item;
  }

  const auto overlapping = R"([{"type":"function","function":{"name":"ambiguous",)"
                           R"("parameters":{"type":"object","properties":{"value":{"type":"array","items":{)"
                           R"("oneOf":[{"type":"string"},{"type":"string"}]}}},"required":["value"]}}}])";
  const auto overlap_kinds = std::unordered_map<std::string, ToolKind>{{"ambiguous", ToolKind::kFunction}};
  ASSERT_TRUE(BuildQwenXmlToolBodyGrammar(overlapping, overlap_kinds));
  EXPECT_TRUE(ParseQwenGuidedToolCalls(
      R"([{"name":"ambiguous","parameters":{"value":["matches-both"]}}])",
      overlapping, overlap_kinds).empty());
}

TEST(QwenXmlToolCallAccumulatorTest, SyntheticIncompleteRequiredSchemasCannotBeGuessed) {
  const auto good = nlohmann::json::parse(
      R"({"type":"function","function":{"name":"grep","parameters":{"type":"object",)"
      R"("properties":{"paths":{"type":"array","items":{"type":"string"}}},"required":["paths"]}}})");
  const std::vector<nlohmann::json> invalid_parameters = {
      nlohmann::json::parse(R"({"type":"object","properties":{"command":{"type":"string"}},)"
                            R"("required":["command","description"]})"),
      nlohmann::json::parse(
          R"({"type":"object","properties":{"meta":{"type":"object","properties":{)"
          R"("phases":{"type":"array","items":{"type":"object",)"
          R"("properties":{"name":{"type":"string"}},"required":["name","title"]}}}}}})"),
  };
  const auto kinds = std::unordered_map<std::string, ToolKind>{
      {"grep", ToolKind::kFunction}, {"unknown_schema", ToolKind::kFunction}};
  for (const auto& parameters : invalid_parameters) {
    const auto tools = nlohmann::json::array({
        good, {{"type", "function"}, {"function", {{"name", "unknown_schema"}, {"parameters", parameters}}}}
    }).dump();
    EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(tools, kinds)) << parameters.dump();
    EXPECT_TRUE(ParseQwenGuidedToolCalls(
        R"([{"name":"grep","parameters":{"paths":["src/a.cc"]}}])", tools, kinds).empty());
    ExpectSchemaFailureWithoutCalls(
        {"<tool_call>\n<function=unknown_schema>\n</function>\n</tool_call>"}, tools, kinds);
  }
}

TEST(QwenXmlToolCallAccumulatorTest, ExplicitlyOpenRootObjectIsNotGuided) {
  const auto tools = R"([{"type":"function","function":{"name":"search",)"
                     R"("parameters":{"type":"object","properties":{"pattern":{"type":"string"}},)"
                     R"("additionalProperties":true}}}])";
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"search", ToolKind::kFunction}};

  EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(tools, kinds));
  EXPECT_TRUE(ParseQwenGuidedToolCalls(
      R"([{"name":"search","parameters":{"pattern":"TODO","unknown":"src"}}])", tools, kinds).empty());
  ExpectSchemaFailureWithoutCalls(
      {"<tool_call>\n<function=search>\n<parameter=pattern>\nTODO\n</parameter>\n</function>\n</tool_call>"},
      tools, kinds);
}

TEST(QwenXmlToolCallAccumulatorTest, SharedReferenceExpansionHasAnAggregateBudget) {
  nlohmann::json parameters = {
      {"type", "object"},
      {"properties", nlohmann::json::object()},
      {"$defs", {{"large", {{"type", "string"}, {"title", std::string(1024 * 1024, 'x')}}}}},
  };
  for (size_t index = 0; index < 9; ++index) {
    parameters["properties"]["field" + std::to_string(index)] = {{"$ref", "#/$defs/large"}};
  }

  const auto tools = nlohmann::json::array({
      {{"type", "function"}, {"function", {{"name", "large"}, {"parameters", parameters}}}}
  }).dump();
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"large", ToolKind::kFunction}};

  EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(tools, kinds));
}

TEST(QwenXmlToolCallAccumulatorTest, OneOfAndUnresolvedReferencesKeepWholeSetIneligible) {
  for (const auto* definition : {
           R"({"oneOf":[{"type":"string"},{"type":"integer"}]})",
           R"({"$ref":"#/$defs/missing"})",
           R"({"$ref":"https://example.com/external"})"}) {
    const auto tools = std::string(R"([{"type":"function","function":{"name":"unsupported",)"
                                   R"("parameters":{"type":"object","properties":{"value":)") +
                       definition + R"(}}}}])";
    const auto kinds = std::unordered_map<std::string, ToolKind>{{"unsupported", ToolKind::kFunction}};
    EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(tools, kinds)) << definition;
    EXPECT_TRUE(ParseQwenGuidedToolCalls(
        R"([{"name":"unsupported","parameters":{"value":"text"}}])", tools, kinds).empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, ScopedPlanningRequiresNativeAutomaticEngineAndAuthoritativeMarkers) {
  ToolCallContext context;
  context.tool_output = true;
  context.text_output = true;
  context.tool_call_start = "<tool_call>";
  context.tool_call_end = "</tool_call>";
  context.tool_call_start_token_id = 248058;
  context.tool_call_end_token_id = 248059;
  context.tools_json = R"([{"type":"function","function":{"name":"grep","parameters":{"type":"object",)"
                       R"("properties":{"paths":{"type":"array","items":{"type":"string"}}},"required":["paths"]}}}])";
  context.tool_kinds.emplace("grep", ToolKind::kFunction);
  const auto plan = [&] {
    return PlanQwenXmlToolBodyGuidance(context, true, ChatBackendKind::kEngine);
  };
  EXPECT_TRUE(plan().has_value());
  EXPECT_FALSE(PlanQwenXmlToolBodyGuidance(context, false, ChatBackendKind::kEngine));
  EXPECT_FALSE(PlanQwenXmlToolBodyGuidance(context, true, ChatBackendKind::kGenerator));
  context.guidance_type = "json_schema";
  context.guidance_data = "{}";
  EXPECT_FALSE(plan());
  context.guidance_type.clear();
  context.guidance_data.clear();
  context.forced_tool = ForcedToolChoice{"grep", ToolKind::kFunction};
  EXPECT_FALSE(plan());
  context.forced_tool.reset();
  context.text_output = false;
  EXPECT_FALSE(plan());
  context.text_output = true;
  context.tool_call_start_token_id.reset();
  EXPECT_FALSE(plan());
  context.tool_call_start_token_id = 248058;
  context.tool_call_end_token_id.reset();
  EXPECT_FALSE(plan());
  context.tool_call_end_token_id = 248059;
  context.tool_call_start = "<custom_tool_call>";
  EXPECT_FALSE(plan());
  context.tool_call_start = "<tool_call>";
  context.tool_call_end = "</custom_tool_call>";
  EXPECT_FALSE(plan());
  context.tool_call_end = "</tool_call>";
  context.tool_call_start_token_id = -1;
  EXPECT_FALSE(plan());
  context.tool_call_start_token_id = 248058;
  context.tool_call_start_token_id = context.tool_call_end_token_id;
  EXPECT_FALSE(plan());
}

TEST(QwenXmlToolCallAccumulatorTest, UnrepresentableWholeSetFallsBackAndHidesUnsupportedXml) {
  const std::string tools = R"([
      {"type":"function","function":{"name":"grep","parameters":{"type":"object",
        "properties":{"paths":{"type":"array","items":{"type":"string"}}},"required":["paths"]}}},
      {"type":"function","function":{"name":"other","parameters":{"type":"object",
        "properties":{"value":{"type":"string","pattern":"^[a-z]+$"}}}}}
    ])";
  const std::unordered_map<std::string, ToolKind> kinds{
      {"grep", ToolKind::kFunction}, {"other", ToolKind::kFunction}};
  EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(tools, kinds).has_value());
  const std::string only_unsupported =
      R"([{"type":"function","function":{"name":"other","parameters":{"type":"object",)"
      R"("properties":{"value":{"type":"string"}},"required":"value"}}}])";
  const std::unordered_map<std::string, ToolKind> other_kind{{"other", ToolKind::kFunction}};
  EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(only_unsupported, other_kind).has_value());
  auto accumulator = MakeQwenAccumulator(only_unsupported, other_kind);
  auto outputs = RunChunks(accumulator, {"visible ", "<tool_call>\n<function=other>\n",
                                         "<parameter=value>\nsecret"});
  EXPECT_TRUE(AnyMalformed(outputs));
  EXPECT_EQ(CollectVisible(outputs), "visible ");
  EXPECT_TRUE(CollectCalls(outputs).empty());
}

TEST(QwenXmlToolCallAccumulatorTest, CopilotToolsDecodeSupportedCallsDespiteOtherUnsupportedSchemas) {
  const auto tools = nlohmann::json::array({
      {{"type", "function"},
       {"function",
        {{"name", "powershell"},
         {"parameters",
          {{"type", "object"},
           {"properties", {{"command", {{"type", "string"}}}, {"description", {{"type", "string"}}}}},
           {"required", nlohmann::json::array({"command", "description"})}}}}}},
      {{"type", "function"},
       {"function",
        {{"name", "github-mcp-server-issue_read"},
         {"parameters",
          {{"type", "object"},
           {"properties",
            {{"method", {{"type", "string"}, {"enum", nlohmann::json::array({"get", "get_comments"})}}},
             {"owner", {{"type", "string"}, {"x-mcp-header", "owner"}}},
             {"repo", {{"type", "string"}, {"x-mcp-header", "repo"}}},
             {"issue_number", {{"type", "number"}}}}},
           {"required", nlohmann::json::array({"method", "owner", "repo", "issue_number"})}}}}}},
      {{"type", "function"},
       {"function",
        {{"name", "unsupported"},
         {"parameters",
          {{"type", "object"},
           {"properties", {{"input", {{"type", "string"}, {"pattern", "^safe$"}}}}}}}}}},
  }).dump();
  const std::unordered_map<std::string, ToolKind> kinds = {
      {"powershell", ToolKind::kFunction},
      {"github-mcp-server-issue_read", ToolKind::kFunction},
      {"unsupported", ToolKind::kFunction},
  };
  ASSERT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools, kinds, true)));

  const std::string issue_call =
      "<tool_call>\n<function=github-mcp-server-issue_read>\n"
      "<parameter=method>\nget\n</parameter>\n"
      "<parameter=owner>\nmicrosoft\n</parameter>\n"
      "<parameter=repo>\nonnxruntime-genai\n</parameter>\n"
      "<parameter=issue_number>\n2653\n</parameter>\n"
      "</function>\n</tool_call>";
  const auto accepted = RunQwen({issue_call}, tools, kinds);
  EXPECT_TRUE(accepted.visible.empty());
  ASSERT_EQ(accepted.calls.size(), 1u);
  EXPECT_EQ(accepted.calls.front().name, "github-mcp-server-issue_read");
  EXPECT_EQ(nlohmann::json::parse(accepted.calls.front().arguments),
            nlohmann::json({{"method", "get"}, {"owner", "microsoft"}, {"repo", "onnxruntime-genai"},
                            {"issue_number", 2653}}));

  const std::string unsupported_call =
      "<tool_call>\n<function=unsupported>\n<parameter=input>\n{\"value\":\"x\"}\n</parameter>\n"
      "</function>\n</tool_call>";
  ExpectSchemaFailureWithoutCalls({unsupported_call}, tools, kinds);
  ExpectSchemaFailureWithoutCalls({issue_call + unsupported_call}, tools, kinds);
}

TEST(QwenXmlToolCallAccumulatorTest, CopilotGrepPathMismatchWithValidSiblingFailsClosed) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"grep","parameters":{"type":"object","properties":{)"
      R"("pattern":{"type":"string"},"paths":{"anyOf":[{"type":"string"},{"type":"array","items":{"type":"string"}}]},)"
      R"("-n":{"type":"boolean"}},"required":["pattern"]}}},)"
      R"({"type":"function","function":{"name":"powershell","parameters":{"type":"object","properties":{)"
      R"("command":{"type":"string"},"description":{"type":"string"}},"required":["command","description"]}}}])";
  const std::unordered_map<std::string, ToolKind> kinds = {
      {"grep", ToolKind::kFunction}, {"powershell", ToolKind::kFunction}};
  const std::string grep_prefix =
      "<tool_call>\n<function=grep>\n<parameter=-n>\ntrue\n</parameter>\n"
      "<parameter=pattern>\njson\n</parameter>\n";
  const std::string grep_suffix = "</function>\n</tool_call>";
  const std::string powershell =
      "\n<tool_call>\n<function=powershell>\n<parameter=command>\nGet-ChildItem\n</parameter>\n"
      "<parameter=description>\nList files\n</parameter>\n</function>\n</tool_call>";
  const std::string invalid = grep_prefix + "<parameter=path>\nsrc\n</parameter>\n" +
                              grep_suffix + powershell;
  const std::string valid = grep_prefix + "<parameter=paths>\nsrc\n</parameter>\n" +
                            grep_suffix + powershell;

  for (const bool recovery_aware : {false, true}) {
    for (const auto& chunks : std::vector<std::vector<std::string>>{
             {invalid}, SplitAt(invalid, invalid.find(powershell)), SplitIntoBytes(invalid)}) {
      auto accumulator = MakeQwenAccumulator(tools, kinds, recovery_aware);
      auto outputs = RunChunks(accumulator, chunks);
      EXPECT_TRUE(AnyMalformed(outputs));
      EXPECT_TRUE(CollectVisible(outputs).empty());
      EXPECT_TRUE(CollectCalls(outputs).empty());
    }
  }

  auto valid_output = RunQwen({valid}, tools, kinds);
  EXPECT_TRUE(valid_output.visible.empty());
  ASSERT_EQ(valid_output.calls.size(), 2u);
  EXPECT_EQ(nlohmann::json::parse(valid_output.calls.front().arguments).at("paths"), "src");
}

TEST(QwenXmlToolCallAccumulatorTest, InterruptedKnownInvalidPrefixesFailClosedWithoutClosingMarker) {
  const auto unsupported_tools =
      R"([{"type":"function","function":{"name":"unsupported","parameters":{"type":"array","items":{"type":"string"}}}}])";
  const std::vector<std::pair<std::string, std::string>> invalid = {
      {"<tool_call>\n<function=typed>\n<parameter=path>\nsrc\n", kQwenTools},
      {"<tool_call>\n<function=unsupported>\n<parameter=value>\nsrc\n", unsupported_tools},
  };

  for (const auto& [candidate, tools] : invalid) {
    for (const auto& chunks : std::vector<std::vector<std::string>>{{candidate}, SplitIntoBytes(candidate)}) {
      const auto kinds = tools == kQwenTools
                             ? kQwenToolKinds
                             : std::unordered_map<std::string, ToolKind>{{"unsupported", ToolKind::kFunction}};
      auto accumulator = MakeQwenAccumulator(tools, kinds, /*recovery_aware=*/true);
      ToolCallStreamAccumulator::Output interrupted;
      for (const auto& chunk : chunks) {
        auto output = accumulator.Push(chunk);
        EXPECT_TRUE(output.events.empty());
      }
      interrupted = accumulator.RejectPendingSelectedPayload();
      EXPECT_TRUE(interrupted.malformed) << candidate;
      EXPECT_TRUE(interrupted.events.empty()) << candidate;
    }
  }

  const std::string ambiguous = "<tool_call>\n<function=typed>\n<parameter=text>\ntruncated";
  auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, /*recovery_aware=*/true);
  EXPECT_FALSE(accumulator.Push(ambiguous).malformed);
  const auto interrupted = accumulator.RejectPendingSelectedPayload();
  EXPECT_FALSE(interrupted.malformed);
  ASSERT_EQ(interrupted.events.size(), 1u);
  EXPECT_EQ(std::get<std::string>(interrupted.events.front()), ambiguous);
}

TEST(QwenXmlToolCallAccumulatorTest, InvalidSiblingAfterUnknownOrRejectedCallFailsClosed) {
  const std::string unknown = "<tool_call>\n<function=missing>\n</function>\n</tool_call>";
  const std::string unclosed_function = "<tool_call>\n<function=missing>\n</tool_call>";
  const std::string rejected = "<tool_call>\n<function=typed>\n<param=text>\nx\n</param>\n"
                               "</function>\n</tool_call>";
  const std::string invalid = "<tool_call>\n<function=typed>\n<parameter=path>\nsrc\n</parameter>\n"
                              "</function>\n</tool_call>";

  for (const auto& first : {unknown, unclosed_function, rejected}) {
    for (const auto& generated : {first + invalid, invalid + first}) {
      for (const auto& chunks : std::vector<std::vector<std::string>>{{generated}, SplitIntoBytes(generated)}) {
        ExpectSchemaFailureWithoutCalls(chunks);
      }
    }
  }

  const auto visible = RunQwen({unknown + kValidZeroQwenCall});
  EXPECT_EQ(visible.visible, unknown + kValidZeroQwenCall);
  EXPECT_TRUE(visible.calls.empty());

  const std::string literal = "<tool_call>\n<function=missing>\n<parameter=text>\n"
                              "literal </tool_call> and <tool_call>\n<function=typed>\n"
                              "<parameter=path>\nsrc\n</parameter> not a sibling\n"
                              "</parameter>\n</function>\n</tool_call>";
  const std::string literal_outer_end = "<tool_call>\n<function=missing>\n<parameter=x>\n"
                                        "literal </function>\n</tool_call> within the body\n"
                                        "</parameter>\n</function>\n</tool_call>";
  const std::string literal_outer_end_and_start =
      "<tool_call>\n<function=missing>\n<parameter=x>\n"
      "literal </function>\n</tool_call> and <tool_call> not a sibling\n"
      "</parameter>\n</function>\n</tool_call>";
  const std::string literal_outer_end_and_line_start =
      "<tool_call>\n<function=missing>\n<parameter=x>\n"
      "literal </function>\n</tool_call> and <tool_call>\nnot a sibling\n"
      "</parameter>\n</function>\n</tool_call>";
  const std::string literal_sibling_header =
      "<tool_call>\n<function=missing>\n<parameter=x>\n"
      "literal </tool_call>\n<tool_call>\n<function=typed>\nnot a sibling\n"
      "</parameter>\n</function>\n</tool_call>";
  const std::string literal_combined_close_and_sibling_header =
      "<tool_call>\n<function=missing>\n<parameter=x>\n"
      "literal </function>\n</tool_call>\n<tool_call>\n<function=typed>\n"
      "not a sibling\n</parameter>\n</function>\n</tool_call>";
  const std::string literal_multiple_combined_closes =
      "<tool_call>\n<function=missing>\n<parameter=x>\n"
      "literal </function>\n</tool_call> not a sibling\n"
      "literal </function>\n</tool_call>\n<tool_call>\n<function=typed>\n"
      "not a sibling\n</parameter>\n</function>\n</tool_call>";
  for (const auto& candidate : {literal, literal_outer_end, literal_outer_end_and_start,
                                literal_outer_end_and_line_start, literal_sibling_header,
                                literal_combined_close_and_sibling_header,
                                literal_multiple_combined_closes}) {
    SCOPED_TRACE(candidate);
    for (const bool recovery_aware : {false, true}) {
      for (const auto& chunks : std::vector<std::vector<std::string>>{
               {candidate}, SplitIntoBytes(candidate)}) {
        auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
        auto outputs = RunChunks(accumulator, chunks);
        EXPECT_FALSE(AnyMalformed(outputs)) << candidate;
        EXPECT_EQ(CollectVisible(outputs), candidate) << candidate;
        EXPECT_TRUE(CollectCalls(outputs).empty()) << candidate;
      }
      for (size_t split = 0; split <= candidate.size(); ++split) {
        auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
        auto outputs = RunChunks(accumulator, SplitAt(candidate, split));
        EXPECT_FALSE(AnyMalformed(outputs)) << "split=" << split;
        EXPECT_EQ(CollectVisible(outputs), candidate) << "split=" << split;
        EXPECT_TRUE(CollectCalls(outputs).empty()) << "split=" << split;
      }
    }
  }
}

TEST(QwenXmlToolCallAccumulatorTest, UnclosedRejectedParameterCannotConsumeInvalidSibling) {
  const std::string unknown =
      "<tool_call>\n<function=missing>\n<parameter=x>\nbody\n"
      "</function>\n</tool_call>";
  const std::string unknown_with_literal_candidate =
      "<tool_call>\n<function=missing>\n<parameter=x>\nbody\n"
      "</function>\n</tool_call> and <tool_call>\nnot a sibling";
  const std::string invalid =
      "<tool_call>\n<function=typed>\n<parameter=path>\nsrc\n"
      "</parameter>\n</function>\n</tool_call>";

  for (const auto& generated : {unknown + "\n" + invalid, invalid + "\n" + unknown,
                                unknown_with_literal_candidate + "\n" + invalid}) {
    for (const bool recovery_aware : {false, true}) {
      {
        auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
        auto outputs = RunChunks(accumulator, SplitIntoBytes(generated));
        EXPECT_TRUE(AnyMalformed(outputs));
        EXPECT_TRUE(CollectVisible(outputs).empty());
        EXPECT_TRUE(CollectCalls(outputs).empty());
      }
      for (size_t split = 0; split <= generated.size(); ++split) {
        auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
        auto outputs = RunChunks(accumulator, SplitAt(generated, split));
        EXPECT_TRUE(AnyMalformed(outputs)) << "split=" << split;
        EXPECT_TRUE(CollectVisible(outputs).empty()) << "split=" << split;
        EXPECT_TRUE(CollectCalls(outputs).empty()) << "split=" << split;
      }
    }
  }
}

TEST(QwenXmlToolCallAccumulatorTest, MissingFunctionCloseCannotConsumeInvalidSibling) {
  const std::string unknown =
      "<tool_call>\n<function=missing>\n<parameter=x>\nbody\n</tool_call>";
  const std::string unknown_with_literal_end =
      "<tool_call>\n<function=missing>\n<parameter=x>\n"
      "literal </tool_call> not a sibling\n</tool_call>";
  const std::string unknown_without_tool_close =
      "<tool_call>\n<function=missing>\n<parameter=x>\nbody\n</function>\n";
  const std::string unknown_with_closed_parameter =
      "<tool_call>\n<function=missing>\n<parameter=x>\nbody\n</parameter>\n";
  const std::string invalid =
      "<tool_call>\n<function=typed>\n<parameter=path>\nsrc\n"
      "</parameter>\n</function>\n</tool_call>";
  const std::string missing_required =
      "<tool_call>\n<function=typed>\n</function>\n</tool_call>";

  for (const auto& first : {unknown, unknown_with_literal_end,
                            unknown_without_tool_close, unknown_with_closed_parameter}) {
    for (const auto& sibling : {invalid, missing_required}) {
      for (const auto& generated : {first + "\n" + sibling, sibling + "\n" + first}) {
        SCOPED_TRACE(generated);
        for (const bool recovery_aware : {false, true}) {
          const auto parsed =
              CreateQwenXmlToolCallPayloadParser(kQwenTools, kQwenToolKinds, recovery_aware)(generated, true);
          EXPECT_EQ(parsed.disposition, ToolCallPayloadDisposition::kMalformed);
          for (const auto& chunks : std::vector<std::vector<std::string>>{
                   {generated}, SplitIntoBytes(generated)}) {
            auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
            auto outputs = RunChunks(accumulator, chunks);
            EXPECT_TRUE(AnyMalformed(outputs));
            EXPECT_TRUE(CollectVisible(outputs).empty());
            EXPECT_TRUE(CollectCalls(outputs).empty());
          }
          for (size_t split = 0; split <= generated.size(); ++split) {
            SCOPED_TRACE(split);
            auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
            auto outputs = RunChunks(accumulator, SplitAt(generated, split));
            EXPECT_TRUE(AnyMalformed(outputs));
            EXPECT_TRUE(CollectVisible(outputs).empty());
            EXPECT_TRUE(CollectCalls(outputs).empty());
          }
        }
      }
    }
  }
}

TEST(QwenXmlToolCallAccumulatorTest, AmbiguousNestedCallMarkupFailsClosed) {
  const std::string ambiguous =
      "<tool_call>\n<function=missing>\n<parameter=x>\n"
      "literal </tool_call>\n<tool_call>\n<function=typed>\n"
      "<parameter=path>\nsrc\n</parameter> not a sibling\n"
      "</parameter>\n</function>\n</tool_call>";

  for (const bool recovery_aware : {false, true}) {
    for (const auto& chunks : std::vector<std::vector<std::string>>{
             {ambiguous}, SplitIntoBytes(ambiguous)}) {
      auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
      auto outputs = RunChunks(accumulator, chunks);
      EXPECT_TRUE(AnyMalformed(outputs));
      EXPECT_TRUE(CollectVisible(outputs).empty());
      EXPECT_TRUE(CollectCalls(outputs).empty());
    }
    for (size_t split = 0; split <= ambiguous.size(); ++split) {
      SCOPED_TRACE(split);
      auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
      auto outputs = RunChunks(accumulator, SplitAt(ambiguous, split));
      EXPECT_TRUE(AnyMalformed(outputs));
      EXPECT_TRUE(CollectVisible(outputs).empty());
      EXPECT_TRUE(CollectCalls(outputs).empty());
    }
  }
}

TEST(QwenXmlToolCallAccumulatorTest, RejectedBodyCannotConsumeSiblingMissingToolClose) {
  const std::string generated =
      "<tool_call>\n<function=missing>\n<parameter=x>\nbody\n"
      "</function>\n</tool_call>\n"
      "<tool_call>\n<function=typed>\n</function>\n";

  for (const bool recovery_aware : {false, true}) {
    for (const auto& chunks : std::vector<std::vector<std::string>>{
             {generated}, SplitIntoBytes(generated)}) {
      auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
      auto outputs = RunChunks(accumulator, chunks);
      EXPECT_TRUE(AnyMalformed(outputs));
      EXPECT_TRUE(CollectVisible(outputs).empty());
      EXPECT_TRUE(CollectCalls(outputs).empty());
    }

    for (size_t split = 0; split <= generated.size(); ++split) {
      SCOPED_TRACE(split);
      auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
      auto outputs = RunChunks(accumulator, SplitAt(generated, split));
      EXPECT_TRUE(AnyMalformed(outputs));
      EXPECT_TRUE(CollectVisible(outputs).empty());
      EXPECT_TRUE(CollectCalls(outputs).empty());
    }
  }
}

TEST(QwenXmlToolCallAccumulatorTest, IndependentValidCallSurvivesLaterInvalidBatchAcrossSplits) {
  const std::string invalid =
      "<tool_call>\n<function=typed>\n<parameter=path>\nsrc\n"
      "</parameter>\n</function>\n</tool_call>";
  const std::string prose = "\nordinary prose\n";
  const std::string generated = kValidZeroQwenCall + prose + invalid + " tail";
  for (const bool recovery_aware : {false, true}) {
    {
      auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
      auto outputs = RunChunks(accumulator, SplitIntoBytes(generated));
      EXPECT_TRUE(AnyMalformed(outputs));
      EXPECT_EQ(CollectVisible(outputs), prose);
      auto calls = CollectCalls(outputs);
      ASSERT_EQ(calls.size(), 1u);
      EXPECT_EQ(calls.front().name, "zero");
    }
    for (size_t split = 0; split <= generated.size(); ++split) {
      auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
      auto outputs = RunChunks(accumulator, SplitAt(generated, split));
      EXPECT_TRUE(AnyMalformed(outputs)) << "split=" << split;
      EXPECT_EQ(CollectVisible(outputs), prose) << "split=" << split;
      auto calls = CollectCalls(outputs);
      ASSERT_EQ(calls.size(), 1u) << "split=" << split;
      EXPECT_EQ(calls.front().name, "zero") << "split=" << split;
    }

    auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, recovery_aware);
    auto adjacent = RunChunks(accumulator, SplitIntoBytes(kValidZeroQwenCall + "\n" + invalid));
    EXPECT_TRUE(AnyMalformed(adjacent));
    EXPECT_TRUE(CollectCalls(adjacent).empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, VisiblePrefixBeforeInvalidCallAndTailIsChunkInvariant) {
  const std::string invalid =
      "<tool_call>\n<function=typed>\n<parameter=path>\nsrc\n</parameter>\n</function>\n</tool_call>";
  const std::string generated = "safe prefix" + invalid + " tail";
  for (size_t split = 0; split <= generated.size(); ++split) {
    auto accumulator = MakeQwenAccumulator();
    auto outputs = RunChunks(accumulator, SplitAt(generated, split));
    EXPECT_TRUE(AnyMalformed(outputs)) << "split=" << split;
    EXPECT_EQ(CollectVisible(outputs), "safe prefix") << "split=" << split;
    EXPECT_TRUE(CollectCalls(outputs).empty()) << "split=" << split;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, InvalidCallWithLiteralClosingMarkerCannotExposeTailOrAdjacentCall) {
  const auto unsupported_tools = nlohmann::json::array({
      {{"type", "function"},
       {"function",
        {{"name", "unsupported"},
         {"parameters", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}},
      {{"type", "function"},
       {"function", {{"name", "zero"}, {"parameters", {{"type", "object"},
                                                       {"properties", nlohmann::json::object()}}}}}},
  }).dump();
  const std::string unsupported =
      "<tool_call>\n<function=unsupported>\n<parameter=value>\n"
      "literal </tool_call> inside body\n</parameter>\n</function>\n</tool_call>";
  const std::string undeclared =
      "<tool_call>\n<function=typed>\n<parameter=unknown>\n"
      "literal </tool_call> inside body\n</parameter>\n</function>\n</tool_call>";
  const std::string valid_before_invalid =
      "<tool_call>\n<function=typed>\n<parameter=text>\n"
      "literal </tool_call> inside body\n</parameter>\n"
      "<parameter=unknown>\nlater\n</parameter>\n</function>\n</tool_call>";

  for (const auto& [call, tools, kinds] : std::vector<std::tuple<
           std::string, std::string, std::unordered_map<std::string, ToolKind>>>{
           {unsupported, unsupported_tools,
            {{"unsupported", ToolKind::kFunction}, {"zero", ToolKind::kFunction}}},
           {undeclared, kQwenTools, kQwenToolKinds},
           {valid_before_invalid, kQwenTools, kQwenToolKinds}}) {
    const std::string generated = call + "\n" + kValidZeroQwenCall + " visible tail";
    const auto literal_end = generated.find("</tool_call>");
    const auto outer_end = generated.find("</tool_call>", literal_end + 1);
    for (const auto& chunks : std::vector<std::vector<std::string>>{
             {generated}, SplitAt(generated, literal_end + 6),
             SplitAt(generated, outer_end), SplitIntoBytes(generated)}) {
      SCOPED_TRACE(call);
      ExpectSchemaFailureWithoutCalls(chunks, tools, kinds);
    }
  }
}

TEST(QwenXmlToolCallAccumulatorTest, AdditionalPropertiesTrueIsNotMistakenForInvalidArguments) {
  const auto tools = nlohmann::json::array(
                         {{{"type", "function"},
                           {"function",
                            {{"name", "fn"},
                             {"parameters",
                              {{"type", "object"},
                               {"properties", nlohmann::json::object()},
                               {"required", nlohmann::json::array({"dynamic"})},
                               {"additionalProperties", true}}}}}}})
                         .dump();

  const auto kinds = std::unordered_map<std::string, ToolKind>{{"fn", ToolKind::kFunction}};
  ASSERT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools, kinds)));
  const std::string call =
      "<tool_call>\n<function=fn>\n<parameter=dynamic>\ntext\n</parameter>\n</function>\n</tool_call>";
  ExpectSchemaFailureWithoutCalls({call}, tools, kinds);
  EXPECT_TRUE(ParseQwenGuidedToolCalls(
      R"([{"name":"fn","parameters":{"dynamic":"text"}}])", tools, kinds).empty());

  const auto open_schema =
      R"([{"type":"function","function":{"name":"fn","parameters":{"type":"object",)"
      R"("properties":{"fixed":{"type":"string"}},"additionalProperties":true}}}])";
  ExpectSchemaFailureWithoutCalls({call}, open_schema, kinds);
  EXPECT_TRUE(ParseQwenGuidedToolCalls(
      R"([{"name":"fn","parameters":{"dynamic":"text"}}])", open_schema, kinds).empty());
}

TEST(QwenXmlToolCallAccumulatorTest, UnsupportedOnlyNonObjectRootSchemasFailClosed) {
  const std::vector<nlohmann::json> parameter_schemas = {
      {{"type", "array"}, {"items", {{"type", "string"}}}},
      {{"type", nlohmann::json::array({"object", "null"})}},
      true,
  };
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"fn", ToolKind::kFunction}};
  const std::string call =
      "<tool_call>\n<function=fn>\n<parameter=dynamic>\ntext\n</parameter>\n</function>\n</tool_call>";

  for (const auto& parameters : parameter_schemas) {
    SCOPED_TRACE(parameters.dump());
    const auto tools = nlohmann::json::array(
                           {{{"type", "function"},
                             {"function", {{"name", "fn"}, {"parameters", parameters}}}}})
                           .dump();
    ASSERT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools, kinds)));
    ExpectSchemaFailureWithoutCalls({call}, tools, kinds);
    EXPECT_TRUE(ParseQwenGuidedToolCalls(
        R"([{"name":"fn","parameters":{"dynamic":"text"}}])", tools, kinds).empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, SkippedSerializedDeclarationsKeepFailClosedDecoderSelected) {
  const auto supported = nlohmann::json{
      {"type", "function"},
      {"function",
       {{"name", "fn"},
        {"parameters", {{"type", "object"}, {"properties", nlohmann::json::object()}}}}},
  };
  const auto custom_tools =
      nlohmann::json::array({supported, {{"type", "custom"}, {"name", "raw"}}}).dump();
  EXPECT_TRUE(static_cast<bool>(
      CreateQwenXmlToolCallPayloadParser(
          custom_tools, {{"fn", ToolKind::kFunction}, {"raw", ToolKind::kCustom}})));
  EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(
      custom_tools, {{"fn", ToolKind::kFunction}, {"raw", ToolKind::kCustom}}));
  ExpectSchemaFailureWithoutCalls(
      {"<tool_call>\n<function=fn>\n</function>\n</tool_call>"}, custom_tools,
      {{"fn", ToolKind::kFunction}, {"raw", ToolKind::kCustom}});

  const auto malformed_tools = nlohmann::json::array(
                                   {supported, {{"type", "function"}, {"function", {{"name", 1}}}}})
                                   .dump();
  EXPECT_TRUE(static_cast<bool>(
      CreateQwenXmlToolCallPayloadParser(malformed_tools, {{"fn", ToolKind::kFunction}})));
  EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(malformed_tools, {{"fn", ToolKind::kFunction}}));
  ExpectSchemaFailureWithoutCalls(
      {"<tool_call>\n<function=fn>\n</function>\n</tool_call>"}, malformed_tools,
      {{"fn", ToolKind::kFunction}});
}

TEST(QwenXmlToolCallAccumulatorTest, UnrecognizableOfferedToolCannotBecomeGuidedSuccessOrVisibleXml) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"known","parameters":{"type":"object","properties":{}}}},)"
      R"({"type":"function","function":{"name":"undecodable","parameters":{"type":"object","properties":{}}}}])";
  const auto kinds = std::unordered_map<std::string, ToolKind>{
      {"known", ToolKind::kFunction}, {"undecodable", ToolKind::kCustom}};
  const auto undecodable = "<tool_call>\n<function=undecodable>\n</function>\n</tool_call>";

  ASSERT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools, kinds)));
  EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(tools, kinds));
  ExpectSchemaFailureWithoutCalls({undecodable}, tools, kinds);
  EXPECT_TRUE(ParseQwenGuidedToolCalls(
      R"([{"name":"undecodable","parameters":{}}])", tools, kinds).empty());
  EXPECT_TRUE(ParseQwenGuidedToolCalls(R"([{"name":"known","parameters":{}}])", tools, kinds).empty());

  auto accumulator = MakeQwenAccumulator(tools, kinds);
  auto outputs = RunChunks(accumulator, {"visible ", undecodable});
  EXPECT_TRUE(AnyMalformed(outputs));
  EXPECT_EQ(CollectVisible(outputs), "visible ");
  EXPECT_TRUE(CollectCalls(outputs).empty());
}

TEST(QwenXmlToolCallAccumulatorTest, MalformedOfferedJsonNeverFallsBackToGenericXmlPassthrough) {
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"fn", ToolKind::kFunction}};
  const std::string call = "<tool_call>\n<function=fn>\n</function>\n</tool_call>";
  for (const auto& tools : {std::string("{invalid"), std::string("[]")}) {
    ASSERT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools, kinds)));
    EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(tools, kinds));
    ExpectSchemaFailureWithoutCalls({call}, tools, kinds);
    EXPECT_TRUE(ParseQwenGuidedToolCalls(R"([{"name":"fn"}])", tools, kinds).empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, NonNaturalFinalizationPreservesCompletePendingCallAsExactText) {
  auto acc = MakeQwenAccumulator();

  EXPECT_TRUE(acc.Push(kValidTypedQwenCall).events.empty());
  auto output = acc.RejectPendingSelectedPayload();
  std::vector<ToolCallStreamAccumulator::Output> outputs;
  outputs.push_back(std::move(output));

  EXPECT_EQ(CollectVisible(outputs), kValidTypedQwenCall);
  EXPECT_TRUE(CollectCalls(outputs).empty());
  EXPECT_FALSE(acc.InsideToolCall());
}

TEST(QwenXmlToolCallAccumulatorTest, InterruptedCompleteSchemaViolationCannotBecomeVisibleText) {
  const std::string invalid =
      "<tool_call>\n<function=typed>\n<parameter=unknown>\nvalue\n</parameter>\n"
      "</function>\n</tool_call>";

  for (size_t split = 0; split <= invalid.size(); ++split) {
    auto acc = MakeQwenAccumulator();
    auto chunks = SplitAt(invalid, split);
    std::vector<ToolCallStreamAccumulator::Output> outputs;
    for (const auto& chunk : chunks) {
      outputs.push_back(acc.Push(chunk));
    }
    outputs.push_back(acc.RejectPendingSelectedPayload());
    outputs.push_back(acc.Push(kValidTypedQwenCall + "tail"));
    outputs.push_back(acc.Flush());

    EXPECT_TRUE(AnyMalformed(outputs)) << "split=" << split;
    EXPECT_TRUE(CollectVisible(outputs).empty()) << "split=" << split;
    EXPECT_TRUE(CollectCalls(outputs).empty()) << "split=" << split;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, NonNaturalFinalizationKeepsAlreadyAdmittedCallTerminal) {
  auto acc = MakeQwenAccumulator();
  auto admitted = acc.Push(kValidTypedQwenCall + "tail");
  auto interrupted = acc.RejectPendingSelectedPayload();
  std::vector<ToolCallStreamAccumulator::Output> outputs;
  outputs.push_back(std::move(admitted));
  outputs.push_back(std::move(interrupted));

  auto calls = CollectCalls(outputs);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].arguments, R"({"text":"first"})");
  EXPECT_EQ(CollectVisible(outputs), "tail");
}

TEST(QwenXmlToolCallAccumulatorTest, EveryByteSplitProducesOneExactCall) {
  const std::string generated =
      "<tool_call>\n"
      "<function=typed>\n"
      "<parameter=text>\n"
      "split me\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";

  for (size_t split = 0; split <= generated.size(); ++split) {
    auto output = RunQwen(SplitAt(generated, split));
    ASSERT_EQ(output.calls.size(), 1u) << "split=" << split;
    EXPECT_EQ(output.calls[0].arguments, R"({"text":"split me"})") << "split=" << split;
    EXPECT_TRUE(output.visible.empty()) << "split=" << split;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, ByteAtATimeProducesOneExactCall) {
  const std::string generated =
      "<tool_call>\n"
      "<function=zero>\n"
      "</function>\n"
      "</tool_call>";
  auto output = RunQwen(SplitIntoBytes(generated));
  ASSERT_EQ(output.calls.size(), 1u);
  EXPECT_EQ(output.calls[0].name, "zero");
  EXPECT_EQ(output.calls[0].arguments, "{}");
  EXPECT_TRUE(output.visible.empty());
}

TEST(QwenXmlToolCallAccumulatorTest, AdjacentCallsAreCommittedAsOneAtomicBatch) {
  const std::string generated =
      "<tool_call>\n"
      "<function=typed>\n"
      "<parameter=text>\n"
      "first\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call> \n\t"
      "<tool_call>\n"
      "<function=typed>\n"
      "<parameter=text>\n"
      "second\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";
  auto acc = MakeQwenAccumulator();

  EXPECT_TRUE(acc.Push(generated).events.empty());
  auto output = acc.Push("tail");

  ASSERT_EQ(output.events.size(), 3u);
  EXPECT_EQ(std::get<ParsedToolCall>(output.events[0]).arguments, R"({"text":"first"})");
  EXPECT_EQ(std::get<ParsedToolCall>(output.events[1]).arguments, R"({"text":"second"})");
  EXPECT_EQ(std::get<std::string>(output.events[2]), "tail");
}

TEST(QwenXmlToolCallAccumulatorTest, SchemaTypesAreDecodedAsCompatibleJson) {
  const std::string generated =
      "<tool_call>\n"
      "<function=typed>\n"
      "<parameter=text>\n"
      "{\"looks\":\"json\"}\n"
      "</parameter>\n"
      "<parameter=number>\n"
      "1.5\n"
      "</parameter>\n"
      "<parameter=integer>\n"
      "-2\n"
      "</parameter>\n"
      "<parameter=boolean>\n"
      "true\n"
      "</parameter>\n"
      "<parameter=array>\n"
      "[1,\"two\"]\n"
      "</parameter>\n"
      "<parameter=object>\n"
      "{\"key\":3}\n"
      "</parameter>\n"
      "<parameter=nothing>\n"
      "null\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";
  auto output = RunQwen({generated});
  ASSERT_EQ(output.calls.size(), 1u);
  const std::string expected =
      "{\"array\":[1,\"two\"],\"boolean\":true,\"integer\":-2,\"nothing\":null,\"number\":1.5,"
      "\"object\":{\"key\":3},\"text\":\"{\\\"looks\\\":\\\"json\\\"}\"}";
  EXPECT_EQ(output.calls[0].arguments, expected);
  EXPECT_TRUE(output.visible.empty());
}

TEST(QwenXmlToolCallAccumulatorTest, PythonBooleanLiteralsDecodeWithoutAcceptingOtherNonJsonValues) {
  for (const auto& [body, expected] : std::vector<std::pair<std::string, bool>>{
           {"True", true}, {"False", false}}) {
    const auto output = RunQwen({
        "<tool_call>\n<function=typed>\n"
        "<parameter=text>\nhello\n</parameter>\n"
        "<parameter=boolean>\n" + body + "\n</parameter>\n"
        "</function>\n</tool_call>",
    });
    EXPECT_TRUE(output.visible.empty());
    ASSERT_EQ(output.calls.size(), 1u);
    EXPECT_EQ(nlohmann::json::parse(output.calls.front().arguments).at("boolean"), expected);
  }

  const std::string unsupported =
      "<tool_call>\n<function=typed>\n<parameter=text>\nhello\n</parameter>\n"
      "<parameter=boolean>\nTRUE\n</parameter>\n</function>\n</tool_call>";
  ExpectSchemaFailureWithoutCalls({unsupported});
}

TEST(QwenXmlToolCallAccumulatorTest, MultilineAndEmptyStringsPreserveExactBodies) {
  const std::string generated =
      "<tool_call>\n"
      "<function=typed>\n"
      "<parameter=text>\n"
      "line one\nline two\n"
      "</parameter>\n"
      "<parameter=empty>\n"
      "\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";
  auto output = RunQwen({generated});
  ASSERT_EQ(output.calls.size(), 1u);
  EXPECT_EQ(output.calls[0].arguments, R"({"empty":"","text":"line one\nline two"})");
}

TEST(QwenXmlToolCallAccumulatorTest, RawAmpersandsRemainExactStringValues) {
  const std::vector<std::string> values = {
      "a&b",
      "https://example.test/search?a=1&b=2",
      "cmd1 && cmd2",
  };

  for (const auto& value : values) {
    const auto generated = "<tool_call>\n<function=typed>\n<parameter=text>\n" + value +
                           "\n</parameter>\n</function>\n</tool_call>";
    auto output = RunQwen({generated});
    ASSERT_EQ(output.calls.size(), 1u) << value;
    EXPECT_EQ(nlohmann::json::parse(output.calls[0].arguments)["text"], value);
    EXPECT_TRUE(output.visible.empty()) << value;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, EditSourceComparisonOperatorsRemainExactStringValues) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"edit","parameters":{"type":"object","properties":{)"
      R"("path":{"type":"string"},"old_str":{"type":"string"},"new_str":{"type":"string"}},)"
      R"("required":["path","old_str","new_str"]}}}])";
  const std::string new_str =
      "if end < start:\n"
      "    return value <= limit\n"
      "mask = value << 1";
  const auto generated =
      "<tool_call>\n"
      "<function=edit>\n"
      "<parameter=path>\n"
      "src/range_math.py\n"
      "</parameter>\n"
      "<parameter=old_str>\n"
      "return value\n"
      "</parameter>\n"
      "<parameter=new_str>\n" +
      new_str +
      "\n</parameter>\n"
      "</function>\n"
      "</tool_call>";
  auto output = RunQwen({generated}, tools, {{"edit", ToolKind::kFunction}});
  ASSERT_EQ(output.calls.size(), 1u);
  const auto arguments = nlohmann::json::parse(output.calls.front().arguments);
  EXPECT_EQ(arguments["path"], "src/range_math.py");
  EXPECT_EQ(arguments["old_str"], "return value");
  EXPECT_EQ(arguments["new_str"], new_str);
  EXPECT_TRUE(output.visible.empty());
}

TEST(QwenXmlToolCallAccumulatorTest, ReservedNestedQwenMarkupFailsClosed) {
  const std::string function_tools =
      R"([{"type":"function","function":{"name":"edit","parameters":{"type":"object","properties":{)"
      R"("new_str":{"type":"string"}},"required":["new_str"]}}}])";
  const std::string custom_tools =
      R"([{"type":"function","function":{"name":"apply_patch","parameters":{"type":"object",)"
      R"("properties":{"input":{"type":"string"}},"required":["input"],"additionalProperties":false}}}])";
  const std::vector<std::tuple<std::string, std::unordered_map<std::string, ToolKind>,
                               std::string, std::string, std::string>>
      cases = {
          {function_tools, {{"edit", ToolKind::kFunction}}, "edit", "new_str", "before <parameter=path> after"},
          {custom_tools, {{"apply_patch", ToolKind::kCustom}}, "apply_patch", "input", "before <tool_call> after"},
      };

  for (const auto& [tools, kinds, function, parameter, body] : cases) {
    const auto generated = "<tool_call>\n<function=" + function + ">\n<parameter=" + parameter +
                           ">\n" + body + "\n</parameter>\n</function>\n</tool_call>";
    SCOPED_TRACE(body);
    ExpectSchemaFailureWithoutCalls({generated}, tools, kinds);
  }
}

TEST(QwenXmlToolCallAccumulatorTest, InvalidSchemaValuesFailClosed) {
  const std::vector<std::string> generated = {
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=integer>\n1.5\n</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=boolean>\n\"true\"\n</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=array>\n{}\n</parameter>\n</function>\n</tool_call>",
  };

  for (const auto& candidate : generated) {
    ExpectSchemaFailureWithoutCalls({candidate});
  }
}

TEST(QwenXmlToolCallAccumulatorTest, InvalidNamesRemainVisibleButInvalidParametersFailClosed) {
  const std::vector<std::string> generated = {
      "<tool_call>\n<function=missing>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\na\n</parameter>\n"
      "<parameter=text>\nb\n</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\na\n</parameter>\n"
      "<parameter=unknown>\nb\n</parameter>\n</function>\n</tool_call>",
  };

  for (size_t index = 0; index < generated.size(); ++index) {
    if (index == 0) {
      auto output = RunQwen({generated[index]});
      EXPECT_EQ(output.visible, generated[index]);
      EXPECT_TRUE(output.calls.empty());
    } else {
      ExpectSchemaFailureWithoutCalls({generated[index]});
    }
  }
}

TEST(QwenXmlToolCallAccumulatorTest, UndeclaredSecondCallRejectsEntireAdjacentBatch) {
  const std::string generated =
      "<tool_call>\n<function=zero>\n</function>\n</tool_call>\n"
      "<tool_call>\n<function=missing>\n</function>\n</tool_call>";
  auto output = RunQwen({generated});
  EXPECT_EQ(output.visible, generated);
  EXPECT_TRUE(output.calls.empty());
}

TEST(QwenXmlToolCallAccumulatorTest, UnsupportedAndAmbiguousSchemasFailClosed) {
  const std::vector<std::string> schemas = {
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"oneOf":[{"type":"string"},{"type":"null"}]}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"date"}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"array","items":{"type":"string"},"maxItems":1}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"object","properties":1}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"object","properties":{"nested":{)"
      R"("oneOf":[{"type":"string"},{"type":"integer"}]}}}}}}}])",
  };
  const std::string generated =
      "<tool_call>\n<function=bad>\n<parameter=value>\ntext\n</parameter>\n</function>\n</tool_call>";

  for (const auto& schema : schemas) {
    ExpectSchemaFailureWithoutCalls({generated}, schema, {{"bad", ToolKind::kFunction}});
  }
}

TEST(QwenXmlToolCallAccumulatorTest, ScalarEnumsAcceptOnlyDeclaredValues) {
  struct EnumCase {
    nlohmann::json schema;
    std::string accepted;
    std::string rejected;
  };
  const std::vector<EnumCase> cases = {
      {{{"type", "string"}, {"enum", {"fast", "thorough"}}}, "fast", "unsupported"},
      {{{"type", "number"}, {"enum", {1, 9007199254740993ULL}}}, "9007199254740993", "2"},
      {{{"type", "integer"}, {"enum", {-2, 3}}}, "-2", "1"},
      {{{"type", "boolean"}, {"enum", {true}}}, "true", "false"},
      {{{"type", "null"}, {"enum", {nullptr}}}, "null", "0"},
  };
  const auto make_call = [](std::string_view value) {
    return "<tool_call>\n<function=select>\n<parameter=value>\n" + std::string(value) +
           "\n</parameter>\n</function>\n</tool_call>";
  };

  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.schema.dump());
    const auto tools =
        nlohmann::json::array(
            {{{"type", "function"},
              {"function",
               {{"name", "select"},
                {"parameters",
                 {{"type", "object"},
                  {"properties", {{"value", test_case.schema}}},
                  {"required", {"value"}}}}}}}})
            .dump();

    auto accepted = RunQwen({make_call(test_case.accepted)}, tools, {{"select", ToolKind::kFunction}});
    ASSERT_EQ(accepted.calls.size(), 1u);
    EXPECT_TRUE(accepted.visible.empty());

    const auto rejected_call = make_call(test_case.rejected);
    ExpectSchemaFailureWithoutCalls({rejected_call}, tools, {{"select", ToolKind::kFunction}});
  }
}

TEST(QwenXmlToolCallAccumulatorTest, InvalidEnumDeclarationsRemainUnadmittable) {
  const std::vector<nlohmann::json> schemas = {
      {{"type", "string"}, {"enum", nlohmann::json::array()}},
      {{"type", "string"}, {"enum", {"text", 1}}},
      {{"type", "boolean"}, {"enum", {true, 1}}},
      {{"type", "array"}, {"enum", nlohmann::json::array({nlohmann::json::array()})}},
      {{"type", "object"}, {"enum", nlohmann::json::array({nlohmann::json::object()})}},
  };

  for (const auto& schema : schemas) {
    SCOPED_TRACE(schema.dump());
    const auto tools =
        nlohmann::json::array(
            {{{"type", "function"},
              {"function",
               {{"name", "select"},
                {"parameters", {{"type", "object"}, {"properties", {{"value", schema}}}}}}}}})
            .dump();

    const auto kinds = std::unordered_map<std::string, ToolKind>{{"select", ToolKind::kFunction}};
    ASSERT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools, kinds)));
    const std::string call =
        "<tool_call>\n<function=select>\n<parameter=value>\ntext\n</parameter>\n</function>\n</tool_call>";
    ExpectSchemaFailureWithoutCalls({call}, tools, kinds);
  }
}

TEST(QwenXmlToolCallAccumulatorTest, FloatingNumericEnumDeclarationsRemainUnadmittable) {
  const std::vector<nlohmann::json> schemas = {
      {{"type", "number"}, {"enum", {1.0}}},
      {{"type", "number"}, {"enum", {0.1}}},
      {{"type", "integer"}, {"enum", {1.0}}},
  };

  for (const auto& schema : schemas) {
    SCOPED_TRACE(schema.dump());
    const auto tools =
        nlohmann::json::array(
            {{{"type", "function"},
              {"function",
               {{"name", "select"},
                {"parameters", {{"type", "object"}, {"properties", {{"value", schema}}}}}}}}})
            .dump();

    const auto kinds = std::unordered_map<std::string, ToolKind>{{"select", ToolKind::kFunction}};
    ASSERT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools, kinds)));
    const std::string call =
        "<tool_call>\n<function=select>\n<parameter=value>\n1\n</parameter>\n</function>\n</tool_call>";
    ExpectSchemaFailureWithoutCalls({call}, tools, kinds);
  }
}

TEST(QwenXmlToolCallAccumulatorTest, LargeEnumDeclarationsDeduplicateIdenticalValues) {
  nlohmann::json values = nlohmann::json::array();
  for (size_t index = 0; index < 4096; ++index) {
    values.push_back("value-" + std::to_string(index));
  }

  const auto make_parser = [](nlohmann::json enum_values) {
    const auto tools =
        nlohmann::json::array(
            {{{"type", "function"},
              {"function",
               {{"name", "select"},
                {"parameters",
                 {{"type", "object"},
                  {"properties", {{"value", {{"type", "string"}, {"enum", std::move(enum_values)}}}}}}}}}}})
            .dump();
    return CreateQwenXmlToolCallPayloadParser(tools, {{"select", ToolKind::kFunction}});
  };

  EXPECT_TRUE(static_cast<bool>(make_parser(values)));
  values.push_back("value-2048");
  const auto duplicate_parser = make_parser(std::move(values));
  ASSERT_TRUE(static_cast<bool>(duplicate_parser));
  const auto call =
      "<tool_call>\n<function=select>\n<parameter=value>\nvalue-2048\n</parameter>\n</function>\n</tool_call>";
  EXPECT_EQ(duplicate_parser(call, true).disposition, ToolCallPayloadDisposition::kParsed);
}

TEST(QwenXmlToolCallAccumulatorTest, NumericEnumDuplicateKeysPreserveJsonRepresentation) {
  const auto tools =
      R"([{"type":"function","function":{"name":"select","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"number","enum":[0,-0]}},"required":["value"]}}}])";
  const auto kinds = std::unordered_map<std::string, ToolKind>{{"select", ToolKind::kFunction}};
  ASSERT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools, kinds)));

  for (const auto value : {"0", "-0"}) {
    const auto call = "<tool_call>\n<function=select>\n<parameter=value>\n" + std::string(value) +
                      "\n</parameter>\n</function>\n</tool_call>";
    auto output = RunQwen({call}, tools, kinds);
    ASSERT_EQ(output.calls.size(), 1u) << value;
    EXPECT_TRUE(output.visible.empty()) << value;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, StringEnumInsideAnyOfIsEnforced) {
  const auto tools =
      R"([{"type":"function","function":{"name":"select","parameters":{"type":"object","properties":{)"
      R"("value":{"anyOf":[{"type":"string","enum":["fast"]},{"type":"null","enum":[null]}]}}}}}])";
  const auto make_call = [](std::string_view value) {
    return "<tool_call>\n<function=select>\n<parameter=value>\n" + std::string(value) +
           "\n</parameter>\n</function>\n</tool_call>";
  };

  for (const auto value : {"fast", "null"}) {
    auto output = RunQwen({make_call(value)}, tools, {{"select", ToolKind::kFunction}});
    ASSERT_EQ(output.calls.size(), 1u) << value;
    EXPECT_TRUE(output.visible.empty()) << value;
  }

  const auto rejected_call = make_call("slow");
  ExpectSchemaFailureWithoutCalls({rejected_call}, tools, {{"select", ToolKind::kFunction}});
}

TEST(QwenXmlToolCallAccumulatorTest, RoundedFloatingValueNeverMatchesIntegerNumberEnum) {
  const auto tools =
      R"([{"type":"function","function":{"name":"select","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"number","enum":[9007199254740992]}},"required":["value"]}}}])";
  const auto make_call = [](std::string_view value) {
    return "<tool_call>\n<function=select>\n<parameter=value>\n" + std::string(value) +
           "\n</parameter>\n</function>\n</tool_call>";
  };
  const auto rounded_call = make_call("9007199254740993.0");
  ExpectSchemaFailureWithoutCalls({rounded_call}, tools, {{"select", ToolKind::kFunction}});

  const auto kinds = std::unordered_map<std::string, ToolKind>{{"select", ToolKind::kFunction}};
  EXPECT_TRUE(ParseQwenGuidedToolCalls(
                  R"([{"name":"select","parameters":{"value":9007199254740993.0}}])", tools, kinds)
                  .empty());
}

TEST(QwenXmlToolCallAccumulatorTest, ArrayItemsEnforceScalarEnums) {
  const auto tools =
      R"([{"type":"function","function":{"name":"select","parameters":{"type":"object","properties":{)"
      R"("values":{"type":"array","items":{"type":"string","enum":["src","test"]}}},"required":["values"]}}}])";
  const auto make_call = [](std::string_view value) {
    return "<tool_call>\n<function=select>\n<parameter=values>\n" + std::string(value) +
           "\n</parameter>\n</function>\n</tool_call>";
  };

  auto accepted = RunQwen({make_call(R"(["src","test"])")}, tools, {{"select", ToolKind::kFunction}});
  ASSERT_EQ(accepted.calls.size(), 1u);
  EXPECT_TRUE(accepted.visible.empty());

  const auto rejected_call = make_call(R"(["other"])");
  ExpectSchemaFailureWithoutCalls({rejected_call}, tools, {{"select", ToolKind::kFunction}});
}

TEST(QwenXmlToolCallAccumulatorTest, InvalidEnumValueRejectsEntireAdjacentBatch) {
  const auto tools =
      R"([{"type":"function","function":{"name":"select","parameters":{"type":"object","properties":{)"
      R"("mode":{"type":"string","enum":["fast"]}},"required":["mode"]}}}])";
  const auto make_call = [](std::string_view value) {
    return "<tool_call>\n<function=select>\n<parameter=mode>\n" + std::string(value) +
           "\n</parameter>\n</function>\n</tool_call>";
  };
  const auto generated = make_call("fast") + "\n" + make_call("unsupported");

  ExpectSchemaFailureWithoutCalls({generated}, tools, {{"select", ToolKind::kFunction}});
}

TEST(QwenXmlToolCallAccumulatorTest, GuidedCallsEnforceEnumMembership) {
  const auto tools =
      R"([{"type":"function","function":{"name":"select","parameters":{"type":"object","properties":{)"
      R"("mode":{"type":"string","enum":["fast","thorough"]}},"required":["mode"]}}}])";
  const std::unordered_map<std::string, ToolKind> kinds = {{"select", ToolKind::kFunction}};

  const auto accepted =
      ParseQwenGuidedToolCalls(R"([{"name":"select","parameters":{"mode":"fast"}}])", tools, kinds);
  ASSERT_EQ(accepted.size(), 1u);
  EXPECT_EQ(accepted.front().arguments, R"({"mode":"fast"})");

  EXPECT_TRUE(ParseQwenGuidedToolCalls(
                  R"([{"name":"select","parameters":{"mode":"unsupported"}}])", tools, kinds)
                  .empty());
}

TEST(QwenXmlToolCallAccumulatorTest, RepresentativeStockGhcpEnumDeclarationsEnableExactDecoder) {
  const nlohmann::json string_schema = {{"type", "string"}};
  nlohmann::json tools = nlohmann::json::array();
  std::unordered_map<std::string, ToolKind> kinds;
  const auto add_tool = [&](std::string name, nlohmann::json properties) {
    tools.push_back(
        {{"type", "function"},
         {"function",
          {{"name", name},
           {"parameters", {{"type", "object"}, {"properties", std::move(properties)}}}}}});
    kinds.emplace(std::move(name), ToolKind::kFunction);
  };

  add_tool("bash", {{"command", string_schema}, {"mode", {{"type", "string"}, {"enum", {"sync", "async"}}}}});
  add_tool("session_store_sql",
           {{"description", string_schema},
            {"query", string_schema},
            {"source", {{"type", "string"}, {"enum", {"cloud", "local"}}}}});
  add_tool("list_agents", {{"scope", {{"type", "string"}, {"enum", {"siblings", "children", "all"}}}}});
  add_tool("write_agent", {{"scope", {{"type", "string"}, {"enum", {"siblings", "children"}}}}});
  add_tool("rg",
           {{"pattern", string_schema},
            {"output_mode", {{"type", "string"}, {"enum", {"content", "files_with_matches", "count"}}}}});
  add_tool("task",
           {{"agent_type",
             {{"type", "string"},
              {"enum",
               {"explore", "task", "general-purpose", "code-review", "research", "security-review",
                "paged-attention-runtime-strategist", "ApiExpert", "CSharpCoder", "CppCoder", "DearLeader", "JsCoder",
                "PortCSharpToCpp", "PythonCoder", "Reviewer", "Tester"}}}},
            {"context_tier", {{"type", "string"}, {"enum", {"default", "long_context"}}}},
            {"mode", {{"type", "string"}, {"enum", {"sync", "background"}}}}});

  EXPECT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(tools.dump(), kinds)));
}

TEST(QwenXmlToolCallAccumulatorTest, RunTestsEnumCallWithVisiblePrefixesIsChunkInvariant) {
  const auto tools =
      R"([{"type":"function","function":{"name":"run_tests","parameters":{"type":"object","properties":{)"
      R"("target":{"type":"string","enum":["test_runtime_options.py"]}},"required":["target"],)"
      R"("additionalProperties":false}}}])";
  const std::string call =
      "<tool_call>\n<function=run_tests>\n<parameter=target>\ntest_runtime_options.py\n"
      "</parameter>\n</function>\n</tool_call>";

  for (const auto prefix : {"\n", "Patch applied. Now running the focused tests:\n\n"}) {
    const auto generated = std::string(prefix) + call + "\n2 passed in 0.03s";
    for (size_t split = 0; split <= generated.size(); ++split) {
      auto output = RunQwen(SplitAt(generated, split), tools, {{"run_tests", ToolKind::kFunction}});
      ASSERT_EQ(output.calls.size(), 1u) << "prefix=" << prefix << ", split=" << split;
      EXPECT_EQ(output.calls.front().arguments, R"({"target":"test_runtime_options.py"})")
          << "prefix=" << prefix << ", split=" << split;
      EXPECT_EQ(output.visible, std::string(prefix) + "\n2 passed in 0.03s")
          << "prefix=" << prefix << ", split=" << split;
    }
  }
}

TEST(QwenXmlToolCallAccumulatorTest, DuplicateDeclarationsFailClosedInEitherOrderForEverySchemaShape) {
  const nlohmann::json valid_parameters = {
      {"type", "object"},
      {"properties", {{"value", {{"type", "string"}}}}},
      {"required", {"value"}},
  };
  const std::vector<std::optional<nlohmann::json>> duplicate_shapes = {
      std::nullopt,
      nlohmann::json(nullptr),
      nlohmann::json::object(),
      nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}},
      nlohmann::json(1),
      nlohmann::json{{"type", "object"}, {"properties", 1}},
  };
  const std::string generated =
      "<tool_call>\n<function=duplicate>\n<parameter=value>\ntext\n</parameter>\n"
      "</function>\n</tool_call>";

  for (const auto& duplicate_parameters : duplicate_shapes) {
    auto valid = nlohmann::json{{"name", "duplicate"}, {"parameters", valid_parameters}};
    auto duplicate = nlohmann::json{{"name", "duplicate"}};
    if (duplicate_parameters.has_value()) {
      duplicate["parameters"] = *duplicate_parameters;
    }

    for (const bool duplicate_first : {false, true}) {
      auto tools = nlohmann::json::array();
      tools.push_back({{"type", "function"},
                       {"function", duplicate_first ? duplicate : valid}});
      tools.push_back({{"type", "function"},
                       {"function", duplicate_first ? valid : duplicate}});
      SCOPED_TRACE(duplicate_first);
      SCOPED_TRACE(duplicate_parameters.has_value() ? duplicate_parameters->dump() : "omitted");
      ExpectSchemaFailureWithoutCalls(
          {generated}, tools.dump(), {{"duplicate", ToolKind::kFunction}});
    }
  }
}

TEST(QwenXmlToolCallAccumulatorTest, DuplicateCustomDeclarationsFailClosedInEitherOrder) {
  const auto canonical_parameters = nlohmann::json::parse(kCustomToolInputSchema);
  const auto valid =
      nlohmann::json{{"name", "duplicate"}, {"parameters", canonical_parameters}};
  const auto invalid = nlohmann::json{
      {"name", "duplicate"},
      {"parameters", {{"type", "object"}, {"properties", nlohmann::json::object()}}},
  };
  const std::string generated =
      "<tool_call>\n<function=duplicate>\n<parameter=input>\ntext\n</parameter>\n"
      "</function>\n</tool_call>";

  for (const bool invalid_first : {false, true}) {
    auto tools = nlohmann::json::array();
    tools.push_back({{"type", "function"},
                     {"function", invalid_first ? invalid : valid}});
    tools.push_back({{"type", "function"},
                     {"function", invalid_first ? valid : invalid}});
    SCOPED_TRACE(invalid_first);
    ExpectSchemaFailureWithoutCalls(
        {generated}, tools.dump(), {{"duplicate", ToolKind::kCustom}});
  }
}

TEST(QwenXmlToolCallAccumulatorTest, CopilotGlobAnyOfDecodesOmittedStringAndArrayPaths) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"glob","parameters":{"type":"object","properties":{)"
      R"("pattern":{"type":"string"},"paths":{"anyOf":[{"type":"string"},{"type":"array","items":{"type":"string"}}]})"
      R"(},"required":["pattern"]}}}])";
  const std::string supported_call =
      "<tool_call>\n"
      "<function=glob>\n"
      "<parameter=pattern>\n"
      "**/range_math.py\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";
  auto supported_accumulator = MakeQwenAccumulator(tools, {{"glob", ToolKind::kFunction}});
  auto supported_outputs = RunChunks(supported_accumulator, {supported_call});
  const auto calls = CollectCalls(supported_outputs);

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.front().name, "glob");
  EXPECT_EQ(calls.front().arguments, R"({"pattern":"**/range_math.py"})");
  EXPECT_TRUE(CollectVisible(supported_outputs).empty());

  const std::vector<std::pair<std::string, nlohmann::json>> path_values = {
      {"src", "src"},
      {R"(["src","test"])", nlohmann::json::array({"src", "test"})},
  };
  for (const auto& [body, expected] : path_values) {
    const auto generated =
        "<tool_call>\n"
        "<function=glob>\n"
        "<parameter=pattern>\n"
        "**/range_math.py\n"
        "</parameter>\n"
        "<parameter=paths>\n" +
        body +
        "\n</parameter>\n"
        "</function>\n"
        "</tool_call>";
    auto output = RunQwen({generated}, tools, {{"glob", ToolKind::kFunction}});
    ASSERT_EQ(output.calls.size(), 1u) << body;
    const auto arguments = nlohmann::json::parse(output.calls.front().arguments);
    EXPECT_EQ(arguments["pattern"], "**/range_math.py");
    EXPECT_EQ(arguments["paths"], expected);
    EXPECT_TRUE(output.visible.empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, CopilotGlobAnyOfRejectsMalformedAndWrongUnionValues) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"glob","parameters":{"type":"object","properties":{)"
      R"("pattern":{"type":"string"},"paths":{"anyOf":[{"type":"string"},{"type":"array","items":{"type":"string"}}]})"
      R"(},"required":["pattern"]}}}])";
  const std::vector<std::string> invalid_values = {
      R"({"root":"src"})",
      R"(["src",1])",
      R"(["src")",
  };

  for (const auto& body : invalid_values) {
    const auto generated =
        "<tool_call>\n"
        "<function=glob>\n"
        "<parameter=pattern>\n"
        "**/range_math.py\n"
        "</parameter>\n"
        "<parameter=paths>\n" +
        body +
        "\n"
        "</parameter>\n"
        "</function>\n"
        "</tool_call>";
    ExpectSchemaFailureWithoutCalls({generated}, tools, {{"glob", ToolKind::kFunction}});
  }
}

TEST(QwenXmlToolCallAccumulatorTest, NestedAnyOfArrayItemsAdmitOnlyDeclaredBranches) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"collect","parameters":{"type":"object","properties":{)"
      R"("values":{"type":"array","items":{"anyOf":[{"type":"string"},{"type":"integer"}]}}},)"
      R"("required":["values"]}}}])";
  const std::string generated =
      "<tool_call>\n"
      "<function=collect>\n"
      "<parameter=values>\n"
      "[\"src\",1]\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";

  EXPECT_TRUE(static_cast<bool>(
      CreateQwenXmlToolCallPayloadParser(tools, {{"collect", ToolKind::kFunction}})));
  EXPECT_TRUE(static_cast<bool>(CreateQwenXmlToolCallPayloadParser(
      tools, {{"collect", ToolKind::kFunction}}, /*recovery_aware=*/true)));

  const auto accepted = RunQwen({generated}, tools, {{"collect", ToolKind::kFunction}});
  ASSERT_EQ(accepted.calls.size(), 1u);
  EXPECT_EQ(accepted.calls.front().arguments, R"({"values":["src",1]})");
  EXPECT_TRUE(accepted.visible.empty());
  EXPECT_TRUE(BuildQwenXmlToolBodyGrammar(tools, {{"collect", ToolKind::kFunction}}));

  auto recovery_accumulator =
      MakeQwenAccumulator(tools, {{"collect", ToolKind::kFunction}}, /*recovery_aware=*/true);
  const std::string malformed =
      "<tool_call>\n"
      "<function=collect>\n"
      "<param=values>\n"
      "[\"src\",1]\n"
      "</param>\n"
      "</function>\n"
      "</tool_call>";
  auto recovery_outputs = RunChunks(recovery_accumulator, {malformed});
  EXPECT_TRUE(AnyMalformed(recovery_outputs));
  EXPECT_TRUE(CollectVisible(recovery_outputs).empty());
  EXPECT_TRUE(CollectCalls(recovery_outputs).empty());

  const auto guided = R"([{"name":"collect","parameters":{"values":["src",1]}}])";
  ASSERT_EQ(ParseQwenGuidedToolCalls(guided, tools, {{"collect", ToolKind::kFunction}}).size(), 1u);
  EXPECT_TRUE(ParseQwenGuidedToolCalls(
      R"([{"name":"collect","parameters":{"values":["src",true]}}])",
      tools, {{"collect", ToolKind::kFunction}}).empty());
}

TEST(QwenXmlToolCallAccumulatorTest, ProductionNormalizedCustomToolIsDecoded) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"apply_patch","parameters":{"type":"object",)"
      R"("properties":{"input":{"type":"string"}},"required":["input"],"additionalProperties":false}}}])";
  const std::string generated =
      "<tool_call>\n"
      "<function=apply_patch>\n"
      "<parameter=input>\n"
      "*** Begin Patch\n"
      "*** Update File: calc.py\n"
      "@@\n"
      "-    return a > b\n"
      "+    return a < b\n"
      "*** End Patch\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";

  for (size_t split = 0; split <= generated.size(); ++split) {
    auto output = RunQwen(SplitAt(generated, split), tools, {{"apply_patch", ToolKind::kCustom}});
    EXPECT_TRUE(output.visible.empty()) << "split=" << split;
    ASSERT_EQ(output.calls.size(), 1u) << "split=" << split;
    EXPECT_EQ(output.calls[0].name, "apply_patch") << "split=" << split;
    EXPECT_EQ(output.calls[0].arguments,
              R"({"input":"*** Begin Patch\n*** Update File: calc.py\n@@\n)"
              R"(-    return a > b\n+    return a < b\n*** End Patch"})")
        << "split=" << split;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, CustomPayloadContainingXmlClosingDelimiterRemainsVisible) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"custom","parameters":{"type":"object",)"
      R"("properties":{"input":{"type":"string"}},"required":["input"],"additionalProperties":false}}}])";
  const std::string generated =
      "<tool_call>\n<function=custom>\n<parameter=input>\nbefore\n</parameter>\nafter\n"
      "</parameter>\n</function>\n</tool_call>";

  auto output = RunQwen({generated}, tools, {{"custom", ToolKind::kCustom}});
  EXPECT_EQ(output.visible, generated);
  EXPECT_TRUE(output.calls.empty());
}

TEST(QwenXmlToolCallAccumulatorTest, CustomToolWithNoncanonicalSchemaFailsClosed) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"custom","parameters":{"type":"object","properties":{}}}}])";
  const std::string generated = "<tool_call>\n<function=custom>\n</function>\n</tool_call>";

  const auto kinds = std::unordered_map<std::string, ToolKind>{{"custom", ToolKind::kCustom}};
  EXPECT_FALSE(BuildQwenXmlToolBodyGrammar(tools, kinds));
  ExpectSchemaFailureWithoutCalls({generated}, tools, kinds);
}

TEST(QwenXmlToolCallAccumulatorTest, DeclaredParameterlessSchemaShapesDecodeExactEmptyArguments) {
  const std::vector<nlohmann::json> parameters = {
      nullptr,
      nlohmann::json::object(),
      {{"type", "object"}},
      {{"type", "object"}, {"properties", nlohmann::json::object()}},
  };

  for (size_t index = 0; index < parameters.size(); ++index) {
    SCOPED_TRACE("schema=" + parameters[index].dump());
    auto function = nlohmann::json{{"name", "zero"}};
    function["parameters"] = parameters[index];
    const auto tools =
        nlohmann::json::array({{{"type", "function"}, {"function", function}}}).dump();
    auto output = RunQwen({kValidZeroQwenCall}, tools, {{"zero", ToolKind::kFunction}});
    ASSERT_EQ(output.calls.size(), 1u);
    EXPECT_EQ(output.calls.front().name, "zero");
    EXPECT_EQ(output.calls.front().arguments, "{}");
    EXPECT_TRUE(output.visible.empty());
  }

  const auto omitted =
      R"([{"type":"function","function":{"name":"zero"}}])";
  auto output = RunQwen({kValidZeroQwenCall}, omitted, {{"zero", ToolKind::kFunction}});
  ASSERT_EQ(output.calls.size(), 1u);
  EXPECT_EQ(output.calls.front().arguments, "{}");
}

TEST(QwenXmlToolCallAccumulatorTest, LegacyDirectNameWithoutTypeDecodes) {
  const auto tools =
      R"([{"name":"zero","parameters":{"type":"object","properties":{}}}])";
  auto output = RunQwen({kValidZeroQwenCall}, tools, {{"zero", ToolKind::kFunction}});
  ASSERT_EQ(output.calls.size(), 1u);
  EXPECT_EQ(output.calls.front().name, "zero");
  EXPECT_EQ(output.calls.front().arguments, "{}");
  EXPECT_TRUE(output.visible.empty());
}

TEST(QwenXmlToolCallAccumulatorTest,
     MalformedParameterSchemasFailEntireAdjacentBatchClosed) {
  const std::vector<nlohmann::json> malformed_parameters = {
      1,
      "object",
      nlohmann::json::array(),
      true,
      {{"type", 1}},
      {{"type", nullptr}},
      {{"type", nlohmann::json::array({"object"})}},
      {{"type", nlohmann::json::object()}},
      {{"type", "object"}, {"properties", 1}},
      {{"type", "object"}, {"required", 1}},
      {{"type", "object"},
       {"properties", nlohmann::json::object()},
       {"required", nlohmann::json::array({1})}},
      {{"type", "object"}, {"minProperties", 1}},
  };
  const std::string malformed_call =
      "<tool_call>\n<function=bad>\n<parameter=value>\ntext\n</parameter>\n"
      "</function>\n</tool_call>";
  const auto generated = kValidZeroQwenCall + "\n" + malformed_call;

  for (const auto& parameters : malformed_parameters) {
    SCOPED_TRACE("parameters=" + parameters.dump());
    const auto tools =
        nlohmann::json::array(
            {{{"type", "function"},
              {"function",
               {{"name", "zero"},
                {"parameters",
                 {{"type", "object"},
                  {"properties", nlohmann::json::object()}}}}}},
             {{"type", "function"},
              {"function", {{"name", "bad"}, {"parameters", parameters}}}}})
            .dump();
    ExpectSchemaFailureWithoutCalls(
        {generated}, tools,
        {{"zero", ToolKind::kFunction}, {"bad", ToolKind::kFunction}});
  }
}

TEST(QwenXmlToolCallAccumulatorTest, ExcessiveArraySchemaNestingFailsClosed) {
  nlohmann::json value_schema = {{"type", "string"}};
  for (size_t depth = 0; depth < 16; ++depth) {
    value_schema = {{"type", "array"}, {"items", std::move(value_schema)}};
  }

  const nlohmann::json parameters = {
      {"type", "object"},
      {"properties", {{"value", std::move(value_schema)}}},
      {"required", {"value"}},
  };
  const nlohmann::json function = {{"name", "deep"}, {"parameters", parameters}};
  const auto tools =
      nlohmann::json::array({{{"type", "function"}, {"function", function}}}).dump();
  const std::string generated =
      "<tool_call>\n<function=deep>\n<parameter=value>\n[]\n</parameter>\n</function>\n</tool_call>";
  ExpectSchemaFailureWithoutCalls({generated}, tools, {{"deep", ToolKind::kFunction}});
}

TEST(QwenXmlToolCallAccumulatorTest, ParameterWithoutSchemaFailsEntireAdjacentBatchClosed) {
  const auto tools =
      R"([{"type":"function","function":{"name":"zero","parameters":{"type":"object"}}}])";
  const std::string generated =
      "<tool_call>\n<function=zero>\n<parameter=unknown>\nvalue\n</parameter>\n"
      "</function>\n</tool_call>\n" +
      kValidZeroQwenCall;
  ExpectSchemaFailureWithoutCalls({generated}, tools, {{"zero", ToolKind::kFunction}});
}

TEST(QwenXmlToolCallAccumulatorTest, MalformedAndIncompleteCandidatesRemainExactVisibleText) {
  const std::vector<std::string> generated = {
      "<tool_call>\r\n<function=zero>\r\n</function>\r\n</tool_call>",
      "<tool_call attribute=x>\n<function=zero>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nprefix\n</parameter>\nsuffix\n"
      "</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<param=text>\nx\n</param>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\ntruncated",
  };

  for (const auto& candidate : generated) {
    auto output = RunQwen({candidate});
    EXPECT_EQ(output.visible, candidate);
    EXPECT_TRUE(output.calls.empty());
  }

  ExpectSchemaFailureWithoutCalls({
      "<tool_call>\n<function=typed>\n<parameter=text>\nx\n</function>\n"
      "</parameter>\n</function>\n</tool_call>",
  });
}

TEST(QwenXmlToolCallAccumulatorTest, RecoveryAwareQualifiedStructuralFailuresAreSuppressed) {
  const std::vector<std::string> generated = {
      "<tool_call>\n<function=typed>\n<param=text>\nx\n</param>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\ntruncated",
  };

  for (const auto& candidate : generated) {
    auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, /*recovery_aware=*/true);
    auto outputs = RunChunks(accumulator, {candidate});

    EXPECT_TRUE(AnyMalformed(outputs)) << candidate;
    EXPECT_TRUE(CollectVisible(outputs).empty()) << candidate;
    EXPECT_TRUE(CollectCalls(outputs).empty()) << candidate;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, RecoveryAwareUnknownToolsRemainVisible) {
  const std::vector<std::string> generated = {
      "<tool_call>\n<function=missing>\n</function>\n</tool_call>",
  };

  for (const auto& candidate : generated) {
    auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, /*recovery_aware=*/true);
    auto outputs = RunChunks(accumulator, {candidate});

    EXPECT_FALSE(AnyMalformed(outputs)) << candidate;
    EXPECT_EQ(CollectVisible(outputs), candidate);
    EXPECT_TRUE(CollectCalls(outputs).empty()) << candidate;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, RecoveryAwareSchemaViolationsFailClosed) {
  const std::vector<std::string> generated = {
      "<tool_call>\n<function=typed>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=unknown>\nvalue\n</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=integer>\n1.5\n</parameter>\n</function>\n</tool_call>",
  };

  for (const auto& candidate : generated) {
    auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, /*recovery_aware=*/true);
    auto outputs = RunChunks(accumulator, {candidate});

    EXPECT_TRUE(AnyMalformed(outputs)) << candidate;
    EXPECT_TRUE(CollectVisible(outputs).empty()) << candidate;
    EXPECT_TRUE(CollectCalls(outputs).empty()) << candidate;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, RecoveryAwareValidCallThenMalformedSiblingRejectsAtomicBatch) {
  const auto generated =
      kValidZeroQwenCall +
      "\n<tool_call>\n<function=typed>\n<param=text>\nx\n</param>\n</function>\n</tool_call>";
  auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, /*recovery_aware=*/true);
  auto outputs = RunChunks(accumulator, {generated});

  EXPECT_FALSE(AnyMalformed(outputs));
  EXPECT_EQ(CollectVisible(outputs), generated);
  EXPECT_TRUE(CollectCalls(outputs).empty());
}

TEST(QwenXmlToolCallAccumulatorTest, RecoveryAwareVisiblePrefixPolicyIsInvariantAcrossEverySplit) {
  const std::string malformed =
      "<tool_call>\n<function=typed>\n<param=text>\nx\n</param>\n</function>\n</tool_call>";
  const std::string generated = "safe prefix" + malformed;

  for (size_t split = 0; split <= generated.size(); ++split) {
    auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, /*recovery_aware=*/true);
    auto outputs = RunChunks(accumulator, SplitAt(generated, split));

    EXPECT_TRUE(AnyMalformed(outputs)) << "split=" << split;
    EXPECT_EQ(CollectVisible(outputs), "safe prefix") << "split=" << split;
    EXPECT_TRUE(CollectCalls(outputs).empty()) << "split=" << split;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, GuidedRetryRejectsDuplicateJsonMembers) {
  const std::vector<std::string> payloads = {
      R"([{"name":"typed","name":"zero","parameters":{"text":"first"}}])",
      R"([{"name":"typed","parameters":{"text":"first","text":"second"}}])",
      R"([{"name":"typed","parameters":{"text":{"nested":1,"nested":2}}}])",
  };

  for (const auto& payload : payloads) {
    EXPECT_TRUE(ParseQwenGuidedToolCalls(payload, kQwenTools, kQwenToolKinds).empty()) << payload;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, BacktickAndTildeFencedExamplesStayVisibleAcrossEverySplit) {
  const std::vector<std::string> generated = {
      "``````xml\n"
      "<tool_call>\n<function=zero>\n</function>\n</tool_call>\n"
      "``````",
      "~~~~qwen\n"
      "<tool_call>\n<function=zero>\n</function>\n</tool_call>\n"
      "~~~~",
  };

  for (const auto& input : generated) {
    for (size_t split = 0; split <= input.size(); ++split) {
      auto output = RunQwen(SplitAt(input, split));
      EXPECT_EQ(output.visible, input) << "split=" << split;
      EXPECT_TRUE(output.calls.empty()) << "split=" << split;
    }

    auto output = RunQwen(SplitIntoBytes(input));
    EXPECT_EQ(output.visible, input);
    EXPECT_TRUE(output.calls.empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, OversizedCallFailsClosedWithWhitespaceAdjacentCallsInOneChunk) {
  const auto generated =
      MakeOversizedQwenCall() + " \n\t" + kValidZeroQwenCall + "\n" + kValidZeroQwenCall;

  ExpectSchemaFailureWithoutCalls({generated});
}

TEST(QwenXmlToolCallAccumulatorTest, OversizedCallFailsClosedAcrossStructuralSplits) {
  const auto oversized = MakeOversizedQwenCall();
  const std::string whitespace = " \n\t";
  const auto generated = oversized + whitespace + kValidZeroQwenCall;
  const auto adjacent_start = oversized.size() + whitespace.size();
  const std::vector<size_t> split_positions = {
      0,
      1,
      std::string("<tool_call>").size() - 1,
      std::string("<tool_call>").size(),
      64 * 1024 - 1,
      64 * 1024,
      64 * 1024 + 1,
      oversized.size() - std::string("</tool_call>").size(),
      oversized.size() - 1,
      oversized.size(),
      adjacent_start - 1,
      adjacent_start,
      adjacent_start + 1,
      adjacent_start + std::string("<tool_call>").size() - 1,
      adjacent_start + std::string("<tool_call>").size(),
      generated.size() - 1,
      generated.size(),
  };

  for (const auto split : split_positions) {
    SCOPED_TRACE("split=" + std::to_string(split));
    ExpectSchemaFailureWithoutCalls(SplitAt(generated, split));
  }
}

TEST(QwenXmlToolCallAccumulatorTest, OversizedCallFailsClosedByteAtATime) {
  const auto generated = MakeOversizedQwenCall() + " \n\t" + kValidZeroQwenCall;

  ExpectSchemaFailureWithoutCalls(SplitIntoBytes(generated));
}

TEST(QwenXmlToolCallAccumulatorTest, OversizedCandidateFailsClosedEvenBeforeIndependentLaterCall) {
  const auto oversized = MakeOversizedQwenCall();
  const std::string generated = oversized + " visible " + kValidZeroQwenCall + " tail";

  for (const auto split : {size_t{0}, kSelectedPayloadBufferLimit - 1,
                           kSelectedPayloadBufferLimit, oversized.size(), generated.size()}) {
    SCOPED_TRACE(split);
    ExpectSchemaFailureWithoutCalls(SplitAt(generated, split));
  }
}

std::string MakeBoundaryStraddledOversizedCandidate() {
  constexpr std::string_view closing_prefix = "</tool_";
  const auto padding_size =
      64 * 1024 - std::string_view("<tool_call>").size() - closing_prefix.size();
  return "<tool_call>" + std::string(padding_size, 'x') +
         std::string(closing_prefix) + "call>";
}

TEST(QwenXmlToolCallAccumulatorTest, BoundaryStraddledOversizeCloseFailsClosedInOneChunk) {
  const auto rejected = MakeBoundaryStraddledOversizedCandidate();
  const auto generated = rejected + " visible " + kValidZeroQwenCall + " tail";

  ExpectSchemaFailureWithoutCalls({generated});
}

TEST(QwenXmlToolCallAccumulatorTest, BoundaryStraddledOversizeCloseFailsClosedAtEveryCloseSplit) {
  const auto rejected = MakeBoundaryStraddledOversizedCandidate();
  const auto generated = rejected + " visible " + kValidZeroQwenCall + " tail";
  constexpr auto close_size = std::string_view("</tool_call>").size();
  const auto close_start = rejected.size() - close_size;

  for (size_t offset = 0; offset <= close_size; ++offset) {
    SCOPED_TRACE("offset=" + std::to_string(offset));
    ExpectSchemaFailureWithoutCalls(SplitAt(generated, close_start + offset));
  }
}

TEST(QwenXmlToolCallAccumulatorTest, BoundaryStraddledOversizeCloseFailsClosedByteAtATime) {
  const auto rejected = MakeBoundaryStraddledOversizedCandidate();
  const auto generated = rejected + " visible " + kValidZeroQwenCall + " tail";

  ExpectSchemaFailureWithoutCalls(SplitIntoBytes(generated));
}

TEST(QwenXmlToolCallAccumulatorTest, CandidateAtLimitIsParsedBeforeOversizedRejection) {
  const std::string visible_tail(64 * 1024, 'v');
  const auto generated = kValidTypedQwenCall + visible_tail + kValidZeroQwenCall;

  ExpectTwoCallsAroundVisibleTail(SplitAt(generated, kValidTypedQwenCall.size()),
                                  visible_tail);
}

TEST(QwenXmlToolCallAccumulatorTest, ExactlyLimitSizedCandidateParsesAcrossBoundarySplits) {
  const auto candidate = MakeSizedQwenCall(kSelectedPayloadBufferLimit);
  const auto generated = candidate + "tail";
  const std::vector<size_t> split_positions = {
      0,
      1,
      std::string_view("<tool_call>").size(),
      kSelectedPayloadBufferLimit - 1,
      kSelectedPayloadBufferLimit,
      kSelectedPayloadBufferLimit + 1,
      generated.size(),
  };

  for (const auto split : split_positions) {
    SCOPED_TRACE("split=" + std::to_string(split));
    auto output = RunQwen(SplitAt(generated, split));
    ASSERT_EQ(output.calls.size(), 1u);
    EXPECT_EQ(output.calls.front().name, "typed");
    EXPECT_EQ(nlohmann::json::parse(output.calls.front().arguments)["text"],
              std::string(kSelectedPayloadBufferLimit -
                              std::string_view("<tool_call>\n<function=typed>\n<parameter=text>\n")
                                  .size() -
                              std::string_view("\n</parameter>\n</function>\n</tool_call>").size(),
                          'x'));
    EXPECT_EQ(output.visible, "tail");
  }
}

TEST(QwenXmlToolCallAccumulatorTest, ExactlyLimitSizedCandidateParsesAtEndOfStream) {
  const auto candidate = MakeSizedQwenCall(kSelectedPayloadBufferLimit);

  {
    auto output = RunQwen(SplitIntoBytes(candidate));
    ASSERT_EQ(output.calls.size(), 1u);
    EXPECT_EQ(output.calls.front().name, "typed");
    EXPECT_TRUE(output.visible.empty());
  }

  {
    auto output = RunQwen(SplitAt(candidate + " ", candidate.size()));
    ASSERT_EQ(output.calls.size(), 1u);
    EXPECT_EQ(output.calls.front().name, "typed");
    EXPECT_EQ(output.visible, " ");
  }
}

TEST(QwenXmlToolCallAccumulatorTest, ExactlyLimitSizedCandidateHandlesWhitespaceBeforeIndependentOutput) {
  const auto candidate = MakeSizedQwenCall(kSelectedPayloadBufferLimit);
  const std::string visible = " visible ";
  const auto generated = candidate + visible + kValidZeroQwenCall;
  const std::vector<std::vector<std::string>> chunkings = {
      {generated},
      SplitAt(generated, candidate.size()),
      SplitIntoBytes(generated),
  };

  for (const auto& chunks : chunkings) {
    auto output = RunQwen(chunks);
    ASSERT_EQ(output.calls.size(), 2u);
    EXPECT_EQ(output.calls[0].name, "typed");
    EXPECT_EQ(output.calls[1].name, "zero");
    EXPECT_EQ(output.visible, visible);
  }
}

TEST(QwenXmlToolCallAccumulatorTest, LimitSizedCandidateAndAdjacentBatchFailsClosedAtEveryMarkerSplit) {
  const std::string separator = " \n\t";
  constexpr size_t partial_marker_size = 5;
  const auto candidate =
      MakeSizedQwenCall(kSelectedPayloadBufferLimit - separator.size() - partial_marker_size);
  const auto generated = candidate + separator + kValidZeroQwenCall;
  const auto adjacent_start = candidate.size() + separator.size();

  ExpectSchemaFailureWithoutCalls({generated});
  for (size_t offset = 0; offset <= std::string_view("<tool_call>").size(); ++offset) {
    SCOPED_TRACE("offset=" + std::to_string(offset));
    ExpectSchemaFailureWithoutCalls(SplitAt(generated, adjacent_start + offset));
  }
  ExpectSchemaFailureWithoutCalls(SplitIntoBytes(generated));
}

TEST(QwenXmlToolCallAccumulatorTest, ExactlyLimitSizedCandidateFinalizesBeforePartialMarkerAtEos) {
  const std::string visible = " \n<tool";
  const auto candidate = MakeSizedQwenCall(kSelectedPayloadBufferLimit - visible.size());
  ASSERT_EQ(candidate.size() + visible.size(), kSelectedPayloadBufferLimit);
  auto output = RunQwen(SplitIntoBytes(candidate + visible));
  ASSERT_EQ(output.calls.size(), 1u);
  EXPECT_EQ(output.calls.front().name, "typed");
  EXPECT_EQ(output.visible, visible);
}

TEST(QwenXmlToolCallAccumulatorTest, ExactlyLimitSizedPartialMarkerMismatchBecomesVisibleText) {
  const std::string separator = " \n";
  const std::string partial_marker = "<tool";
  const auto candidate =
      MakeSizedQwenCall(kSelectedPayloadBufferLimit - separator.size() - partial_marker.size());
  const std::string visible = separator + partial_marker + "x ordinary";
  const auto generated = candidate + visible;
  const std::vector<std::vector<std::string>> chunkings = {
      {generated},
      SplitAt(generated, kSelectedPayloadBufferLimit),
      SplitIntoBytes(generated),
  };

  for (const auto& chunks : chunkings) {
    auto output = RunQwen(chunks);
    ASSERT_EQ(output.calls.size(), 1u);
    EXPECT_EQ(output.calls.front().name, "typed");
    EXPECT_EQ(output.visible, visible);
  }
}

TEST(QwenXmlToolCallAccumulatorTest, CandidateOneByteOverLimitFailsClosed) {
  const auto candidate = MakeSizedQwenCall(kSelectedPayloadBufferLimit + 1);

  ExpectSchemaFailureWithoutCalls({candidate});
  ExpectSchemaFailureWithoutCalls(SplitAt(candidate, kSelectedPayloadBufferLimit));
  ExpectSchemaFailureWithoutCalls(SplitIntoBytes(candidate));
}

TEST(QwenXmlToolCallAccumulatorTest, ByteSizedChunksPerformBoundedSelectedPayloadParseWork) {
  const auto candidate = MakeSizedQwenCall(kSelectedPayloadBufferLimit - 1);
  auto parser =
      CreateQwenXmlToolCallPayloadParser(kQwenTools, kQwenToolKinds);
  size_t parsed_bytes = 0;
  size_t parse_count = 0;
  ToolCallStreamAccumulator acc(
      "<tool_call>", "</tool_call>", kQwenTools, "",
      [&](std::string_view source, bool end_of_stream) {
        ++parse_count;
        parsed_bytes += source.size();
        return parser(source, end_of_stream);
      });

  std::vector<ToolCallStreamAccumulator::Output> outputs;
  for (const auto& chunk : SplitIntoBytes(candidate + "tail")) {
    outputs.push_back(acc.Push(chunk));
  }
  outputs.push_back(acc.Flush());

  auto calls = CollectCalls(outputs);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(CollectVisible(outputs), "tail");
  EXPECT_EQ(parse_count, 1u);
  EXPECT_LE(parsed_bytes, candidate.size() + 1);
}

TEST(QwenXmlToolCallAccumulatorTest, CallsAroundLimitSizedVisibleTailAreChunkInvariant) {
  const std::string visible_tail(64 * 1024, 'v');
  const auto generated = kValidTypedQwenCall + visible_tail + kValidZeroQwenCall;

  ExpectTwoCallsAroundVisibleTail({generated}, visible_tail);

  for (size_t split = 0; split <= generated.size(); ++split) {
    SCOPED_TRACE("split=" + std::to_string(split));
    ExpectTwoCallsAroundVisibleTail(SplitAt(generated, split), visible_tail);
  }

  ExpectTwoCallsAroundVisibleTail(SplitIntoBytes(generated), visible_tail);
}

// ========================================================================
// Passthrough mode — no markers configured.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, EmptyMarkersIsPassthrough) {
  ToolCallStreamAccumulator acc("", "");
  auto out = acc.Push("any text including <tool_call> markers");
  ASSERT_EQ(out.events.size(), 1u);
  EXPECT_EQ(std::get<std::string>(out.events[0]), "any text including <tool_call> markers");
  EXPECT_FALSE(acc.InsideToolCall());
}

TEST(ToolCallStreamAccumulatorTest, EmptyChunkProducesNothing) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  auto out = acc.Push("");
  EXPECT_TRUE(out.events.empty());
}

// ========================================================================
// No tool calls — text-only path.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, PlainTextPassesThroughVerbatim) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  auto outs = RunChunks(acc, {"Hello", ", ", "world!"});
  EXPECT_EQ(CollectVisible(outs), "Hello, world!");
  for (const auto& o : outs) {
    EXPECT_TRUE(std::none_of(o.events.begin(), o.events.end(), [](const auto& event) {
      return std::holds_alternative<ParsedToolCall>(event);
    }));
  }
}

// ========================================================================
// Single tool call, varying chunking strategies.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, SingleToolCallInOneChunk) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  std::string chunk =
      R"(prefix <tool_call>[{"name":"add","arguments":{"a":1,"b":2}}]</tool_call> suffix)";
  auto outs = RunChunks(acc, {chunk});

  EXPECT_EQ(CollectVisible(outs), "prefix  suffix");
  ASSERT_EQ(outs[0].events.size(), 3u);
  EXPECT_EQ(std::get<std::string>(outs[0].events[0]), "prefix ");
  EXPECT_EQ(std::get<ParsedToolCall>(outs[0].events[1]).name, "add");
  EXPECT_EQ(std::get<std::string>(outs[0].events[2]), " suffix");
}

TEST(ToolCallStreamAccumulatorTest, SingleToolCallSplitAcrossManyChunks) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");

  // Deliberately split markers and JSON across tiny pieces — exactly what real tokenizers do.
  std::vector<std::string> chunks = {
      "before ",
      "<tool",
      "_call",
      ">",
      "[{\"name\":\"mul\",\"arg",
      "uments\":{\"x\":7,\"y\":6}}]",
      "</tool",
      "_call>",
      " after",
  };
  auto outs = RunChunks(acc, chunks);

  EXPECT_EQ(CollectVisible(outs), "before  after")
      << "Marker and JSON bytes must not leak into visible text";

  auto all = CollectCalls(outs);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].name, "mul");
  EXPECT_NE(all[0].arguments.find("7"), std::string::npos);
  EXPECT_NE(all[0].arguments.find("6"), std::string::npos);
}

TEST(ToolCallStreamAccumulatorTest, MarkerByteByByte) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");

  std::string full =
      R"(<tool_call>{"name":"f","arguments":{}}</tool_call>)";
  std::vector<std::string> chunks;
  chunks.reserve(full.size());
  for (char c : full) {
    chunks.emplace_back(1, c);
  }
  auto outs = RunChunks(acc, chunks);

  EXPECT_TRUE(CollectVisible(outs).empty()) << "Single tool-call block with no surrounding text produces no visible";

  auto all = CollectCalls(outs);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].name, "f");
}

TEST(ToolCallStreamAccumulatorTest, LargeArgumentScansIncrementallyByteByByte) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  const std::string argument(16 * 1024, 'x');
  const std::string generated =
      R"(<tool_call>{"name":"write","arguments":{"content":")" + argument +
      R"("}}</tool_call>)";

  std::vector<ParsedToolCall> calls;
  for (char byte : generated) {
    auto output = acc.Push(std::string(1, byte));
    for (auto& event : output.events) {
      if (auto* call = std::get_if<ParsedToolCall>(&event)) {
        calls.push_back(std::move(*call));
      }
    }
  }
  auto final = acc.Flush();
  for (auto& event : final.events) {
    if (auto* call = std::get_if<ParsedToolCall>(&event)) {
      calls.push_back(std::move(*call));
    }
  }

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "write");
  EXPECT_EQ(calls[0].arguments.size(), argument.size() + 14);
}

// ========================================================================
// Multiple tool calls — sequential blocks, with interspersed visible text.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, TwoSequentialToolCalls) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  std::string chunk =
      R"(<tool_call>{"name":"a","arguments":{}}</tool_call> middle )"
      R"(<tool_call>{"name":"b","arguments":{}}</tool_call>)";
  auto outs = RunChunks(acc, {chunk});

  EXPECT_EQ(CollectVisible(outs), " middle ");

  auto all = CollectCalls(outs);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].name, "a");
  EXPECT_EQ(all[1].name, "b");
}

// ========================================================================
// EOS draining behavior.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, FlushDrainsPendingSuffixWhenOutside) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  // The trailing "<too" looks like a potential start-marker prefix and gets held back across Push calls; Flush
  // must surface it as visible text since no real marker arrives.
  auto outs = RunChunks(acc, {"hello <too"});
  EXPECT_EQ(CollectVisible(outs), "hello <too");
}

TEST(ToolCallStreamAccumulatorTest, UnterminatedToolCallBecomesVisibleOnFlush) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  // Opens a tool-call block but stream ends before the closing marker. Buffered bytes should surface as visible
  // text on Flush so the caller still sees what the model produced.
  auto outs = RunChunks(acc, {"prefix <tool_call>{\"name\":\"truncated"});

  std::string visible = CollectVisible(outs);
  EXPECT_NE(visible.find("prefix "), std::string::npos);
  EXPECT_NE(visible.find("<tool_call>"), std::string::npos);
  EXPECT_NE(visible.find("truncated"), std::string::npos);

  // No tool call should have been emitted — the block never closed.
  for (const auto& o : outs) {
    EXPECT_TRUE(std::none_of(o.events.begin(), o.events.end(), [](const auto& event) {
      return std::holds_alternative<ParsedToolCall>(event);
    }));
  }

  EXPECT_FALSE(acc.InsideToolCall()) << "Flush should leave accumulator in outside state";
}

TEST(ToolCallStreamAccumulatorTest, FlushRecoversCompleteCallWithoutClosingMarker) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  auto outs = RunChunks(acc, {R"(<tool_call>{"name":"complete","arguments":{"value":1}})"});

  EXPECT_TRUE(CollectVisible(outs).empty());
  auto calls = CollectCalls(outs);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "complete");
  EXPECT_EQ(calls[0].arguments, R"({"value":1})");
  EXPECT_FALSE(acc.InsideToolCall());
}

TEST(ToolCallStreamAccumulatorTest, CompletedMalformedToolCallBecomesVisible) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  auto outs = RunChunks(acc, {"before <tool_call>not json</tool_call> after"});

  EXPECT_EQ(CollectVisible(outs), "before <tool_call>not json</tool_call> after");
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, RecoveredMissingOuterBracePreservesDuplicateCustomInputSource) {
  const std::string tools =
      R"([{"type":"function","name":"run","parameters":{"type":"object"}}])";
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>", tools);
  const std::string arguments = R"({ "input":"first", "input":"second" })";
  const std::string generated =
      "<tool_call>{\"name\":\"run\",\"arguments\":" + arguments + "</tool_call>";
  auto outs = RunChunks(acc, {generated});

  EXPECT_TRUE(CollectVisible(outs).empty());
  auto calls = CollectCalls(outs);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].argument_source, arguments);
  EXPECT_EQ(ExtractCustomToolInput(calls[0].argument_source), arguments);
}

TEST(ToolCallStreamAccumulatorTest, RepairedCustomShapesPreserveDuplicateInputSource) {
  const std::string tools =
      R"([{"type":"function","name":"run","parameters":{"type":"object"}}])";
  const std::string arguments = R"({ "input":"first", "input":"sec\u006fnd" })";
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"<tool_call>{\"name\":\"run\",\"args\":" + arguments + "}</tool_call>", arguments},
      {"<tool_call>{\"run\":" + arguments + "}</tool_call>", arguments},
      {R"(<tool_call><run","input":"first", "input":"sec\u006fnd" }</tool_call>)",
       R"({"input":"first", "input":"sec\u006fnd" })"},
  };

  for (const auto& [generated, expected_source] : cases) {
    ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>", tools);
    auto outs = RunChunks(acc, {generated});

    EXPECT_TRUE(CollectVisible(outs).empty()) << generated;
    auto calls = CollectCalls(outs);
    ASSERT_EQ(calls.size(), 1u) << generated;
    EXPECT_EQ(calls[0].argument_source, expected_source);
    EXPECT_EQ(ExtractCustomToolInput(calls[0].argument_source), expected_source);
  }
}

TEST(ToolCallStreamAccumulatorTest, UnadvertisedParametersCallBecomesVisible) {
  const std::string tools =
      R"([{"type":"function","name":"advertised","parameters":{"type":"object"}}])";
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>", tools);
  const std::string generated =
      R"(<tool_call>{"name":"unadvertised","parameters":{"value":1}}</tool_call>)";
  auto outs = RunChunks(acc, {generated});

  EXPECT_EQ(CollectVisible(outs), generated);
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, CompletedToolCallWithoutNameBecomesVisible) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  const std::string generated = R"(<tool_call>{"arguments":{"value":1}}</tool_call>)";
  auto outs = RunChunks(acc, {generated});

  EXPECT_EQ(CollectVisible(outs), generated);
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, CompletedToolCallWithNonStringNameBecomesVisible) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  const std::string generated = R"(<tool_call>{"name":123,"arguments":{}}</tool_call>)";
  auto outs = RunChunks(acc, {generated});

  EXPECT_EQ(CollectVisible(outs), generated);
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, CompletedMixedValidAndInvalidArrayBecomesVisible) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  const std::string generated =
      R"(<tool_call>[{"name":"fn1","arguments":{}},{"name":123}]</tool_call>)";
  auto outs = RunChunks(acc, {"before ", generated, " after"});

  EXPECT_EQ(CollectVisible(outs), "before " + generated + " after")
      << "A partially-invalid block must be preserved whole, not partially parsed";
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, WrongClosingTagWithTrailingTextStaysVisible) {
  std::string tools =
      R"([{"type":"function","name":"exec_command","parameters":{"type":"object",)"
      R"("properties":{"cmd":{"type":"string"}}}}])";
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>", tools, "</think>");
  auto outs = RunChunks(
      acc, {R"(<tool_call>{"function":"exec_command","arguments":{"cmd":"pwd"}</think> explanation)"});

  EXPECT_EQ(CollectVisible(outs),
            R"(<tool_call>{"function":"exec_command","arguments":{"cmd":"pwd"}</think> explanation)");
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, FlushRecoversTerminalWrongClosingTag) {
  std::string tools =
      R"([{"type":"function","name":"exec_command","parameters":{"type":"object"}}])";
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>", tools, "</think>");
  auto outs = RunChunks(
      acc, {R"(<tool_call>{"function":"exec_command","arguments":{"cmd":"pwd"}</think>)"});

  EXPECT_TRUE(CollectVisible(outs).empty());
  auto calls = CollectCalls(outs);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "exec_command");
}

TEST(ToolCallStreamAccumulatorTest, ConfiguredWrongClosingMarkerInsideArgumentIsIgnored) {
  std::string tools =
      R"([{"type":"function","name":"shell","parameters":{"type":"object"}}])";
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>", tools, "</reason>");
  const std::string generated =
      R"(<tool_call>{"name":"shell","arguments":{"cmd":"echo '</reason>'"}</reason>)";
  auto outs = RunChunks(acc, {generated});

  EXPECT_TRUE(CollectVisible(outs).empty());
  auto calls = CollectCalls(outs);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"echo '</reason>'"})");
}

TEST(ToolCallStreamAccumulatorTest, EndMarkerInsideArgumentDoesNotTruncateCall) {
  const std::string generated =
      R"(<tool_call>{"name":"shell","arguments":{"cmd":"grep '</tool_call>' output"}}</tool_call>)";
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  auto outs = RunChunks(acc, {generated});

  EXPECT_TRUE(CollectVisible(outs).empty());
  auto calls = CollectCalls(outs);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"grep '</tool_call>' output"})");
}

TEST(ToolCallStreamAccumulatorTest, NestedRecoveryRecoversSupportedPrefix) {
  std::string tools =
      R"([{"type":"function","name":"exec_command","parameters":{"type":"object"}}])";
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>", tools);
  const std::string malformed_prefix = R"(<tool_call><exec_command","arguments":{"cmd":"ls"})";
  const std::string valid_inner =
      R"(<tool_call>{"name":"exec_command","args":{"cmd":"pwd"}}</tool_call>)";
  auto outs = RunChunks(acc, {malformed_prefix + valid_inner});

  EXPECT_TRUE(CollectVisible(outs).empty());
  auto calls = CollectCalls(outs);
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].name, "exec_command");
  EXPECT_EQ(calls[0].arguments, R"({"cmd":"ls"})");
  EXPECT_EQ(calls[1].name, "exec_command");
  EXPECT_EQ(calls[1].arguments, R"({"cmd":"pwd"})");
}

TEST(ToolCallStreamAccumulatorTest, NestedStartRecoversCompleteFirstCall) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  const std::string generated =
      R"(<tool_call>{"name":"a","arguments":{}})"
      R"(<tool_call>{"name":"b","arguments":{}}</tool_call>)";
  auto outs = RunChunks(acc, {generated});

  EXPECT_TRUE(CollectVisible(outs).empty());
  auto calls = CollectCalls(outs);
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].name, "a");
  EXPECT_EQ(calls[1].name, "b");
}

TEST(ToolCallStreamAccumulatorTest, NestedMarkerInsideUnterminatedStringRemainsVisible) {
  std::string tools =
      R"([{"type":"function","name":"danger","parameters":{"type":"object"}}])";
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>", tools);
  const std::string generated =
      R"(<tool_call>{"name":"shell","arguments":{"cmd":"echo )"
      R"(<tool_call>{\"name\":\"danger\",\"arguments\":{}}</tool_call>)";
  auto outs = RunChunks(acc, {generated});

  EXPECT_EQ(CollectVisible(outs), generated);
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, RecoveredModelCallCanBeAnsweredAndContinued) {
  std::string tools =
      R"([{"type":"function","name":"read_file","parameters":{"type":"object"}}])";
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>", tools);
  auto outs = RunChunks(
      acc, {R"(<tool_call><read_file","arguments":{"path":"README.md"}}</tool_call>)"});
  auto calls = CollectCalls(outs);

  ASSERT_EQ(calls.size(), 1u);
  auto generated = MakeGeneratedToolCall(calls[0].id, calls[0].name, calls[0].arguments);
  ASSERT_TRUE(generated.arguments_usable);

  TranscriptMessage assistant;
  assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  assistant.AppendToolCall(std::move(generated.call));

  ChatTranscript transcript;
  transcript.CommitTurn({TranscriptMessage(FOUNDRY_LOCAL_ROLE_USER, "Read README.md")},
                        std::move(assistant), {});
  ASSERT_TRUE(transcript.IsOutstanding(calls[0].id));

  transcript.CommitTurn({TranscriptMessage::ToolResult(calls[0].id, "Foundry Local")},
                        TranscriptMessage(FOUNDRY_LOCAL_ROLE_ASSISTANT, "The file was read."), {});
  EXPECT_FALSE(transcript.HasOutstandingCalls());
  EXPECT_EQ(transcript.Messages().back().VisibleText(), "The file was read.");
}

// ========================================================================
// InsideToolCall state machine.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, InsideToolCallTransitions) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");

  EXPECT_FALSE(acc.InsideToolCall());

  acc.Push("text ");
  EXPECT_FALSE(acc.InsideToolCall());

  acc.Push("<tool_call>{\"name\":\"f\"");
  EXPECT_TRUE(acc.InsideToolCall());

  acc.Push(",\"arguments\":{}}</tool_call>");
  EXPECT_FALSE(acc.InsideToolCall());
}

// ========================================================================
// Pathological prefix that does not become a marker — must not eat user text.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, FalseStartPrefixReleasesAfterDisambiguation) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");

  // First chunk ends with "<tool" — a real prefix of the start marker. The accumulator must hold it back.
  auto out1 = acc.Push("hello <tool");
  ASSERT_EQ(out1.events.size(), 1u);
  EXPECT_EQ(std::get<std::string>(out1.events[0]), "hello ");

  // Next chunk reveals the prefix was actually part of unrelated XML-ish text. The held-back "<tool" plus the new
  // bytes must flow out as visible.
  auto out2 = acc.Push("box>");
  ASSERT_EQ(out2.events.size(), 1u);
  EXPECT_EQ(std::get<std::string>(out2.events[0]), "<toolbox>");
}
