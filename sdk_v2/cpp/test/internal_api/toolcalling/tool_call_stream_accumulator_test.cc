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

void ExpectExactVisibleWithoutCalls(const std::vector<std::string>& chunks,
                                    const std::string& expected) {
  auto output = RunQwen(chunks);
  EXPECT_EQ(output.visible, expected);
  EXPECT_TRUE(output.calls.empty());
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

TEST(QwenXmlToolCallAccumulatorTest, UnsupportedSchemaKeywordsDisableExactDecoder) {
  const std::vector<nlohmann::json> schemas = {
      {{"type", "object"},
       {"properties", {{"value", {{"type", "string"}, {"pattern", "^[a-z]+$"}}}}}},
      {{"type", "object"},
       {"properties", {{"value", {{"type", "string"}, {"format", "date-time"}}}}}},
      {{"type", "object"},
       {"properties", {{"value", {{"$ref", "#/$defs/value"}}}}},
       {"$defs", {{"value", {{"type", "string"}}}}}},
  };

  for (const auto& parameters : schemas) {
    SCOPED_TRACE(parameters.dump());
    const auto tools = nlohmann::json::array(
                           {{{"type", "function"},
                             {"function", {{"name", "fn"}, {"parameters", parameters}}}}})
                           .dump();

    EXPECT_FALSE(static_cast<bool>(
        CreateQwenXmlToolCallPayloadParser(tools, {{"fn", ToolKind::kFunction}})));
  }
}

TEST(QwenXmlToolCallAccumulatorTest, RequiredAdditionalPropertyDisablesExactDecoder) {
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

  EXPECT_FALSE(static_cast<bool>(
      CreateQwenXmlToolCallPayloadParser(tools, {{"fn", ToolKind::kFunction}})));
}

TEST(QwenXmlToolCallAccumulatorTest, SkippedSerializedDeclarationsDisableExactDecoder) {
  const auto supported = nlohmann::json{
      {"type", "function"},
      {"function",
       {{"name", "fn"},
        {"parameters", {{"type", "object"}, {"properties", nlohmann::json::object()}}}}},
  };
  const auto custom_tools =
      nlohmann::json::array({supported, {{"type", "custom"}, {"name", "raw"}}}).dump();
  EXPECT_FALSE(static_cast<bool>(
      CreateQwenXmlToolCallPayloadParser(
          custom_tools, {{"fn", ToolKind::kFunction}, {"raw", ToolKind::kCustom}})));

  const auto malformed_tools = nlohmann::json::array(
                                   {supported, {{"type", "function"}, {"function", {{"name", 1}}}}})
                                   .dump();
  EXPECT_FALSE(static_cast<bool>(
      CreateQwenXmlToolCallPayloadParser(malformed_tools, {{"fn", ToolKind::kFunction}})));
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

TEST(QwenXmlToolCallAccumulatorTest, ReservedNestedQwenMarkupRemainsExactVisibleText) {
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
    auto output = RunQwen({generated}, tools, kinds);
    EXPECT_TRUE(output.calls.empty()) << body;
    EXPECT_EQ(output.visible, generated) << body;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, InvalidSchemaValuesRejectTheExactCandidate) {
  const std::vector<std::string> generated = {
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=integer>\n1.5\n</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=boolean>\n\"true\"\n</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=array>\n{}\n</parameter>\n</function>\n</tool_call>",
  };

  for (const auto& candidate : generated) {
    auto output = RunQwen({candidate});
    EXPECT_EQ(output.visible, candidate);
    EXPECT_TRUE(output.calls.empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, InvalidNamesAndRequiredParametersRejectWholeBatchExactly) {
  const std::vector<std::string> generated = {
      "<tool_call>\n<function=missing>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\na\n</parameter>\n"
      "<parameter=text>\nb\n</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\na\n</parameter>\n"
      "<parameter=unknown>\nb\n</parameter>\n</function>\n</tool_call>",
  };

  for (const auto& candidate : generated) {
    auto output = RunQwen({candidate});
    EXPECT_EQ(output.visible, candidate);
    EXPECT_TRUE(output.calls.empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, InvalidSecondCallRejectsEntireAdjacentBatch) {
  const std::string generated =
      "<tool_call>\n<function=zero>\n</function>\n</tool_call>\n"
      "<tool_call>\n<function=missing>\n</function>\n</tool_call>";
  auto output = RunQwen({generated});
  EXPECT_EQ(output.visible, generated);
  EXPECT_TRUE(output.calls.empty());
}

TEST(QwenXmlToolCallAccumulatorTest, UnsupportedAndAmbiguousSchemasRemainVisible) {
  const std::vector<std::string> schemas = {
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":["string","null"]}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"oneOf":[{"type":"string"},{"type":"null"}]}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"date"}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"string","enum":["allowed"]}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"array","items":{"type":"string"},"maxItems":1}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"array","items":{"type":"string","enum":["allowed"]}}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"object","properties":1}}}}}])",
      R"([{"type":"function","function":{"name":"bad","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"object","properties":{"nested":{)"
      R"("oneOf":[{"type":"string"},{"type":"integer"}]}}}}}}}])",
  };
  const std::string generated =
      "<tool_call>\n<function=bad>\n<parameter=value>\ntext\n</parameter>\n</function>\n</tool_call>";

  for (const auto& schema : schemas) {
    auto output = RunQwen({generated}, schema, {{"bad", ToolKind::kFunction}});
    EXPECT_EQ(output.visible, generated);
    EXPECT_TRUE(output.calls.empty());
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
      auto output =
          RunQwen({generated}, tools.dump(), {{"duplicate", ToolKind::kFunction}});
      EXPECT_TRUE(output.calls.empty())
          << "duplicate_first=" << duplicate_first
          << ", parameters="
          << (duplicate_parameters.has_value() ? duplicate_parameters->dump()
                                               : "omitted");
      EXPECT_EQ(output.visible, generated);
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
    auto acc =
        MakeQwenAccumulator(tools.dump(), {{"duplicate", ToolKind::kCustom}});
    auto outputs = RunChunks(acc, {generated});

    EXPECT_TRUE(CollectCalls(outputs).empty()) << "invalid_first=" << invalid_first;
    EXPECT_EQ(CollectVisible(outputs), generated);
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
    auto output = RunQwen({generated}, tools, {{"glob", ToolKind::kFunction}});
    EXPECT_TRUE(output.calls.empty()) << body;
    EXPECT_EQ(output.visible, generated) << body;
  }
}

TEST(QwenXmlToolCallAccumulatorTest, NestedAnyOfArrayItemsRemainExactVisibleText) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"collect","parameters":{"type":"object","properties":{)"
      R"("values":{"type":"array","items":{"anyOf":[{"type":"string"},{"type":"integer"}]}}})"
      R"(},"required":["values"]}}}])";
  const std::string generated =
      "<tool_call>\n"
      "<function=collect>\n"
      "<parameter=values>\n"
      "[\"src\",1]\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";

  auto output = RunQwen({generated}, tools, {{"collect", ToolKind::kFunction}});
  EXPECT_TRUE(output.calls.empty());
  EXPECT_EQ(output.visible, generated);

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
  EXPECT_FALSE(AnyMalformed(recovery_outputs));
  EXPECT_EQ(CollectVisible(recovery_outputs), malformed);
  EXPECT_TRUE(CollectCalls(recovery_outputs).empty());

  const auto guided = R"([{"name":"collect","parameters":{"values":["src",1]}}])";
  EXPECT_TRUE(ParseQwenGuidedToolCalls(guided, tools, {{"collect", ToolKind::kFunction}}).empty());
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

TEST(QwenXmlToolCallAccumulatorTest, CustomToolWithNoncanonicalSchemaRemainsVisible) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"custom","parameters":{"type":"object","properties":{}}}}])";
  const std::string generated = "<tool_call>\n<function=custom>\n</function>\n</tool_call>";

  auto output = RunQwen({generated}, tools, {{"custom", ToolKind::kCustom}});
  EXPECT_EQ(output.visible, generated);
  EXPECT_TRUE(output.calls.empty());
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
     MalformedParameterSchemasRejectEntireAdjacentBatchExactly) {
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
    auto output = RunQwen(
        {generated}, tools,
        {{"zero", ToolKind::kFunction}, {"bad", ToolKind::kFunction}});
    EXPECT_EQ(output.visible, generated);
    EXPECT_TRUE(output.calls.empty());
  }
}

TEST(QwenXmlToolCallAccumulatorTest, ExcessiveArraySchemaNestingRemainsExactVisibleText) {
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
  auto output = RunQwen({generated}, tools, {{"deep", ToolKind::kFunction}});
  EXPECT_TRUE(output.calls.empty());
  EXPECT_EQ(output.visible, generated);
}

TEST(QwenXmlToolCallAccumulatorTest, ParameterWithoutSchemaRejectsEntireAdjacentBatchExactly) {
  const auto tools =
      R"([{"type":"function","function":{"name":"zero","parameters":{"type":"object"}}}])";
  const std::string generated =
      "<tool_call>\n<function=zero>\n<parameter=unknown>\nvalue\n</parameter>\n"
      "</function>\n</tool_call>\n" +
      kValidZeroQwenCall;
  auto output = RunQwen({generated}, tools, {{"zero", ToolKind::kFunction}});
  EXPECT_EQ(output.visible, generated);
  EXPECT_TRUE(output.calls.empty());
}

TEST(QwenXmlToolCallAccumulatorTest, MalformedAndIncompleteCandidatesRemainExactVisibleText) {
  const std::vector<std::string> generated = {
      "<tool_call>\r\n<function=zero>\r\n</function>\r\n</tool_call>",
      "<tool_call attribute=x>\n<function=zero>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nprefix\n</parameter>\nsuffix\n"
      "</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<param=text>\nx\n</param>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nx\n</function>\n</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\ntruncated",
  };

  for (const auto& candidate : generated) {
    auto output = RunQwen({candidate});
    EXPECT_EQ(output.visible, candidate);
    EXPECT_TRUE(output.calls.empty());
  }
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

TEST(QwenXmlToolCallAccumulatorTest, RecoveryAwareOrdinaryRejectionsRemainExactVisibleText) {
  const std::vector<std::string> generated = {
      "<tool_call>\n<function=missing>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=unknown>\nvalue\n</parameter>\n</function>\n</tool_call>",
      "<tool_call>\n<function=typed>\n<parameter=text>\nok\n</parameter>\n"
      "<parameter=integer>\n1.5\n</parameter>\n</function>\n</tool_call>",
  };

  for (const auto& candidate : generated) {
    auto accumulator = MakeQwenAccumulator(kQwenTools, kQwenToolKinds, /*recovery_aware=*/true);
    auto outputs = RunChunks(accumulator, {candidate});

    EXPECT_FALSE(AnyMalformed(outputs)) << candidate;
    EXPECT_EQ(CollectVisible(outputs), candidate);
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

TEST(QwenXmlToolCallAccumulatorTest, OversizedCallRejectsWhitespaceAdjacentCallInOneChunk) {
  const auto generated =
      MakeOversizedQwenCall() + " \n\t" + kValidZeroQwenCall + "\n" + kValidZeroQwenCall;

  ExpectExactVisibleWithoutCalls({generated}, generated);
}

TEST(QwenXmlToolCallAccumulatorTest, OversizedCallRejectsWhitespaceAdjacentCallAcrossStructuralSplits) {
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
    ExpectExactVisibleWithoutCalls(SplitAt(generated, split), generated);
  }
}

TEST(QwenXmlToolCallAccumulatorTest, OversizedCallRejectsWhitespaceAdjacentCallByteAtATime) {
  const auto generated = MakeOversizedQwenCall() + " \n\t" + kValidZeroQwenCall;

  ExpectExactVisibleWithoutCalls(SplitIntoBytes(generated), generated);
}

TEST(QwenXmlToolCallAccumulatorTest, OversizedCandidateDoesNotHideIndependentLaterCallAcrossEverySplit) {
  const auto oversized = MakeOversizedQwenCall();
  const std::string generated = oversized + " visible " + kValidZeroQwenCall + " tail";

  for (size_t split = 0; split <= generated.size(); ++split) {
    auto output = RunQwen(SplitAt(generated, split));
    ASSERT_EQ(output.calls.size(), 1u) << "split=" << split;
    EXPECT_EQ(output.calls[0].name, "zero") << "split=" << split;
    EXPECT_EQ(output.visible, oversized + " visible  tail") << "split=" << split;
  }
}

std::string MakeBoundaryStraddledOversizedCandidate() {
  constexpr std::string_view closing_prefix = "</tool_";
  const auto padding_size =
      64 * 1024 - std::string_view("<tool_call>").size() - closing_prefix.size();
  return "<tool_call>" + std::string(padding_size, 'x') +
         std::string(closing_prefix) + "call>";
}

void ExpectBoundaryStraddledOversizeRecovery(const std::vector<std::string>& chunks,
                                             const std::string& rejected) {
  auto output = RunQwen(chunks);
  ASSERT_EQ(output.calls.size(), 1u);
  EXPECT_EQ(output.calls.front().name, "zero");
  EXPECT_EQ(output.calls.front().arguments, "{}");
  EXPECT_EQ(output.visible, rejected + " visible  tail");
}

TEST(QwenXmlToolCallAccumulatorTest, BoundaryStraddledOversizeClosePreservesLaterCallInOneChunk) {
  const auto rejected = MakeBoundaryStraddledOversizedCandidate();
  const auto generated = rejected + " visible " + kValidZeroQwenCall + " tail";

  ExpectBoundaryStraddledOversizeRecovery({generated}, rejected);
}

TEST(QwenXmlToolCallAccumulatorTest, BoundaryStraddledOversizeClosePreservesLaterCallAtEveryCloseSplit) {
  const auto rejected = MakeBoundaryStraddledOversizedCandidate();
  const auto generated = rejected + " visible " + kValidZeroQwenCall + " tail";
  constexpr auto close_size = std::string_view("</tool_call>").size();
  const auto close_start = rejected.size() - close_size;

  for (size_t offset = 0; offset <= close_size; ++offset) {
    SCOPED_TRACE("offset=" + std::to_string(offset));
    ExpectBoundaryStraddledOversizeRecovery(
        SplitAt(generated, close_start + offset), rejected);
  }
}

TEST(QwenXmlToolCallAccumulatorTest, BoundaryStraddledOversizeClosePreservesLaterCallByteAtATime) {
  const auto rejected = MakeBoundaryStraddledOversizedCandidate();
  const auto generated = rejected + " visible " + kValidZeroQwenCall + " tail";

  ExpectBoundaryStraddledOversizeRecovery(SplitIntoBytes(generated), rejected);
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

TEST(QwenXmlToolCallAccumulatorTest, ExactlyLimitSizedCandidateRejectsWhitespaceAdjacentBatchAtEveryMarkerSplit) {
  const std::string separator = " \n\t";
  constexpr size_t partial_marker_size = 5;
  const auto candidate =
      MakeSizedQwenCall(kSelectedPayloadBufferLimit - separator.size() - partial_marker_size);
  const auto generated = candidate + separator + kValidZeroQwenCall;
  const auto adjacent_start = candidate.size() + separator.size();

  ExpectExactVisibleWithoutCalls({generated}, generated);
  for (size_t offset = 0; offset <= std::string_view("<tool_call>").size(); ++offset) {
    SCOPED_TRACE("offset=" + std::to_string(offset));
    ExpectExactVisibleWithoutCalls(SplitAt(generated, adjacent_start + offset), generated);
  }
  ExpectExactVisibleWithoutCalls(SplitIntoBytes(generated), generated);
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

TEST(QwenXmlToolCallAccumulatorTest, CandidateOneByteOverLimitRemainsExactVisibleText) {
  const auto candidate = MakeSizedQwenCall(kSelectedPayloadBufferLimit + 1);

  ExpectExactVisibleWithoutCalls({candidate}, candidate);
  ExpectExactVisibleWithoutCalls(
      SplitAt(candidate, kSelectedPayloadBufferLimit), candidate);
  ExpectExactVisibleWithoutCalls(SplitIntoBytes(candidate), candidate);
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
