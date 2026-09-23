// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for Session (base) and ChatSession.
// Unit tests validate state management without a model.
// Integration tests run actual inference against the shared test model.

#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "inferencing/generative/chat/stop_strings.h"
#include "inferencing/generative/openresponses/response_converter.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "contracts/tool_definitions.h"
#include "exception.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/session/request.h"
#include "inferencing/session/tool_registry.h"
#include "inferencing/generative/toolcalling/qwen_xml_tool_call_decoder.h"
#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "model.h"
#include "internal_api/null_session_manager.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "utils/string_utils.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

using namespace fl;

namespace {

using Segment = ReasoningStreamSplitter::Segment;

class RecordingLogger final : public ILogger {
 public:
  void Log(LogLevel level, std::string_view message) override {
    entries.emplace_back(level, message);
  }

  std::vector<std::pair<LogLevel, std::string>> entries;
};

struct GenerationTrace {
  bool requires_cancellation = false;
  bool canceled = false;
  bool usage_requested = false;
  bool canceled_before_usage = false;
  int generated_tokens = 0;
};

struct GeneratorCounters {
  int created = 0;
  int destroyed = 0;
  int appended = 0;
  int canceled = 0;
  int closed = 0;
  int usage_after_cancel = 0;
};

class FixedOutputGenerator final : public ChatGenerator {
 public:
  FixedOutputGenerator(std::string output, BackendTerminationCause cause,
                       bool prompt_opens_reasoning = false,
                       std::shared_ptr<GeneratorCounters> counters = {},
                       int prompt_tokens = 4,
                       int generated_tokens = 1,
                       std::function<void()> generate_hook = {},
                       GenerationTrace* trace = nullptr)
      : output_(std::move(output)),
        cause_(cause),
        prompt_opens_reasoning_(prompt_opens_reasoning),
        counters_(std::move(counters)),
        prompt_tokens_(prompt_tokens),
        generated_tokens_(generated_tokens),
        generate_hook_(std::move(generate_hook)),
        trace_(trace) {
    if (counters_) {
      ++counters_->created;
    }
  }

  FixedOutputGenerator(std::string output, BackendTerminationCause cause,
                       bool prompt_opens_reasoning, GenerationTrace* trace)
      : FixedOutputGenerator(std::move(output), cause, prompt_opens_reasoning, {}, 4, 1, {}, trace) {}

  ~FixedOutputGenerator() override {
    if (counters_) {
      ++counters_->destroyed;
    }
  }

  bool IsDone() const override {
    return canceled_ || (generated_ && !(trace_ && trace_->requires_cancellation));
  }

  void GenerateNextToken() override {
    if (generated_) {
      throw std::logic_error("The host must end this turn after the first fragment");
    }

    generated_ = true;
    current_token_ = 1;
    if (trace_) {
      ++trace_->generated_tokens;
    }
    if (generate_hook_) {
      generate_hook_();
    }
  }

  std::string Decode() override {
    current_token_.reset();
    return output_;
  }

  std::optional<int32_t> CurrentTokenId() const override {
    return current_token_;
  }

  int TokenCount() const override {
    return prompt_tokens_ + (generated_ ? generated_tokens_ : 0);
  }

  int PromptTokenCount() const override {
    return prompt_tokens_;
  }

  void Cancel() override {
    canceled_ = true;
    if (trace_) {
      trace_->canceled = true;
    }
    if (counters_) {
      ++counters_->canceled;
    }
  }

  void Close() override {
    if (!closed_ && counters_) {
      ++counters_->closed;
    }
    closed_ = true;
  }

  int AppendMessages(const std::vector<TranscriptMessage>&,
                     const chat_internal::PreparedChatMessages&,
                     GenAIModelInstance&,
                     const ToolCallContext&,
                     const SearchOptions&) override {
    if (counters_) {
      ++counters_->appended;
    }

    generated_ = false;
    canceled_ = false;
    current_token_.reset();
    return 1;
  }

  std::optional<ChatTurnUsage> GetTurnUsage() const override {
    if (trace_) {
      trace_->usage_requested = true;
      trace_->canceled_before_usage = canceled_;
    }
    if (counters_ && canceled_) {
      ++counters_->usage_after_cancel;
    }

    return ChatTurnUsage{
        prompt_tokens_,
        generated_ ? generated_tokens_ : 0,
        cause_ == BackendTerminationCause::kNaturalEnd ? FOUNDRY_LOCAL_FINISH_STOP
                                                       : FOUNDRY_LOCAL_FINISH_LENGTH,
        canceled_ ? BackendTerminationCause::kCancellation : cause_,
    };
  }

  bool PromptOpensReasoning() const override {
    return prompt_opens_reasoning_;
  }

 private:
  std::string output_;
  BackendTerminationCause cause_;
  bool prompt_opens_reasoning_;
  bool generated_ = false;
  bool canceled_ = false;
  bool closed_ = false;
  std::optional<int32_t> current_token_;
  std::shared_ptr<GeneratorCounters> counters_;
  int prompt_tokens_;
  int generated_tokens_;
  std::function<void()> generate_hook_;
  GenerationTrace* trace_;
};

constexpr std::string_view kNativeQwenChatTemplate =
    R"({% for message in messages %})"
    R"({% if message.role == 'user' %})"
    R"({{ '<|im_start|>user\n' + message.content + '<|im_end|>\n' }})"
    R"({% elif message.role == 'assistant' %})"
    R"({{ '<|im_start|>assistant\n<think>\n\n</think>\n\n' }})"
    R"({% for call in message.tool_calls %})"
    R"({{ '<tool_call>\n<function=' + call.function.name + '>\n' }})"
    R"({% for name, value in call.function.arguments.items() %})"
    R"({{ '<parameter=' + name + '>\n' + value + '\n</parameter>\n' }})"
    R"({% endfor %}{{ '</function>\n</tool_call>' }})"
    R"({% if not loop.last %}{{ '\n' }}{% endif %}{% endfor %})"
    R"({{ '<|im_end|>\n' }})"
    R"({% elif message.role == 'tool' %})"
    R"({% if loop.first or messages[loop.index0 - 1].role != 'tool' %})"
    R"({{ '<|im_start|>user\n' }}{% endif %})"
    R"({{ '<tool_response>\n' + message.content + '\n</tool_response>' }})"
    R"({% if loop.last or messages[loop.index0 + 1].role != 'tool' %})"
    R"({{ '<|im_end|>\n' }}{% else %}{{ '\n' }}{% endif %})"
    R"({% endif %}{% endfor %})";

constexpr std::string_view kLookupCall =
    "<tool_call>\n"
    "<function=lookup>\n"
    "<parameter=city>\n"
    "Paris\n"
    "</parameter>\n"
    "</function>\n"
    "</tool_call>";

constexpr std::string_view kSecondCall =
    "<tool_call>\n"
    "<function=clock>\n"
    "<parameter=zone>\n"
    "UTC\n"
    "</parameter>\n"
    "</function>\n"
    "</tool_call>";

void AppendSegments(std::vector<Segment>& destination, const std::vector<Segment>& source) {
  for (const auto& segment : source) {
    if (!destination.empty() && destination.back().type == segment.type) {
      destination.back().text += segment.text;
    } else {
      destination.push_back(segment);
    }
  }
}

template <typename Action>
void ExpectOperationCancelled(Action&& action) {
  try {
    action();
    FAIL() << "Expected operation cancellation";
  } catch (const fl::Exception& error) {
    EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  }
}

std::vector<ToolCallStreamAccumulator::Event> RunToolOutput(
    const std::vector<std::string>& chunks, const std::string& tools = {},
    const std::unordered_map<std::string, ToolKind>& kinds = {}) {
  RawEnvelopeDetector raw_detector(
      {"apply_patch", "*** Begin Patch", "*** End Patch"});
  auto structured_accumulator =
      tools.empty()
          ? ToolCallStreamAccumulator("<tool_call>", "</tool_call>")
          : ToolCallStreamAccumulator(
                "<tool_call>", "</tool_call>", tools, "",
                CreateQwenXmlToolCallPayloadParser(tools, kinds));
  std::vector<ToolCallStreamAccumulator::Event> events;
  const auto append = [&](ToolCallStreamAccumulator::Output output) {
    events.insert(events.end(),
                  std::make_move_iterator(output.events.begin()),
                  std::make_move_iterator(output.events.end()));
  };

  for (const auto& chunk : chunks) {
    append(chat_session_internal::PushToolOutput(
        chunk, &raw_detector, structured_accumulator));
  }
  append(chat_session_internal::FlushToolOutput(
      &raw_detector, structured_accumulator));
  return events;
}

}  // namespace

TEST(ChatSessionDecisionTest, HostOutputLimitTruncatesOnlyAnUnfinishedBackendAtTheBoundary) {
  using chat_session_internal::DidHostOutputLimitTruncate;

  EXPECT_FALSE(DidHostOutputLimitTruncate(/*output_tokens=*/31, /*max_output_tokens=*/32,
                                          /*backend_finished=*/false));
  EXPECT_TRUE(DidHostOutputLimitTruncate(/*output_tokens=*/32, /*max_output_tokens=*/32,
                                         /*backend_finished=*/false));
  EXPECT_FALSE(DidHostOutputLimitTruncate(/*output_tokens=*/32, /*max_output_tokens=*/32,
                                          /*backend_finished=*/true));
}

TEST(ChatSessionDecisionTest, JsonToolContextUsesOnlyTheCapturedSessionSnapshot) {
  ToolRegistry registry;
  const auto captured = registry.Definitions();

  std::thread registration([&] {
    registry.Add({"late_custom", "registered after capture", "", ToolKind::kCustom});
  });
  registration.join();

  std::vector<ToolDefinition> definitions{
      {"payload_tool", {}, "{}", ToolKind::kFunction}};
  const auto local =
      chat_session_internal::BuildJsonRequestToolDefinitions(definitions, captured);

  ASSERT_EQ(local.size(), 1u);
  EXPECT_EQ(local[0].name, "payload_tool");
  EXPECT_EQ(local[0].json_schema, "{}");
  EXPECT_EQ(local[0].kind, ToolKind::kFunction);

  // A fresh snapshot sees the custom registration and preserves JSON request isolation.
  EXPECT_THROW(chat_session_internal::BuildJsonRequestToolDefinitions(
                   definitions, registry.Definitions()),
               fl::Exception);
}

TEST(ChatSessionDecisionTest, EmptyJsonToolsIgnoreSessionFunctionAndCustomDefinitions) {
  const std::vector<ToolDefinition> function_definitions{
      {"lookup", "Look up a value.", R"({"type":"object"})", ToolKind::kFunction}};
  const std::vector<ToolDefinition> custom_definitions{
      {"apply_patch", "Apply a patch.", "", ToolKind::kCustom}};

  EXPECT_TRUE(
      chat_session_internal::BuildJsonRequestToolDefinitions(
          std::vector<ToolDefinition>{}, function_definitions)
          .empty());
  EXPECT_TRUE(
      chat_session_internal::BuildJsonRequestToolDefinitions(
          std::vector<ToolDefinition>{}, custom_definitions)
          .empty());
}

TEST(ChatSessionDecisionTest, SupportedStrictFalseSurvivesPromptSerialization) {
  std::vector<ToolDefinition> definitions{
      {"strict_false", "", R"({"type":"object"})", ToolKind::kFunction, false, true, false},
      {"unspecified", "", "{}", ToolKind::kFunction, false, false}};
  ToolCallContext context;

  chat_session_internal::PopulateToolDefinitions(definitions, context);

  const auto tools = nlohmann::json::parse(context.tools_json);
  EXPECT_EQ(tools[0], nlohmann::json::parse(
                          R"({"type":"function","function":{"name":"strict_false",)"
                          R"("parameters":{"type":"object"},"strict":false}})"));
  EXPECT_FALSE(tools[1]["function"].contains("strict"));
  EXPECT_FALSE(tools[1]["function"].contains("parameters"));
}

TEST(ChatSessionDecisionTest, StockApplyPatchGrammarDeclaresBuiltInRawDescriptor) {
  ToolCallContext context;
  context.tool_output = true;
  context.text_output = true;
  context.tool_kinds = {{"apply_patch", ToolKind::kCustom}};
  context.custom_lark_grammars = {
      {"apply_patch", std::string(tools::kStockGhcpApplyPatchLarkGrammar)}};

  chat_session_internal::ResolveBuiltInRawEnvelope(context);

  ASSERT_TRUE(context.raw_envelope.has_value());
  EXPECT_EQ(context.raw_envelope->tool_name, "apply_patch");
  EXPECT_EQ(context.raw_envelope->start_marker, "*** Begin Patch");
  EXPECT_EQ(context.raw_envelope->end_marker, "*** End Patch");
  EXPECT_NE(context.ActiveRawEnvelope(), nullptr);
}

TEST(ChatSessionDecisionTest, StockApplyPatchGrammarRejectsConflictingMetadataDescriptor) {
  ToolCallContext context;
  context.custom_lark_grammars = {
      {"apply_patch", std::string(tools::kStockGhcpApplyPatchLarkGrammar)}};
  context.raw_envelope =
      RawEnvelopeDescriptor{"apply_patch", "BEGIN", "END"};

  EXPECT_THROW(chat_session_internal::ResolveBuiltInRawEnvelope(context), fl::Exception);
}

TEST(ChatSessionDecisionTest, ForcedRawGuidanceUsesMatchingGrammarOrDisablesGuidance) {
  RecordingLogger logger;
  ToolCallContext with_grammar;
  with_grammar.tool_output = true;
  with_grammar.text_output = false;
  with_grammar.tool_kinds = {{"edit", ToolKind::kCustom}};
  with_grammar.raw_envelope = RawEnvelopeDescriptor{"edit", "BEGIN", "END"};
  with_grammar.forced_tool = ForcedToolChoice{"edit", ToolKind::kCustom};
  with_grammar.custom_lark_grammars = {{"edit", "start: \"BEGIN\" /(.|\\n)+/ \"END\""}};
  with_grammar.guidance_type = "lark_grammar";
  with_grammar.guidance_data = "start: \"BEGIN\" /(.|\\n)+/ \"END\"";

  chat_session_internal::ApplyRawEnvelopeGuidance(with_grammar, logger);

  EXPECT_EQ(with_grammar.guidance_type, "lark_grammar");
  EXPECT_EQ(with_grammar.guidance_data, "start: \"BEGIN\" /(.|\\n)+/ \"END\"");
  EXPECT_FALSE(with_grammar.guidance_disabled);
  EXPECT_TRUE(logger.entries.empty());

  ToolCallContext without_grammar = with_grammar;
  without_grammar.custom_lark_grammars.clear();
  without_grammar.guidance_type = "json_schema";
  without_grammar.guidance_data = "{}";

  chat_session_internal::ApplyRawEnvelopeGuidance(without_grammar, logger);

  EXPECT_TRUE(without_grammar.guidance_type.empty());
  EXPECT_TRUE(without_grammar.guidance_data.empty());
  EXPECT_TRUE(without_grammar.guidance_disabled);
  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_EQ(logger.entries.front().first, LogLevel::Debug);
  EXPECT_NE(logger.entries.front().second.find("json_schema"), std::string::npos);
  EXPECT_NE(logger.entries.front().second.find("edit"), std::string::npos);
}

TEST(ChatSessionDecisionTest, ForcedMatchingRawEnvelopeStartsOutsidePromptOpenedReasoning) {
  ToolCallContext built_in;
  built_in.tool_output = true;
  built_in.text_output = false;
  built_in.tool_kinds = {{"apply_patch", ToolKind::kCustom}};
  built_in.custom_lark_grammars = {
      {"apply_patch", std::string(tools::kStockGhcpApplyPatchLarkGrammar)}};
  built_in.forced_tool = ForcedToolChoice{"apply_patch", ToolKind::kCustom};
  chat_session_internal::ResolveBuiltInRawEnvelope(built_in);
  RecordingLogger logger;
  chat_session_internal::ApplyRawEnvelopeGuidance(built_in, logger);

  EXPECT_TRUE(built_in.HasForcedRawEnvelope());
  EXPECT_FALSE(chat_session_internal::ShouldStartInsideReasoning(
      built_in, /*prompt_opens_reasoning=*/true));

  ToolCallContext explicit_descriptor;
  explicit_descriptor.tool_output = true;
  explicit_descriptor.text_output = false;
  explicit_descriptor.tool_kinds = {{"edit", ToolKind::kCustom}};
  explicit_descriptor.raw_envelope = RawEnvelopeDescriptor{"edit", "BEGIN", "END"};
  explicit_descriptor.forced_tool = ForcedToolChoice{"edit", ToolKind::kCustom};

  EXPECT_TRUE(explicit_descriptor.HasForcedRawEnvelope());
  EXPECT_TRUE(chat_session_internal::ShouldStartInsideReasoning(
      explicit_descriptor, /*prompt_opens_reasoning=*/true));
}

TEST(ChatSessionDecisionTest, NonForcedOrInactiveRawEnvelopeRetainsPromptOpenedReasoning) {
  ToolCallContext context;
  context.tool_output = true;
  context.text_output = true;
  context.tool_kinds = {{"apply_patch", ToolKind::kCustom}};
  context.custom_lark_grammars = {
      {"apply_patch", std::string(tools::kStockGhcpApplyPatchLarkGrammar)}};
  chat_session_internal::ResolveBuiltInRawEnvelope(context);

  EXPECT_FALSE(context.HasForcedRawEnvelope());
  EXPECT_TRUE(chat_session_internal::ShouldStartInsideReasoning(
      context, /*prompt_opens_reasoning=*/true));

  context.forced_tool = ForcedToolChoice{"other", ToolKind::kCustom};
  EXPECT_TRUE(chat_session_internal::ShouldStartInsideReasoning(
      context, /*prompt_opens_reasoning=*/true));

  context.forced_tool = ForcedToolChoice{"apply_patch", ToolKind::kFunction};
  EXPECT_TRUE(chat_session_internal::ShouldStartInsideReasoning(
      context, /*prompt_opens_reasoning=*/true));

  context.forced_tool = ForcedToolChoice{"apply_patch", ToolKind::kCustom};
  context.tool_output = false;
  EXPECT_TRUE(chat_session_internal::ShouldStartInsideReasoning(
      context, /*prompt_opens_reasoning=*/true));
}

TEST(ChatSessionDecisionTest, QwenXmlParserIsSelectedOnlyForNativeAutoToolOutput) {
  ToolCallContext context;
  context.tool_output = true;
  context.text_output = true;
  context.tools_json =
      R"([{"type":"function","function":{"name":"lookup","parameters":{"type":"object","properties":{}}}}])";
  context.tool_kinds = {{"lookup", ToolKind::kFunction}};
  context.tool_call_start = std::string(kQwenXmlToolCallStartMarker);
  context.tool_call_end = std::string(kQwenXmlToolCallEndMarker);

  EXPECT_TRUE(chat_session_internal::ShouldUseQwenXmlToolCallParser(
      context, /*has_native_qwen_xml_tool_calls=*/true));
  EXPECT_FALSE(chat_session_internal::ShouldUseQwenXmlToolCallParser(
      context, /*has_native_qwen_xml_tool_calls=*/false));

  context.tool_output = false;
  EXPECT_FALSE(chat_session_internal::ShouldUseQwenXmlToolCallParser(
      context, /*has_native_qwen_xml_tool_calls=*/true));
  context.tool_output = true;

  context.text_output = false;
  EXPECT_FALSE(chat_session_internal::ShouldUseQwenXmlToolCallParser(
      context, /*has_native_qwen_xml_tool_calls=*/true));
  context.text_output = true;

  context.forced_tool = ForcedToolChoice{"lookup", ToolKind::kFunction};
  EXPECT_FALSE(chat_session_internal::ShouldUseQwenXmlToolCallParser(
      context, /*has_native_qwen_xml_tool_calls=*/true));
  context.forced_tool.reset();

  context.guidance_type = "json_schema";
  EXPECT_FALSE(chat_session_internal::ShouldUseQwenXmlToolCallParser(
      context, /*has_native_qwen_xml_tool_calls=*/true));
  context.guidance_type.clear();

  context.tool_call_start = "<tool>";
  EXPECT_FALSE(chat_session_internal::ShouldUseQwenXmlToolCallParser(
      context, /*has_native_qwen_xml_tool_calls=*/true));
  context.tool_call_start = std::string(kQwenXmlToolCallStartMarker);

  context.tool_call_end = "</tool>";
  EXPECT_FALSE(chat_session_internal::ShouldUseQwenXmlToolCallParser(
      context, /*has_native_qwen_xml_tool_calls=*/true));
  context.tool_call_end = std::string(kQwenXmlToolCallEndMarker);

  context.tools_json.clear();
  EXPECT_FALSE(chat_session_internal::ShouldUseQwenXmlToolCallParser(
      context, /*has_native_qwen_xml_tool_calls=*/true));
}

TEST(ChatSessionDecisionTest, QwenXmlParserUsesAuthoritativeKindsToDecodeCustomTools) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"lookup","parameters":{"type":"object","properties":{}}}},)"
      R"({"type":"function","function":{"name":"custom","parameters":{"type":"object",)"
      R"("properties":{"input":{"type":"string"}},"required":["input"],"additionalProperties":false}}}])";
  const std::unordered_map<std::string, ToolKind> kinds = {
      {"lookup", ToolKind::kFunction},
      {"custom", ToolKind::kCustom},
  };
  ToolCallStreamAccumulator accumulator(
      "<tool_call>", "</tool_call>", tools, "",
      CreateQwenXmlToolCallPayloadParser(tools, kinds));
  const std::string generated =
      "<tool_call>\n"
      "<function=custom>\n"
      "<parameter=input>\n"
      "raw payload\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";

  auto output = accumulator.Push(generated);
  auto terminal = accumulator.Flush();
  output.events.insert(output.events.end(),
                       std::make_move_iterator(terminal.events.begin()),
                       std::make_move_iterator(terminal.events.end()));

  ToolCallContext context;
  context.tool_kinds = kinds;
  chat_session_internal::NormalizeToolOutputBatch(output, context);

  ASSERT_EQ(output.events.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<ParsedToolCall>(output.events.front()));
  const auto& call = std::get<ParsedToolCall>(output.events.front());
  EXPECT_EQ(call.name, "custom");
  EXPECT_EQ(call.arguments, "raw payload");
}

TEST(ChatSessionDecisionTest, InvalidLaterCustomCallPreventsTheWholeBatchFromStreaming) {
  ToolCallContext context;
  context.tool_kinds = {{"custom", ToolKind::kCustom}};

  ToolCallStreamAccumulator::Output output;
  ParsedToolCall valid{"call_1", "custom", R"({"input":"valid"})"};
  valid.argument_source = valid.arguments;
  output.events.emplace_back(std::move(valid));

  ParsedToolCall invalid{"call_2", "custom", R"({"input":"invalid\u0000payload"})"};
  invalid.argument_source = invalid.arguments;
  output.events.emplace_back(std::move(invalid));

  size_t streamed_calls = 0;
  EXPECT_THROW(
      {
        chat_session_internal::NormalizeToolOutputBatch(output, context);
        for (const auto& event : output.events) {
          if (std::holds_alternative<ParsedToolCall>(event)) {
            ++streamed_calls;
          }
        }
      },
      fl::Exception);
  EXPECT_EQ(streamed_calls, 0u);
}

TEST(ChatSessionDecisionTest, CustomBatchNormalizationUsesTheCompleteArgumentSource) {
  ToolCallContext context;
  context.tool_kinds = {{"custom", ToolKind::kCustom}};

  ToolCallStreamAccumulator::Output output;

  ParsedToolCall wrapper{"call_1", "custom", "parser projection"};
  wrapper.argument_source = R"({"input":"plain text"})";
  output.events.emplace_back(std::move(wrapper));

  ParsedToolCall extra_member{"call_2", "custom", "parser projection"};
  extra_member.argument_source = R"({"input":"plain text","extra":true})";
  output.events.emplace_back(std::move(extra_member));

  ParsedToolCall json_string{"call_3", "custom", "parser projection"};
  json_string.argument_source = R"({"input":"{\"looks\":\"json\"}"})";
  output.events.emplace_back(std::move(json_string));

  chat_session_internal::NormalizeToolOutputBatch(output, context);

  EXPECT_EQ(std::get<ParsedToolCall>(output.events[0]).arguments, "plain text");
  EXPECT_EQ(std::get<ParsedToolCall>(output.events[1]).arguments,
            R"({"input":"plain text","extra":true})");
  EXPECT_EQ(std::get<ParsedToolCall>(output.events[2]).arguments, R"({"looks":"json"})");
}

TEST(ChatSessionDecisionTest, CustomBatchNormalizationPreservesRawEnvelopeBytes) {
  const std::string envelope = "{\n\"input\":\"replacement text\"\n}";
  ToolCallContext context;
  context.tool_kinds = {{"edit", ToolKind::kCustom}};

  ToolCallStreamAccumulator::Output output;
  ParsedToolCall raw_call{"call_1", "edit", envelope};
  raw_call.argument_source = envelope;
  raw_call.raw_envelope = true;
  output.events.emplace_back(std::move(raw_call));

  chat_session_internal::NormalizeToolOutputBatch(output, context);

  const auto& normalized = std::get<ParsedToolCall>(output.events.front());
  EXPECT_EQ(normalized.arguments, envelope);
  EXPECT_EQ(normalized.argument_source, envelope);
}

TEST(ChatSessionDecisionTest, RawEligibleOutputStillRecognizesStructuredCalls) {
  RawEnvelopeDetector raw_detector(
      {"apply_patch", "*** Begin Patch", "*** End Patch"});
  ToolCallStreamAccumulator structured_accumulator("<tool_call>", "</tool_call>");
  std::vector<ToolCallStreamAccumulator::Event> events;

  const auto append = [&](ToolCallStreamAccumulator::Output output) {
    events.insert(events.end(),
                  std::make_move_iterator(output.events.begin()),
                  std::make_move_iterator(output.events.end()));
  };

  append(chat_session_internal::PushToolOutput(
      "<tool_call>{\"name\":\"lookup\",\n", &raw_detector,
      structured_accumulator));
  EXPECT_TRUE(chat_session_internal::InsideToolOutput(
      &raw_detector, structured_accumulator));
  append(chat_session_internal::PushToolOutput(
      R"("arguments":{"key":"alpha"}}</tool_call>)",
      &raw_detector, structured_accumulator));
  append(chat_session_internal::FlushToolOutput(
      &raw_detector, structured_accumulator));

  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<ParsedToolCall>(events.front()));
  const auto& call = std::get<ParsedToolCall>(events.front());
  EXPECT_EQ(call.name, "lookup");
  EXPECT_EQ(call.arguments, R"({"key":"alpha"})");
  EXPECT_FALSE(call.raw_envelope);
}

TEST(ChatSessionDecisionTest, IncompleteRawCandidateCannotPromoteNestedStructuredCallAtTerminalFlush) {
  RawEnvelopeDetector raw_detector(
      {"apply_patch", "*** Begin Patch", "*** End Patch"});
  ToolCallStreamAccumulator structured_accumulator("<tool_call>", "</tool_call>");
  const std::string input =
      "*** Begin Patch\n"
      R"(<tool_call>{"name":"lookup","arguments":{"key":"nested"}}</tool_call>)"
      "\n";

  EXPECT_TRUE(chat_session_internal::PushToolOutput(
                  input, &raw_detector, structured_accumulator)
                  .events.empty());

  const auto output = chat_session_internal::FlushToolOutput(
      &raw_detector, structured_accumulator);
  ASSERT_EQ(output.events.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<std::string>(output.events.front()));
  EXPECT_EQ(std::get<std::string>(output.events.front()), input);
}

TEST(ChatSessionDecisionTest, OverLimitRawCandidateCannotPromoteNestedStructuredCall) {
  RawEnvelopeDetector raw_detector(
      {"apply_patch", "*** Begin Patch", "*** End Patch"});
  ToolCallStreamAccumulator structured_accumulator("<tool_call>", "</tool_call>");
  const std::string input =
      std::string("*** Begin Patch\n") +
      R"(<tool_call>{"name":"lookup","arguments":{"key":"nested"}}</tool_call>)" +
      std::string(RawEnvelopeDetector::kMaxBufferedBytes, 'x');

  auto output = chat_session_internal::PushToolOutput(
      input, &raw_detector, structured_accumulator);
  const auto terminal = chat_session_internal::FlushToolOutput(
      &raw_detector, structured_accumulator);
  output.events.insert(output.events.end(),
                       std::make_move_iterator(terminal.events.begin()),
                       std::make_move_iterator(terminal.events.end()));

  ASSERT_EQ(output.events.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<std::string>(output.events.front()));
  EXPECT_EQ(std::get<std::string>(output.events.front()), input);
}

TEST(ChatSessionDecisionTest, StructuredCallDisablesLaterRawRecognitionAndPreservesProducedOrder) {
  RawEnvelopeDetector raw_detector(
      {"apply_patch", "*** Begin Patch", "*** End Patch"});
  ToolCallStreamAccumulator structured_accumulator("<tool_call>", "</tool_call>");
  const std::string structured =
      R"(<tool_call>{"name":"lookup","arguments":{"key":"alpha"}}</tool_call>)";
  const std::string raw =
      "\n*** Begin Patch\n*** Update File: a.txt\n@@\n-old\n+new\n*** End Patch";

  const auto output = chat_session_internal::PushToolOutput(
      structured + raw, &raw_detector, structured_accumulator);

  ASSERT_FALSE(output.events.empty());
  ASSERT_TRUE(std::holds_alternative<ParsedToolCall>(output.events.front()));
  EXPECT_FALSE(std::get<ParsedToolCall>(output.events.front()).raw_envelope);

  std::string post_call_text;
  for (size_t i = 1; i < output.events.size(); ++i) {
    ASSERT_TRUE(std::holds_alternative<std::string>(output.events[i]));
    post_call_text += std::get<std::string>(output.events[i]);
  }
  EXPECT_EQ(post_call_text, raw);
}

TEST(ChatSessionDecisionTest, RawCallDisablesLaterStructuredRecognitionAndPreservesProducedOrder) {
  RawEnvelopeDetector raw_detector(
      {"apply_patch", "*** Begin Patch", "*** End Patch"});
  ToolCallStreamAccumulator structured_accumulator("<tool_call>", "</tool_call>");
  const std::string raw =
      "*** Begin Patch\n*** Update File: a.txt\n@@\n-old\n+new\n*** End Patch";
  const std::string structured =
      R"(<tool_call>{"name":"lookup","arguments":{"key":"alpha"}}</tool_call>)";

  auto output = chat_session_internal::PushToolOutput(
      raw + "\n" + structured, &raw_detector, structured_accumulator);
  auto terminal = chat_session_internal::FlushToolOutput(
      &raw_detector, structured_accumulator);
  output.events.insert(output.events.end(),
                       std::make_move_iterator(terminal.events.begin()),
                       std::make_move_iterator(terminal.events.end()));

  ASSERT_FALSE(output.events.empty());
  ASSERT_TRUE(std::holds_alternative<ParsedToolCall>(output.events.front()));
  EXPECT_TRUE(std::get<ParsedToolCall>(output.events.front()).raw_envelope);

  std::string post_call_text;
  for (size_t i = 1; i < output.events.size(); ++i) {
    ASSERT_TRUE(std::holds_alternative<std::string>(output.events[i]));
    post_call_text += std::get<std::string>(output.events[i]);
  }
  EXPECT_EQ(post_call_text, "\n" + structured);
}

TEST(ChatSessionDecisionTest, StructuredCandidateOwnsNestedRawEnvelopeAtEverySplit) {
  const std::string input =
      "<tool_call>{\"name\":\"lookup\",\"arguments\":{\n"
      "*** Begin Patch\n"
      "<tool_call>{\"name\":\"nested\",\"arguments\":{}}</tool_call>\n"
      "*** End Patch\n"
      "malformed}}</tool_call>";

  for (size_t split = 0; split <= input.size(); ++split) {
    const auto events =
        RunToolOutput({input.substr(0, split), input.substr(split)});

    EXPECT_TRUE(std::ranges::none_of(events, [](const auto& event) {
      const auto* call = std::get_if<ParsedToolCall>(&event);
      return call != nullptr && call->raw_envelope;
    })) << split;
  }
}

TEST(ChatSessionDecisionTest, RawCandidateOwnsNestedStructuredEnvelopeAtEverySplit) {
  const std::string input =
      "*** Begin Patch\n"
      "<tool_call>{\"name\":\"lookup\",\"arguments\":{}}</tool_call>\n"
      "*** End Patch";

  for (size_t split = 0; split <= input.size(); ++split) {
    const auto events =
        RunToolOutput({input.substr(0, split), input.substr(split)});

    ASSERT_EQ(events.size(), 1u) << split;
    ASSERT_TRUE(std::holds_alternative<ParsedToolCall>(events.front())) << split;
    EXPECT_TRUE(std::get<ParsedToolCall>(events.front()).raw_envelope) << split;
  }
}

TEST(ChatSessionDecisionTest, RawCandidateOwnsNestedQwenXmlEnvelopeAtEverySplit) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"lookup","parameters":{"type":"object","properties":{}}}}])";
  const std::unordered_map<std::string, ToolKind> kinds = {
      {"lookup", ToolKind::kFunction},
  };
  const std::string input =
      "*** Begin Patch\n"
      "<tool_call>\n"
      "<function=lookup>\n"
      "</function>\n"
      "</tool_call>\n"
      "*** End Patch";

  for (size_t split = 0; split <= input.size(); ++split) {
    const auto events =
        RunToolOutput({input.substr(0, split), input.substr(split)}, tools, kinds);

    ASSERT_EQ(events.size(), 1u) << split;
    ASSERT_TRUE(std::holds_alternative<ParsedToolCall>(events.front())) << split;
    EXPECT_TRUE(std::get<ParsedToolCall>(events.front()).raw_envelope) << split;
  }
}

TEST(ChatSessionDecisionTest, QwenXmlCandidateOwnsNestedRawEnvelopeAtEverySplit) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"lookup","parameters":{"type":"object","properties":{)"
      R"("patch":{"type":"string"}},"required":["patch"]}}}])";
  const std::unordered_map<std::string, ToolKind> kinds = {
      {"lookup", ToolKind::kFunction},
  };
  const std::string input =
      "<tool_call>\n"
      "<function=lookup>\n"
      "<parameter=patch>\n"
      "*** Begin Patch\n"
      "*** End Patch\n"
      "</parameter>\n"
      "</function>\n"
      "</tool_call>";

  for (size_t split = 0; split <= input.size(); ++split) {
    const auto events =
        RunToolOutput({input.substr(0, split), input.substr(split)}, tools, kinds);

    ASSERT_EQ(events.size(), 1u) << split;
    ASSERT_TRUE(std::holds_alternative<ParsedToolCall>(events.front())) << split;
    const auto& call = std::get<ParsedToolCall>(events.front());
    EXPECT_FALSE(call.raw_envelope) << split;
    EXPECT_EQ(call.name, "lookup") << split;
    EXPECT_EQ(call.arguments,
              R"({"patch":"*** Begin Patch\n*** End Patch"})")
        << split;
  }
}

TEST(ChatSessionDecisionTest, NonNaturalTerminalCausesRejectMarkerAtEof) {
  const std::string input = "*** Begin Patch\npayload\n*** End Patch";
  for (const auto& cause : {
           std::tuple{true, false, false, std::optional<BackendTerminationCause>{}},
           std::tuple{false, true, false, std::optional<BackendTerminationCause>{}},
           std::tuple{false, false, true, std::optional<BackendTerminationCause>{}},
           std::tuple{false, false, false,
                      std::optional{BackendTerminationCause::kStopSequence}},
           std::tuple{false, false, false,
                      std::optional{BackendTerminationCause::kOutputTokenLimit}},
           std::tuple{false, false, false,
                      std::optional{BackendTerminationCause::kSessionTokenLimit}},
           std::tuple{false, false, false,
                      std::optional{BackendTerminationCause::kCancellation}},
           std::tuple{false, false, false,
                      std::optional{BackendTerminationCause::kFailure}},
           std::tuple{false, false, false, std::optional<BackendTerminationCause>{}},
       }) {
    const auto [canceled, stop, host_limit, backend_termination] = cause;
    RawEnvelopeDetector raw_detector(
        {"apply_patch", "*** Begin Patch", "*** End Patch"});
    ToolCallStreamAccumulator structured_accumulator("<tool_call>", "</tool_call>");
    auto output = chat_session_internal::PushToolOutput(
        input, &raw_detector, structured_accumulator);
    const bool natural = chat_session_internal::IsNaturalToolOutputEnd(
        canceled, stop, host_limit, backend_termination);
    auto terminal = chat_session_internal::FlushToolOutput(
        &raw_detector, structured_accumulator, natural);
    output.events.insert(output.events.end(),
                         std::make_move_iterator(terminal.events.begin()),
                         std::make_move_iterator(terminal.events.end()));

    EXPECT_TRUE(std::ranges::none_of(output.events, [](const auto& event) {
      return std::holds_alternative<ParsedToolCall>(event);
    }));
    ASSERT_EQ(output.events.size(), 1u);
    EXPECT_EQ(std::get<std::string>(output.events.front()), input);
  }
}

TEST(ChatSessionDecisionTest, NaturalEosRecognizesMarkerAtEof) {
  RawEnvelopeDetector raw_detector(
      {"apply_patch", "*** Begin Patch", "*** End Patch"});
  ToolCallStreamAccumulator structured_accumulator("<tool_call>", "</tool_call>");
  const std::string input = "*** Begin Patch\npayload\n*** End Patch";

  auto output = chat_session_internal::PushToolOutput(
      input, &raw_detector, structured_accumulator);
  auto terminal = chat_session_internal::FlushToolOutput(
      &raw_detector, structured_accumulator, /*natural_end=*/true);
  output.events.insert(output.events.end(),
                       std::make_move_iterator(terminal.events.begin()),
                       std::make_move_iterator(terminal.events.end()));

  ASSERT_EQ(output.events.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<ParsedToolCall>(output.events.front()));
  EXPECT_TRUE(std::get<ParsedToolCall>(output.events.front()).raw_envelope);
}

TEST(ChatSessionDecisionTest, NonNaturalTerminalCausesRejectPendingQwenBatchExactly) {
  const std::string tools =
      R"([{"type":"function","function":{"name":"lookup","parameters":{"type":"object",)"
      R"("properties":{"city":{"type":"string"}},"required":["city"]}}}])";
  const std::vector<std::string> pending_batches = {
      std::string(kLookupCall),
      "<tool_call>\n<function=lookup>\n<parameter=city>\nParis",
      std::string(kLookupCall) + " \n\t",
  };
  const std::vector causes = {
      std::tuple{true, false, false,
                 std::optional<BackendTerminationCause>{}},
      std::tuple{false, true, false,
                 std::optional<BackendTerminationCause>{}},
      std::tuple{false, false, true,
                 std::optional<BackendTerminationCause>{}},
      std::tuple{false, false, false,
                 std::optional{BackendTerminationCause::kStopSequence}},
      std::tuple{false, false, false,
                 std::optional{BackendTerminationCause::kOutputTokenLimit}},
      std::tuple{false, false, false,
                 std::optional{BackendTerminationCause::kSessionTokenLimit}},
      std::tuple{false, false, false,
                 std::optional{BackendTerminationCause::kCancellation}},
      std::tuple{false, false, false,
                 std::optional{BackendTerminationCause::kFailure}},
      std::tuple{false, false, false,
                 std::optional<BackendTerminationCause>{}},
  };

  for (const auto& pending : pending_batches) {
    for (const auto& [canceled, stop, host_limit, backend_termination] :
         causes) {
      ToolCallStreamAccumulator accumulator(
          "<tool_call>", "</tool_call>", tools, "",
          CreateQwenXmlToolCallPayloadParser(
              tools, {{"lookup", ToolKind::kFunction}}));
      auto output = accumulator.Push(pending);
      const auto natural = chat_session_internal::IsNaturalToolOutputEnd(
          canceled, stop, host_limit, backend_termination);
      auto terminal = chat_session_internal::FlushToolOutput(
          nullptr, accumulator, natural);
      output.events.insert(output.events.end(),
                           std::make_move_iterator(terminal.events.begin()),
                           std::make_move_iterator(terminal.events.end()));

      ASSERT_EQ(output.events.size(), 1u);
      EXPECT_EQ(std::get<std::string>(output.events.front()), pending);
    }
  }
}

TEST(OnnxChatGeneratorDecisionTest, ExactEosAndLimitsHaveDistinctRawFinalizationCauses) {
  using onnx_chat_generator_internal::ClassifyTurnTermination;

  const auto eos = ClassifyTurnTermination(false, true, 32, 32, 128, 128, true);
  EXPECT_EQ(eos.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
  EXPECT_EQ(eos.cause, BackendTerminationCause::kNaturalEnd);

  const auto output_limit =
      ClassifyTurnTermination(false, false, 32, 32, 96, 128, true);
  EXPECT_EQ(output_limit.finish_reason, FOUNDRY_LOCAL_FINISH_LENGTH);
  EXPECT_EQ(output_limit.cause, BackendTerminationCause::kOutputTokenLimit);

  const auto context_limit =
      ClassifyTurnTermination(false, false, 31, 32, 128, 128, true);
  EXPECT_EQ(context_limit.finish_reason, FOUNDRY_LOCAL_FINISH_LENGTH);
  EXPECT_EQ(context_limit.cause, BackendTerminationCause::kSessionTokenLimit);

  const auto unclassified =
      ClassifyTurnTermination(false, false, 31, 32, 127, 128, true);
  EXPECT_EQ(unclassified.finish_reason, std::nullopt);
  EXPECT_EQ(unclassified.cause, BackendTerminationCause::kFailure);
}

TEST(ChatSessionDecisionTest, ReasoningBoundaryRejectsRawCandidateWithoutStructuredReparse) {
  const std::string nested =
      R"(<tool_call>{"name":"lookup","arguments":{}}</tool_call>)";
  const std::vector<std::pair<std::string, std::string>> boundaries{
      {"*** Begin Patch\n" + nested, "\npayload\n*** End Patch"},
      {"*** Begin Patch\npayload\n", "*** End Patch"},
      {"*** Begin Patch\npayload\n*** End Pa", "tch"},
  };

  for (const auto& [before_reasoning, after_reasoning] : boundaries) {
    RawEnvelopeDetector raw_detector(
        {"apply_patch", "*** Begin Patch", "*** End Patch"});
    ToolCallStreamAccumulator structured_accumulator("<tool_call>", "</tool_call>");
    std::vector<ToolCallStreamAccumulator::Event> events;
    const auto append = [&](ToolCallStreamAccumulator::Output output) {
      events.insert(events.end(),
                    std::make_move_iterator(output.events.begin()),
                    std::make_move_iterator(output.events.end()));
    };

    append(chat_session_internal::PushToolOutput(
        before_reasoning, &raw_detector, structured_accumulator));
    append(chat_session_internal::AbortRawToolOutput(&raw_detector));
    append(chat_session_internal::PushToolOutput(
        after_reasoning, &raw_detector, structured_accumulator));
    append(chat_session_internal::FlushToolOutput(
        &raw_detector, structured_accumulator));

    EXPECT_TRUE(std::ranges::none_of(events, [](const auto& event) {
      return std::holds_alternative<ParsedToolCall>(event);
    })) << before_reasoning;
  }
}

TEST(ChatSessionDecisionTest, InvalidLaterFunctionCallPreventsTheWholeBatchFromStreaming) {
  ToolCallContext context;
  context.tool_kinds = {{"first", ToolKind::kFunction}, {"second", ToolKind::kFunction}};

  ToolCallStreamAccumulator::Output output;
  ParsedToolCall valid{"call_1", "first", R"({"value":1})"};
  valid.argument_source = valid.arguments;
  output.events.emplace_back(std::move(valid));

  ParsedToolCall invalid{"call_2", "second", std::string("before\0after", 12)};
  invalid.argument_source = R"("before\u0000after")";
  output.events.emplace_back(std::move(invalid));

  size_t streamed_calls = 0;
  EXPECT_THROW(
      {
        chat_session_internal::NormalizeToolOutputBatch(output, context);
        for (const auto& event : output.events) {
          if (std::holds_alternative<ParsedToolCall>(event)) {
            ++streamed_calls;
          }
        }
      },
      fl::Exception);
  EXPECT_EQ(streamed_calls, 0u);
}

TEST(ChatSessionDecisionTest, FunctionCallNameMustBeNulFreeUtf8) {
  ToolCallContext context;
  context.tool_kinds = {{"tool", ToolKind::kFunction}};

  ToolCallStreamAccumulator::Output output;
  ParsedToolCall invalid{"call_1", std::string("tool\0hidden", 11), R"({"value":1})"};
  invalid.argument_source = invalid.arguments;
  output.events.emplace_back(std::move(invalid));

  EXPECT_THROW(chat_session_internal::NormalizeToolOutputBatch(output, context), fl::Exception);
}

TEST(ChatSessionDecisionTest, ExactResidentPrefixSelectsOnlyTheUnmatchedFullPromptSuffix) {
  const std::vector<int32_t> resident = {10, 20, 30};
  const std::vector<int32_t> full_prompt = {10, 20, 30, 40, 50};

  EXPECT_EQ(chat_internal::FindUnmatchedPromptSuffix(resident, full_prompt), 3u);
}

TEST(ChatSessionDecisionTest, ResidentPromptMismatchRequiresRebuild) {
  const std::vector<int32_t> changed_token = {10, 21, 30};
  const std::vector<int32_t> longer_resident = {10, 20, 30, 40};
  const std::vector<int32_t> full_prompt = {10, 20, 30};

  EXPECT_EQ(chat_internal::FindUnmatchedPromptSuffix(changed_token, full_prompt), std::nullopt);
  EXPECT_EQ(chat_internal::FindUnmatchedPromptSuffix(longer_resident, full_prompt), std::nullopt);
}

TEST(ChatSessionDecisionTest, EqualResidentAndFullPromptHasAnEmptySuffix) {
  const std::vector<int32_t> tokens = {10, 20, 30};

  EXPECT_EQ(chat_internal::FindUnmatchedPromptSuffix(tokens, tokens), tokens.size());
}

TEST(ChatSessionDecisionTest, HostOutputLimitAppliesToClassicAndMediaGeneratorsButNotEngineText) {
  using chat_session_internal::ShouldEnforceHostOutputLimit;

  EXPECT_TRUE(ShouldEnforceHostOutputLimit(ChatBackendKind::kGenerator, /*media_turn=*/false));
  EXPECT_TRUE(ShouldEnforceHostOutputLimit(ChatBackendKind::kGenerator, /*media_turn=*/true));
  EXPECT_TRUE(ShouldEnforceHostOutputLimit(ChatBackendKind::kEngine, /*media_turn=*/true));
  EXPECT_FALSE(ShouldEnforceHostOutputLimit(ChatBackendKind::kEngine, /*media_turn=*/false));
}

TEST(ChatSessionDecisionTest, FinishReasonPrecedenceCoversEveryTerminalSource) {
  using chat_session_internal::ResolveGeneratedFinishReason;

  struct TestCase {
    const char* name;
    bool has_tool_calls;
    bool stop_sequence_matched;
    bool host_output_limit_reached;
    std::optional<flFinishReason> backend_finish_reason;
    flFinishReason expected;
  };
  const std::vector<TestCase> cases = {
      {"tool calls win over stop", true, true, false, FOUNDRY_LOCAL_FINISH_STOP,
       FOUNDRY_LOCAL_FINISH_TOOL_CALLS},
      {"tool calls win over host limit", true, true, true, FOUNDRY_LOCAL_FINISH_NONE,
       FOUNDRY_LOCAL_FINISH_TOOL_CALLS},
      {"stop wins over host limit", false, true, true, FOUNDRY_LOCAL_FINISH_NONE,
       FOUNDRY_LOCAL_FINISH_STOP},
      {"host limit produces length", false, false, true, FOUNDRY_LOCAL_FINISH_NONE,
       FOUNDRY_LOCAL_FINISH_LENGTH},
      {"backend reason survives natural completion", false, false, false, FOUNDRY_LOCAL_FINISH_STOP,
       FOUNDRY_LOCAL_FINISH_STOP},
  };

  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    EXPECT_EQ(ResolveGeneratedFinishReason(test.has_tool_calls, test.stop_sequence_matched,
                                           test.host_output_limit_reached, test.backend_finish_reason,
                                           /*completion_tokens=*/32, /*max_output_tokens=*/32),
              test.expected);
  }
}

TEST(ChatSessionDecodedStreamTest, CombinedFilterOutputPreservesTokenProvenance) {
  StopStringFilter stop_filter({"STOP"});
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102});
  std::vector<Segment> segments;
  const auto process = [&](const std::vector<Segment>& emitted) { AppendSegments(segments, emitted); };

  EXPECT_FALSE(chat_session_internal::PushDecodedFragment(
      "S", 1, &stop_filter, splitter, process));
  EXPECT_FALSE(chat_session_internal::PushDecodedFragment(
      "<think>hidden</think>visible", 2, &stop_filter, splitter, process));
  chat_session_internal::FlushDecodedStream(&stop_filter, splitter, process);

  ASSERT_EQ(segments.size(), 3u);
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(segments[0].text, "S");
  EXPECT_EQ(segments[1].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  EXPECT_EQ(segments[1].text, "hidden");
  EXPECT_EQ(segments[2].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(segments[2].text, "visible");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ChatSessionDecodedStreamTest, FlushReleasesUnmatchedStopPrefixThroughReasoningSplitter) {
  StopStringFilter stop_filter({"STOP"});
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102});
  std::vector<Segment> segments;
  const auto process = [&](const std::vector<Segment>& emitted) { AppendSegments(segments, emitted); };

  EXPECT_FALSE(chat_session_internal::PushDecodedFragment(
      "", 101, &stop_filter, splitter, process));
  EXPECT_FALSE(chat_session_internal::PushDecodedFragment(
      "unfinished ", 1, &stop_filter, splitter, process));
  EXPECT_FALSE(chat_session_internal::PushDecodedFragment(
      "ST", 2, &stop_filter, splitter, process));
  chat_session_internal::FlushDecodedStream(&stop_filter, splitter, process);

  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  EXPECT_EQ(segments[0].text, "unfinished ST");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 2);
}

TEST(ChatSessionDecodedStreamTest, MatchedStopSuppressesStopBytesButFlushesPendingReasoningText) {
  StopStringFilter stop_filter({"STOP"});
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102});
  std::vector<Segment> segments;
  const auto process = [&](const std::vector<Segment>& emitted) { AppendSegments(segments, emitted); };

  EXPECT_FALSE(chat_session_internal::PushDecodedFragment(
      "", 101, &stop_filter, splitter, process));
  EXPECT_FALSE(chat_session_internal::PushDecodedFragment(
      "hidden</thiST", 1, &stop_filter, splitter, process));
  EXPECT_TRUE(chat_session_internal::PushDecodedFragment(
      "OPignored", 2, &stop_filter, splitter, process));
  chat_session_internal::FlushDecodedStream(&stop_filter, splitter, process);

  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  EXPECT_EQ(segments[0].text, "hidden</thi");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ChatSessionDecisionTest, PreAppendRebuildTracksBackendBakedSettings) {
  using chat_session_internal::ShouldRebuildRetainedGeneratorBeforeAppend;

  // A dynamic Engine or classic generator may continue when nothing that is baked into retained state changed.
  EXPECT_FALSE(ShouldRebuildRetainedGeneratorBeforeAppend(ChatBackendKind::kGenerator,
                                                          /*guidance_requirement_changed=*/false,
                                                          /*guidance_payload_changed=*/false,
                                                          /*retained_generation_settings_changed=*/false));
  EXPECT_FALSE(ShouldRebuildRetainedGeneratorBeforeAppend(ChatBackendKind::kEngine,
                                                          /*guidance_requirement_changed=*/false,
                                                          /*guidance_payload_changed=*/false,
                                                          /*retained_generation_settings_changed=*/false));

  // The caller reports only options baked into the selected backend. These changes rebuild a classic Generator;
  // dynamic Engine settings are per-turn and therefore reach this helper as unchanged.
  EXPECT_TRUE(ShouldRebuildRetainedGeneratorBeforeAppend(ChatBackendKind::kGenerator,
                                                         /*guidance_requirement_changed=*/true,
                                                         /*guidance_payload_changed=*/false,
                                                         /*retained_generation_settings_changed=*/false));
  EXPECT_TRUE(ShouldRebuildRetainedGeneratorBeforeAppend(ChatBackendKind::kGenerator,
                                                         /*guidance_requirement_changed=*/false,
                                                         /*guidance_payload_changed=*/true,
                                                         /*retained_generation_settings_changed=*/false));
  EXPECT_TRUE(ShouldRebuildRetainedGeneratorBeforeAppend(ChatBackendKind::kGenerator,
                                                         /*guidance_requirement_changed=*/false,
                                                         /*guidance_payload_changed=*/false,
                                                         /*retained_generation_settings_changed=*/true));
}

TEST(ChatSessionDecisionTest, RetainedStateInvalidationMatchesSuccessfulTurnSemantics) {
  using chat_session_internal::ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn;

  EXPECT_FALSE(ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(
      ChatBackendKind::kGenerator,
      /*grammar_was_active=*/false, /*reasoning_was_active=*/false, /*stop_sequence_matched=*/false,
      /*host_output_limit_reached=*/false));
  EXPECT_FALSE(ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(
      ChatBackendKind::kEngine,
      /*grammar_was_active=*/false, /*reasoning_was_active=*/false, /*stop_sequence_matched=*/false,
      /*host_output_limit_reached=*/false));
  EXPECT_TRUE(ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(
      ChatBackendKind::kGenerator,
      /*grammar_was_active=*/true, /*reasoning_was_active=*/false, /*stop_sequence_matched=*/false,
      /*host_output_limit_reached=*/false));

  EXPECT_TRUE(ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(
      ChatBackendKind::kEngine,
      /*grammar_was_active=*/false, /*reasoning_was_active=*/false, /*stop_sequence_matched=*/true,
      /*host_output_limit_reached=*/false));
  EXPECT_TRUE(ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(
      ChatBackendKind::kEngine,
      /*grammar_was_active=*/false, /*reasoning_was_active=*/true, /*stop_sequence_matched=*/false,
      /*host_output_limit_reached=*/false));
  EXPECT_TRUE(ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(
      ChatBackendKind::kGenerator,
      /*grammar_was_active=*/false, /*reasoning_was_active=*/false, /*stop_sequence_matched=*/false,
      /*host_output_limit_reached=*/true));
}

TEST(ChatSessionDecisionTest, UndoInvalidatesGeneratorsWithoutAUsableRewindBoundary) {
  using chat_session_internal::ShouldInvalidateRetainedGeneratorForUndo;

  EXPECT_TRUE(ShouldInvalidateRetainedGeneratorForUndo(
      /*undo_all=*/true, /*has_pre_turn_boundary=*/true, /*can_rewind=*/true));
  EXPECT_TRUE(ShouldInvalidateRetainedGeneratorForUndo(
      /*undo_all=*/false, /*has_pre_turn_boundary=*/false, /*can_rewind=*/true));
  EXPECT_TRUE(ShouldInvalidateRetainedGeneratorForUndo(
      /*undo_all=*/false, /*has_pre_turn_boundary=*/true, /*can_rewind=*/false));
  EXPECT_FALSE(ShouldInvalidateRetainedGeneratorForUndo(
      /*undo_all=*/false, /*has_pre_turn_boundary=*/true, /*can_rewind=*/true));
}

// ===========================================================================
// Integration test fixture: loads the shared test model once per suite
// ===========================================================================

class ChatSessionTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    auto model_path = fl::test::GetTestModelPath(fl::test::kTestChatModelAlias);
    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);

    auto result = load_manager_->LoadModel(
        model_path.string(),
        fl::test::kTestChatModelAlias);

    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess)
        << "Failed to load test model from: " << model_path;

    model_ = result.model;
  }

  static void TearDownTestSuite() {
    if (load_manager_) {
      load_manager_->UnloadModel(fl::test::kTestChatModelAlias);
    }

    load_manager_.reset();
    ep_detector_.reset();
    model_ = nullptr;
  }

  GenAIModelInstance& GetModel() { return *model_; }
  const Model& GetCatalogModel() { return catalog_model_; }
  Model MakeReasoningCatalogModel() {
    ModelInfo info;
    info.task = "chat-completion";
    info.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT, 1);
    info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR, "<think>");
    info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR, "</think>");
    return Model::FromModelInfo(
        std::move(info), "", svc_.download_manager, svc_.model_load_manager);
  }

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline fl::test::FakeServiceBindings svc_;
  static inline Model catalog_model_ = [] {
    ModelInfo info;
    info.task = "chat-completion";
    return Model::FromModelInfo(std::move(info), "", svc_.download_manager, svc_.model_load_manager);
  }();
  TelemetryLogger null_telemetry_{"test", fl::test::NullLog()};
  fl::test::NullSessionManager null_session_manager_;
};

class QwenNativeProductionIntegrationTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    model_directory_ = test::MakeUniqueTempPath("qwen_native_production_");
    ASSERT_TRUE(std::filesystem::create_directory(model_directory_));
    const auto source = test::GetTestDataPath("tiny-random-gpt2-fp32-1");
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
      if (entry.is_regular_file()) {
        std::filesystem::copy_file(
            entry.path(), model_directory_ / entry.path().filename());
      }
    }

    const auto config_path = model_directory_ / "genai_config.json";
    auto config = nlohmann::json::parse(ReadText(config_path));
    config["model"]["type"] = "qwen3_5_text";
    config.erase("engine");
    WriteText(config_path, config.dump(2));

    const auto tokenizer_config_path =
        model_directory_ / "tokenizer_config.json";
    auto tokenizer_config = nlohmann::json::parse(ReadText(tokenizer_config_path));
    tokenizer_config["chat_template"] = kNativeQwenChatTemplate;
    WriteText(tokenizer_config_path, tokenizer_config.dump(2));

    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);
    const auto result = load_manager_->LoadModel(
        model_directory_.string(), kModelId, ExecutionProvider::kCPU);
    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess);
    model_ = result.model;
    ASSERT_NE(model_, nullptr);
    ASSERT_EQ(model_->ModelType(), "qwen3_5_text");
    ASSERT_TRUE(model_->HasNativeQwenXmlToolCalls());
    ASSERT_TRUE(model_->HasPositionalToolResults());

    engine_model_directory_ = test::MakeUniqueTempPath("qwen_native_engine_");
    ASSERT_TRUE(std::filesystem::create_directory(engine_model_directory_));
    const auto engine_source = test::GetTestDataPath("tiny-paged-attention");
    for (const auto& entry : std::filesystem::directory_iterator(engine_source)) {
      if (entry.is_regular_file()) {
        std::filesystem::copy_file(
            entry.path(), engine_model_directory_ / entry.path().filename());
      }
    }

    const auto engine_config_path = engine_model_directory_ / "genai_config.json";
    auto engine_config = nlohmann::json::parse(ReadText(engine_config_path));
    engine_config["model"]["type"] = "qwen3_5_text";
    WriteText(engine_config_path, engine_config.dump(2));

    const auto engine_tokenizer_config_path = engine_model_directory_ / "tokenizer_config.json";
    auto engine_tokenizer_config = nlohmann::json::parse(ReadText(engine_tokenizer_config_path));
    engine_tokenizer_config["chat_template"] = kNativeQwenChatTemplate;
    WriteText(engine_tokenizer_config_path, engine_tokenizer_config.dump(2));

    const auto engine_result = load_manager_->LoadModel(
        engine_model_directory_.string(), kEngineModelId, ExecutionProvider::kCPU);
    ASSERT_EQ(engine_result.status, ModelLoadManager::LoadStatus::kSuccess);
    engine_model_ = engine_result.model;
    ASSERT_NE(engine_model_, nullptr);
    ASSERT_EQ(engine_model_->GetGenAIConfig().GetChatBackendKind(), ChatBackendKind::kEngine);
    ASSERT_TRUE(engine_model_->HasNativeQwenXmlToolCalls());
  }

  static void TearDownTestSuite() {
    if (load_manager_ && engine_model_) {
      EXPECT_TRUE(load_manager_->UnloadModel(kEngineModelId));
    }
    if (load_manager_ && model_) {
      EXPECT_TRUE(load_manager_->UnloadModel(kModelId));
    }

    engine_model_ = nullptr;
    model_ = nullptr;
    load_manager_.reset();
    ep_detector_.reset();
    logger_.reset();
    std::error_code error;
    std::filesystem::remove_all(model_directory_, error);
    EXPECT_FALSE(error) << error.message();
    model_directory_.clear();

    std::filesystem::remove_all(engine_model_directory_, error);
    EXPECT_FALSE(error) << error.message();
    engine_model_directory_.clear();
  }

  static std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }

  static void WriteText(const std::filesystem::path& path,
                        const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    if (!output) {
      throw std::runtime_error("Failed to write native Qwen fixture: " +
                               path.string());
    }
  }

  static Model MakeCatalogModel(bool reasoning = false) {
    ModelInfo info;
    info.task = "chat-completion";
    info.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT, 1);
    info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR,
                        "<tool_call>");
    info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR,
                        "</tool_call>");
    if (reasoning) {
      info.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT, 1);
      info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR,
                          "<think>");
      info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR,
                          "</think>");
    }

    return Model::FromModelInfo(std::move(info), "", services_.download_manager,
                                services_.model_load_manager);
  }

  static TextChatGeneratorFactory OutputFactory(
      std::string output,
      std::shared_ptr<GeneratorCounters> counters = {},
      BackendTerminationCause cause = BackendTerminationCause::kNaturalEnd) {
    return [output = std::move(output), counters = std::move(counters), cause](
               const auto&, const auto&, auto&, const auto&, bool) {
      return std::make_unique<FixedOutputGenerator>(
          output, cause, /*prompt_opens_reasoning=*/false, counters);
    };
  }

  static void AddFunctionTools(ChatSession& session) {
    session.AddToolDefinition(
        {"lookup", "Look up a city.",
         R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"]})",
         ToolKind::kFunction});
    session.AddToolDefinition(
        {"clock", "Read a clock.",
         R"({"type":"object","properties":{"zone":{"type":"string"}},"required":["zone"]})",
         ToolKind::kFunction});
  }

  static Request MakeStatefulRequest(std::string text) {
    Request request;
    request.AddOwnedItem(
        std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(text)));
    request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, "auto");
    return request;
  }

  static nlohmann::json RunChatCompletions(ChatSession& session,
                                           std::string tool_choice,
                                           std::vector<nlohmann::json>* chunks = nullptr) {
    if (chunks != nullptr) {
      session.SetStreamingCallback(
          [chunks](flStreamingCallbackData event, void*) {
            auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
            while (auto item = queue->TryPop()) {
              if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
                chunks->push_back(nlohmann::json::parse(
                    static_cast<const TextItem&>(*item).text));
              }
            }

            return 0;
          });
    }

    auto choice = nlohmann::json(tool_choice);
    if (tool_choice == "forced") {
      choice = {{"type", "function"},
                {"function", {{"name", "lookup"}}}};
    }
    const auto body = nlohmann::json{
        {"model", kModelId},
        {"messages", nlohmann::json::array(
                         {{{"role", "user"}, {"content", "route this"}}})},
        {"tools",
         nlohmann::json::array(
             {{{"type", "function"},
               {"function",
                {{"name", "lookup"},
                 {"description", "Look up a city."},
                 {"parameters",
                  {{"type", "object"},
                   {"properties",
                    {{"city", {{"type", "string"}}}}}}}}}},
              {{"type", "function"},
               {"function",
                {{"name", "clock"},
                 {"description", "Read a clock."},
                 {"parameters",
                  {{"type", "object"},
                   {"properties",
                    {{"zone", {{"type", "string"}}}}}}}}}}})},
        {"tool_choice", std::move(choice)}};

    Request request;
    request.AddOwnedItem(std::make_unique<TextItem>(
        body.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    Response response;
    session.ProcessRequest(request, response);
    EXPECT_EQ(response.items.size(), 1u);
    return nlohmann::json::parse(
        static_cast<const TextItem&>(*response.items.front()).text);
  }

  static std::vector<const ToolCallItem*> Calls(const Response& response) {
    std::vector<const ToolCallItem*> calls;
    for (const auto& item : response.items) {
      if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
        calls.push_back(static_cast<const ToolCallItem*>(item.get()));
      }
    }

    return calls;
  }

  static constexpr const char* kModelId = "qwen-native-production-fixture";
  static constexpr const char* kEngineModelId = "qwen-native-engine-fixture";
  static inline std::filesystem::path model_directory_;
  static inline std::filesystem::path engine_model_directory_;
  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline GenAIModelInstance* engine_model_ = nullptr;
  static inline test::FakeServiceBindings services_;
  TelemetryLogger telemetry_{"qwen-native-production-test", test::NullLog()};
};

TEST_F(QwenNativeProductionIntegrationTest,
       AutoOutputMatrixRoutesStatefulAndStatelessProductionPaths) {
  struct Case {
    std::string name;
    std::string output;
    std::string visible;
    std::string reasoning;
    size_t call_count;
  };
  const std::vector<Case> cases{
      {"ordinary_text", "ordinary answer", "ordinary answer", "", 0},
      {"one_call", std::string(kLookupCall), "", "", 1},
      {"two_adjacent_calls", std::string(kLookupCall) + std::string(kSecondCall),
       "", "", 2},
      {"xml_in_reasoning",
       "<think>" + std::string(kLookupCall) + "</think>visible answer",
       "visible answer", std::string(kLookupCall), 0},
      {"malformed_call",
       "<tool_call>\n<function=lookup>\n<parameter=city>\nParis",
       "<tool_call>\n<function=lookup>\n<parameter=city>\nParis", "", 0},
      {"undeclared_call",
       "<tool_call>\n<function=missing>\n</function>\n</tool_call>",
       "<tool_call>\n<function=missing>\n</function>\n</tool_call>", "", 0},
      {"valid_then_undeclared_is_atomic",
       std::string(kLookupCall) +
           "<tool_call>\n<function=missing>\n</function>\n</tool_call>",
       std::string(kLookupCall) +
           "<tool_call>\n<function=missing>\n</function>\n</tool_call>",
       "", 0},
      {"incomplete_call_then_reasoning_then_suffix",
       "<tool_call>\n<function=lookup>\n<parameter=city>\nParis"
       "<think>inspect</think>suffix",
       "<tool_call>\n<function=lookup>\n<parameter=city>\nParissuffix",
       "inspect", 0},
      {"complete_call_then_reasoning",
       std::string(kLookupCall) + "<think>inspect</think>",
       std::string(kLookupCall), "inspect", 0},
      {"first_call_then_reasoning_then_second_call",
       std::string(kLookupCall) + "<think>inspect</think>" +
           std::string(kSecondCall),
       std::string(kLookupCall), "inspect", 1},
  };

  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    auto catalog_model = MakeCatalogModel(/*reasoning=*/true);

    ChatSession stateful(catalog_model, *model_, *logger_, telemetry_, {},
                         OutputFactory(test_case.output));
    AddFunctionTools(stateful);
    auto stateful_request = MakeStatefulRequest("route this");
    Response stateful_response;
    stateful.ProcessRequest(stateful_request, stateful_response);
    EXPECT_EQ(Calls(stateful_response).size(), test_case.call_count);
    EXPECT_EQ(stateful.Transcript().Messages().back().VisibleText(),
              test_case.visible);
    EXPECT_EQ(stateful.Transcript().Messages().back().ReasoningText(),
              test_case.reasoning);
    EXPECT_EQ(stateful_response.finish_reason,
              test_case.call_count == 0 ? FOUNDRY_LOCAL_FINISH_STOP
                                        : FOUNDRY_LOCAL_FINISH_TOOL_CALLS);

    ChatSession stateless(catalog_model, *model_, *logger_, telemetry_, {},
                          OutputFactory(test_case.output));
    const auto completion = RunChatCompletions(stateless, "auto");
    const auto& message = completion.at("choices").at(0).at("message");
    EXPECT_EQ(message.at("content").is_null()
                  ? std::string{}
                  : message.at("content").get<std::string>(),
              test_case.visible);
    EXPECT_EQ(message.contains("reasoning_content") &&
                      !message.at("reasoning_content").is_null()
                  ? message.at("reasoning_content").get<std::string>()
                  : std::string{},
              test_case.reasoning);
    EXPECT_EQ(message.value("tool_calls", nlohmann::json::array()).size(),
              test_case.call_count);
    EXPECT_EQ(completion.at("choices").at(0).at("finish_reason"),
              test_case.call_count == 0 ? "stop" : "tool_calls");
    EXPECT_TRUE(stateless.Transcript().Empty());
  }
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineAutoMalformedCallRetriesAfterDestroyAndPublishesAcceptedUsageOnce) {
  const std::vector<std::string> outputs = {
      "<tool_call>\n<function=lookup>\n<param=city>\nParis\n</param>\n</function>\n</tool_call>",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":"Paris"}}]</tool_call>)",
  };
  auto counters = std::make_shared<GeneratorCounters>();
  std::vector<int> destroyed_at_creation;
  std::vector<int> closed_at_creation;
  std::vector<std::string> prepared_messages;
  std::vector<ToolCallContext> contexts;
  std::vector<SearchOptions> options;
  std::vector<bool> use_full_context;
  size_t creation = 0;
  TextChatGeneratorFactory factory =
      [&](const chat_internal::PreparedChatMessages& messages, const SearchOptions& search_options,
          auto&, const ToolCallContext& context, bool full_context) {
        destroyed_at_creation.push_back(counters->destroyed);
        closed_at_creation.push_back(counters->closed);
        prepared_messages.push_back(BuildChatMessagesJson(messages.Messages()));
        contexts.push_back(context);
        options.push_back(search_options);
        use_full_context.push_back(full_context);
        const auto index = creation++;
        return std::make_unique<FixedOutputGenerator>(
            outputs.at(index), BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/false, counters,
            index == 0 ? 3 : 11, index == 0 ? 2 : 5);
      };

  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {}, std::move(factory));
  const auto params = nlohmann::json{
      {"model", kEngineModelId},
      {"input", "route this"},
      {"instructions", "stable prefix"},
      {"tool_choice", "auto"},
      {"tools",
       nlohmann::json::array(
           {{{"type", "function"},
             {"name", "lookup"},
             {"description", "Look up a city."},
             {"parameters",
              {{"type", "object"},
               {"properties", {{"city", {{"type", "string"}}}}},
               {"required", nlohmann::json::array({"city"})}}}}})}}
                          .get<responses::ResponseCreateParams>();
  auto request = ResponseConverter::ToSessionRequest(params);
  for (auto& definition :
       ResponseConverter::ExtractResponsesToolDefinitions(params, request)) {
    session.AddToolDefinition(std::move(definition));
  }
  std::vector<std::tuple<std::string, std::string, std::string>> streamed_calls;
  std::string streamed_text;
  session.SetStreamingCallback(
      [&streamed_calls, &streamed_text](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
            const auto& call = static_cast<const ToolCallItem&>(*item);
            streamed_calls.emplace_back(call.call_id, call.name, call.arguments);
          } else if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
            streamed_text += static_cast<const TextItem&>(*item).text;
          }
        }

        return 0;
      });

  Response response;
  session.ProcessRequest(request, response);

  ASSERT_EQ(counters->created, 2);
  ASSERT_EQ(destroyed_at_creation, (std::vector<int>{0, 1}));
  ASSERT_EQ(closed_at_creation, (std::vector<int>{0, 1}));
  EXPECT_EQ(counters->closed, 1);
  EXPECT_EQ(counters->appended, 0);
  ASSERT_EQ(prepared_messages.size(), 2u);
  EXPECT_EQ(prepared_messages[0], prepared_messages[1]);
  EXPECT_TRUE(options[0].HasSameRetainedGenerationSettings(options[1], ChatBackendKind::kEngine));
  EXPECT_EQ(use_full_context, (std::vector<bool>{true, true}));
  EXPECT_TRUE(contexts[0].text_output);
  EXPECT_TRUE(contexts[0].tool_output);
  EXPECT_FALSE(contexts[1].text_output);
  EXPECT_TRUE(contexts[1].tool_output);
  EXPECT_EQ(contexts[0].tools_json, contexts[1].tools_json);

  const auto calls = Calls(response);
  ASSERT_EQ(calls.size(), 1u);
  ASSERT_EQ(streamed_calls.size(), 1u);
  EXPECT_EQ(std::get<0>(streamed_calls.front()), calls.front()->call_id);
  EXPECT_EQ(std::get<1>(streamed_calls.front()), "lookup");
  EXPECT_EQ(std::get<2>(streamed_calls.front()), R"({"city":"Paris"})");
  EXPECT_TRUE(streamed_text.empty());
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_TOOL_CALLS);
  EXPECT_EQ(response.usage.prompt_tokens, 11);
  EXPECT_EQ(response.usage.completion_tokens, 5);
  EXPECT_EQ(response.usage.total_tokens, 16);
  EXPECT_EQ(session.TurnCount(), 1u);
  ASSERT_EQ(session.Transcript().Messages().back().ToolCalls().size(), 1u);
  EXPECT_TRUE(session.Transcript().Messages().back().VisibleText().empty());

  auto [response_output, output_text] = ResponseConverter::FromSessionResponse(response, "msg_recovered");
  const nlohmann::json projected = ResponseConverter::BuildResponseObject(
      "resp_recovered", 123, kEngineModelId, params, std::move(response_output), output_text, response.usage);
  ASSERT_EQ(projected.at("output").size(), 1u);
  EXPECT_EQ(projected.at("output").at(0).at("type"), "function_call");
  EXPECT_EQ(projected.at("output").at(0).at("call_id"), calls.front()->call_id);
  EXPECT_EQ(projected.at("output").at(0).at("name"), "lookup");
  EXPECT_EQ(projected.at("output").at(0).at("arguments"), R"({"city":"Paris"})");
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineAutoNaturalTextAndNativeCallRemainSingleAttemptForOmittedAndExplicitChoice) {
  struct Case {
    bool explicit_auto;
    std::string output;
    size_t call_count;
  };
  const std::vector<Case> cases = {
      {false, "ordinary answer", 0},
      {true, "ordinary answer", 0},
      {false, std::string(kLookupCall), 1},
      {true, std::string(kLookupCall), 1},
  };

  for (const auto& test_case : cases) {
    auto counters = std::make_shared<GeneratorCounters>();
    auto catalog_model = MakeCatalogModel();
    ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {},
                        OutputFactory(test_case.output, counters));
    AddFunctionTools(session);
    auto request = MakeStatefulRequest("route this");
    if (!test_case.explicit_auto) {
      request.options.Remove(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE);
    }

    Response response;
    session.ProcessRequest(request, response);

    EXPECT_EQ(counters->created, 1);
    EXPECT_EQ(Calls(response).size(), test_case.call_count);
    EXPECT_EQ(response.finish_reason, test_case.call_count == 0 ? FOUNDRY_LOCAL_FINISH_STOP
                                                                : FOUNDRY_LOCAL_FINISH_TOOL_CALLS);
  }
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineMalformedRecoveryExhaustionLeavesStateUntouchedAndNextRequestSucceeds) {
  const std::vector<std::string> outputs = {
      "<tool_call>\n<function=lookup>\n<parameter=city>\nParis",
      "not a tool call",
      "clean follow-up",
  };
  auto counters = std::make_shared<GeneratorCounters>();
  size_t creation = 0;
  TextChatGeneratorFactory factory =
      [&](const auto&, const auto&, auto&, const auto&, bool) {
        return std::make_unique<FixedOutputGenerator>(
            outputs.at(creation++), BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/false, counters);
      };

  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {}, std::move(factory));
  AddFunctionTools(session);
  std::vector<std::string> streamed;
  session.SetStreamingCallback(
      [&streamed](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
            streamed.push_back(static_cast<const TextItem&>(*item).text);
          }
        }

        return 0;
      });
  auto malformed_request = MakeStatefulRequest("route this");
  Response failed_response;
  std::string failure_message;
  try {
    session.ProcessRequest(malformed_request, failed_response);
    FAIL() << "expected malformed recovery exhaustion";
  } catch (const fl::Exception& error) {
    EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_INTERNAL);
    failure_message = error.what();
    EXPECT_NE(std::string(error.what()).find(
                  "Model emitted a malformed tool call and guided recovery did not produce a valid tool call"),
              std::string::npos);
  }

  EXPECT_EQ(counters->created, 2);
  EXPECT_EQ(counters->destroyed, 2);
  EXPECT_EQ(session.TurnCount(), 0u);
  EXPECT_TRUE(session.Transcript().Empty());
  EXPECT_TRUE(failed_response.items.empty());
  EXPECT_TRUE(streamed.empty());
  EXPECT_EQ(failed_response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_EQ(failed_response.usage.prompt_tokens, 0);
  EXPECT_EQ(failed_response.usage.completion_tokens, 0);
  EXPECT_EQ(failed_response.usage.total_tokens, 0);
  EXPECT_EQ(failed_response.usage.reasoning_tokens, 0);

  responses::ResponseCreateParams params;
  params.model = kEngineModelId;
  params.input = "route this";
  const nlohmann::json failed = ResponseConverter::BuildFailedResponseObject(
      "resp_failed", 123, kEngineModelId, params, "server_error", failure_message);
  EXPECT_EQ(failed.at("status"), "failed");
  EXPECT_EQ(failed.at("error").at("code"), "server_error");
  EXPECT_NE(failed.at("error").at("message").get<std::string>().find("guided recovery"),
            std::string::npos);
  EXPECT_TRUE(failed.at("output").empty());

  auto clean_request = MakeStatefulRequest("try again");
  Response clean_response;
  session.ProcessRequest(clean_request, clean_response);

  EXPECT_EQ(counters->created, 3);
  EXPECT_EQ(session.TurnCount(), 1u);
  EXPECT_EQ(session.Transcript().Messages().back().VisibleText(), "clean follow-up");
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineMalformedRecoveryCancellationDuringSecondAttemptCancelsBeforeUsageAndPublishesNothing) {
  const std::vector<std::string> outputs = {
      "<tool_call>\n<function=lookup>\n<parameter=city>\nParis",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":"Paris"}}]</tool_call>)",
  };
  auto counters = std::make_shared<GeneratorCounters>();
  Request* active_request = nullptr;
  size_t creation = 0;
  TextChatGeneratorFactory factory =
      [&](const auto&, const auto&, auto&, const auto&, bool) {
        const auto index = creation++;
        std::function<void()> hook;
        if (index == 1) {
          hook = [&] {
            active_request->Cancel();
          };
        }

        return std::make_unique<FixedOutputGenerator>(
            outputs.at(index), BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/false, counters,
            /*prompt_tokens=*/4, /*generated_tokens=*/1, std::move(hook));
      };

  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {},
                      std::move(factory));
  AddFunctionTools(session);
  std::vector<std::string> streamed;
  session.SetStreamingCallback(
      [&streamed](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
            streamed.push_back(static_cast<const TextItem&>(*item).text);
          }
        }

        return 0;
      });

  auto request = MakeStatefulRequest("route this");
  active_request = &request;
  Response response;
  ExpectOperationCancelled([&] { session.ProcessRequest(request, response); });

  EXPECT_TRUE(request.IsCompleted());
  EXPECT_FALSE(request.IsCancellationRequested());
  EXPECT_EQ(counters->created, 2);
  EXPECT_EQ(counters->closed, 1);
  EXPECT_EQ(counters->canceled, 1);
  EXPECT_EQ(counters->usage_after_cancel, 1);
  EXPECT_TRUE(streamed.empty());
  EXPECT_TRUE(response.items.empty());
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_EQ(session.TurnCount(), 0u);
  EXPECT_TRUE(session.Transcript().Empty());
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineMalformedFirstAttemptCancellationFromCallbackCancelsBeforeUsageWithoutRecovery) {
  const std::string output =
      "safe prefix<tool_call>\n<function=lookup>\n<parameter=city>\nParis";
  auto counters = std::make_shared<GeneratorCounters>();
  auto catalog_model = MakeCatalogModel();
  ChatSession session(
      catalog_model, *engine_model_, *logger_, telemetry_, {},
      OutputFactory(output, counters));
  AddFunctionTools(session);
  std::string streamed_text;
  session.SetStreamingCallback(
      [&streamed_text](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
            streamed_text += static_cast<const TextItem&>(*item).text;
          }
        }

        return 1;
      });

  auto request = MakeStatefulRequest("route this");
  Response response;
  ExpectOperationCancelled([&] { session.ProcessRequest(request, response); });

  EXPECT_TRUE(request.IsCompleted());
  EXPECT_FALSE(request.IsCancellationRequested());
  EXPECT_EQ(counters->created, 1);
  EXPECT_EQ(counters->canceled, 1);
  EXPECT_EQ(counters->usage_after_cancel, 1);
  EXPECT_EQ(streamed_text, "safe prefix");
  EXPECT_TRUE(response.items.empty());
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_EQ(session.TurnCount(), 0u);
  EXPECT_TRUE(session.Transcript().Empty());
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineChatCompletionsMalformedFirstAttemptCancellationFromCallbackCancelsBeforeUsageWithoutRecovery) {
  const std::string output =
      "safe prefix<tool_call>\n<function=lookup>\n<parameter=city>\nParis";
  auto counters = std::make_shared<GeneratorCounters>();
  auto catalog_model = MakeCatalogModel();
  ChatSession session(
      catalog_model, *engine_model_, *logger_, telemetry_, {},
      OutputFactory(output, counters));
  std::vector<nlohmann::json> streamed_chunks;
  session.SetStreamingCallback(
      [&streamed_chunks](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        bool semantic_output_seen = false;
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
            auto chunk = nlohmann::json::parse(static_cast<const TextItem&>(*item).text);
            const auto& delta = chunk.at("choices").at(0).at("delta");
            semantic_output_seen |= !delta.value("content", "").empty();
            streamed_chunks.push_back(std::move(chunk));
          }
        }

        return semantic_output_seen ? 1 : 0;
      });

  const auto body = nlohmann::json{
      {"model", kModelId},
      {"messages", nlohmann::json::array(
                       {{{"role", "user"}, {"content", "route this"}}})},
      {"tools",
       nlohmann::json::array(
           {{{"type", "function"},
             {"function",
              {{"name", "lookup"},
               {"description", "Look up a city."},
               {"parameters",
                {{"type", "object"},
                 {"properties", {{"city", {{"type", "string"}}}}}}}}}}})},
      {"tool_choice", "auto"},
      {"stream", true}};
  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>(
      body.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  Response response;
  ExpectOperationCancelled([&] { session.ProcessRequest(request, response); });

  EXPECT_TRUE(request.IsCompleted());
  EXPECT_FALSE(request.IsCancellationRequested());
  EXPECT_EQ(counters->created, 1);
  EXPECT_EQ(counters->canceled, 1);
  EXPECT_EQ(counters->usage_after_cancel, 1);
  std::string streamed_content;
  for (const auto& chunk : streamed_chunks) {
    const auto& choice = chunk.at("choices").at(0);
    const auto& delta = choice.at("delta");
    streamed_content += delta.value("content", "");
    EXPECT_FALSE(delta.contains("tool_calls"));
    EXPECT_TRUE(choice.at("finish_reason").is_null());
  }
  EXPECT_EQ(streamed_content, "safe prefix");
  EXPECT_EQ(streamed_content.find("<tool_call>"), std::string::npos);
  EXPECT_EQ(streamed_content.find("<function="), std::string::npos);
  EXPECT_TRUE(response.items.empty());
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_TRUE(session.Transcript().Empty());
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineMalformedRecoveryIsIneligibleForStopNonAutoChoiceAndExplicitGuidance) {
  enum class IneligibleCase {
    kStop,
    kNone,
    kRequired,
    kForced,
    kGuidance,
  };
  const std::string malformed =
      "<tool_call>\n<function=lookup>\n<parameter=city>\nParis";

  for (const auto test_case :
       {IneligibleCase::kStop, IneligibleCase::kNone, IneligibleCase::kRequired,
        IneligibleCase::kForced, IneligibleCase::kGuidance}) {
    auto counters = std::make_shared<GeneratorCounters>();
    auto catalog_model = MakeCatalogModel();
    ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {},
                        OutputFactory(malformed, counters));
    AddFunctionTools(session);
    auto request = MakeStatefulRequest("route this");
    switch (test_case) {
      case IneligibleCase::kStop:
        StoreStopStringsOption({"never matches"}, request.options);
        break;
      case IneligibleCase::kNone:
        request.options[FOUNDRY_LOCAL_PARAM_TOOL_CHOICE] = "none";
        break;
      case IneligibleCase::kRequired:
        request.options[FOUNDRY_LOCAL_PARAM_TOOL_CHOICE] = "required";
        break;
      case IneligibleCase::kForced:
        request.forced_tool_choice =
            ForcedToolChoice{"lookup", ToolKind::kFunction};
        break;
      case IneligibleCase::kGuidance:
        request.options["guidance_type"] = "json_schema";
        request.options["guidance_data"] =
            R"({"type":"object","properties":{}})";
        break;
    }

    Response response;
    if (test_case == IneligibleCase::kNone) {
      session.ProcessRequest(request, response);
      ASSERT_EQ(response.items.size(), 1u);
      ASSERT_EQ(response.items.front()->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
      EXPECT_EQ(static_cast<const MessageItem&>(*response.items.front()).GetSimpleText(),
                malformed);
      EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
      EXPECT_EQ(session.Transcript().Messages().back().VisibleText(),
                malformed);
    } else {
      try {
        session.ProcessRequest(request, response);
      } catch (const fl::Exception&) {
      }
    }

    EXPECT_EQ(counters->created, 1) << static_cast<int>(test_case);
  }
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineMalformedRecoveryRejectsSemanticPrefixAndNonNaturalCompletionWithoutRetry) {
  struct Case {
    std::string output;
    BackendTerminationCause cause;
    std::string expected_streamed_text;
  };
  const std::vector<Case> cases = {
      {"safe prefix<tool_call>\n<function=lookup>\n<param=city>\nParis\n</param>\n"
       "</function>\n</tool_call>",
       BackendTerminationCause::kNaturalEnd,
       "safe prefix"},
      {"<tool_call>\n<function=lookup>\n<param=city>\nParis\n</param>\n"
       "</function>\n</tool_call>tail",
       BackendTerminationCause::kOutputTokenLimit,
       "tail"},
  };

  for (const auto& test_case : cases) {
    auto counters = std::make_shared<GeneratorCounters>();
    auto catalog_model = MakeCatalogModel();
    ChatSession session(
        catalog_model, *engine_model_, *logger_, telemetry_, {},
        OutputFactory(test_case.output, counters, test_case.cause));
    AddFunctionTools(session);
    std::string streamed_text;
    int streamed_tool_calls = 0;
    session.SetStreamingCallback(
        [&streamed_text, &streamed_tool_calls](flStreamingCallbackData event,
                                               void*) {
          auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
          while (auto item = queue->TryPop()) {
            if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
              streamed_text += static_cast<const TextItem&>(*item).text;
            } else if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
              ++streamed_tool_calls;
            }
          }

          return 0;
        });

    auto request = MakeStatefulRequest("route this");
    Response response;
    try {
      session.ProcessRequest(request, response);
      FAIL() << "expected malformed output to fail";
    } catch (const fl::Exception& error) {
      EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_INTERNAL);
    }

    EXPECT_EQ(counters->created, 1);
    EXPECT_EQ(session.TurnCount(), 0u);
    EXPECT_TRUE(response.items.empty());
    EXPECT_EQ(streamed_text, test_case.expected_streamed_text);
    EXPECT_EQ(streamed_tool_calls, 0);
    EXPECT_EQ(streamed_text.find("<tool_call>"), std::string::npos);
    EXPECT_EQ(streamed_text.find("<function="), std::string::npos);
    EXPECT_TRUE(session.Transcript().Empty());
  }
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineMalformedRecoveryRejectsEveryNonCallRetryOutputWithoutThirdAttempt) {
  const std::string malformed =
      "<tool_call>\n<function=lookup>\n<parameter=city>\nParis";
  const std::vector<std::string> rejected_retries = {
      "<tool_call>not json</tool_call>",
      "",
      "<think>reasoning only</think>",
      "ordinary text",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":"Paris"}}])",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":"Paris"}}</tool_call>)",
      R"(<tool_call>{"name":"lookup","parameters":{"city":"Paris"}}</tool_call>)",
      R"(<tool_call>[{"function":"lookup","parameters":{"city":"Paris"}}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup","arguments":{"city":"Paris"}}]</tool_call>)",
      R"(<tool_call>[{"lookup":{"city":"Paris"}}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup"}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup","parameters":"Paris"}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup","parameters":{}}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":"Paris","country":"FR"}}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":7}}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":{"name":"Paris"}}}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup","name":"clock","parameters":{"city":"Paris"}}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":"Paris","city":"Lyon"}}]</tool_call>)",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":"Paris"}},{"name":"clock","parameters":{}}]</tool_call>)",
  };

  for (const auto& retry_output : rejected_retries) {
    std::vector<std::string> outputs = {malformed, retry_output};
    auto counters = std::make_shared<GeneratorCounters>();
    size_t creation = 0;
    TextChatGeneratorFactory factory =
        [&](const auto&, const auto&, auto&, const auto&, bool) {
          return std::make_unique<FixedOutputGenerator>(
              outputs.at(creation++), BackendTerminationCause::kNaturalEnd,
              /*prompt_opens_reasoning=*/false, counters);
        };

    auto catalog_model = MakeCatalogModel();
    ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {}, std::move(factory));
    AddFunctionTools(session);
    auto request = MakeStatefulRequest("route this");
    Response response;

    EXPECT_THROW(session.ProcessRequest(request, response), fl::Exception) << retry_output;
    EXPECT_EQ(counters->created, 2) << retry_output;
    EXPECT_EQ(session.TurnCount(), 0u) << retry_output;
    EXPECT_TRUE(response.items.empty()) << retry_output;
  }
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineMalformedRecoveryAcceptsNormalizedCustomToolCall) {
  const std::vector<std::string> outputs = {
      "<tool_call>\n<function=transform>\n<parameter=input>\ntext",
      R"(<tool_call>[{"name":"transform","parameters":{"input":"transformed text"}}]</tool_call>)",
  };
  auto counters = std::make_shared<GeneratorCounters>();
  size_t creation = 0;
  TextChatGeneratorFactory factory =
      [&](const auto&, const auto&, auto&, const auto&, bool) {
        return std::make_unique<FixedOutputGenerator>(
            outputs.at(creation++), BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/false, counters);
      };

  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {},
                      std::move(factory));
  session.AddToolDefinition(
      tools::MakeCustomTool("transform", "Transform text."));

  auto request = MakeStatefulRequest("apply this patch");
  Response response;
  session.ProcessRequest(request, response);

  EXPECT_EQ(counters->created, 2);
  const auto calls = Calls(response);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.front()->name, "transform");
  EXPECT_EQ(calls.front()->arguments, "transformed text");
  EXPECT_EQ(calls.front()->kind, ToolKind::kCustom);
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_TOOL_CALLS);
}

TEST_F(QwenNativeProductionIntegrationTest,
       ClassicGeneratorPreservesMalformedBytesAndNeverRetries) {
  const std::string malformed =
      "<tool_call>\n<function=lookup>\n<parameter=city>\nParis";
  auto counters = std::make_shared<GeneratorCounters>();
  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                      OutputFactory(malformed, counters));
  AddFunctionTools(session);

  auto request = MakeStatefulRequest("route this");
  Response response;
  session.ProcessRequest(request, response);

  EXPECT_EQ(counters->created, 1);
  EXPECT_TRUE(Calls(response).empty());
  EXPECT_EQ(session.Transcript().Messages().back().VisibleText(), malformed);
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineAutoRawEnvelopePreservesMalformedBytesAndNeverRetries) {
  const std::string malformed =
      "<tool_call>\n<function=lookup>\n<param=city>\nParis\n</param>\n</function>\n</tool_call>";
  auto counters = std::make_shared<GeneratorCounters>();
  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {},
                      OutputFactory(malformed, counters));
  AddFunctionTools(session);
  session.AddToolDefinition(tools::MakeCustomTool(
      "apply_patch", "Apply a patch.", /*description_present=*/true,
      std::string(tools::kStockGhcpApplyPatchLarkGrammar)));

  std::string streamed_text;
  session.SetStreamingCallback(
      [&streamed_text](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
            streamed_text += static_cast<const TextItem&>(*item).text;
          }
        }

        return 0;
      });

  auto request = MakeStatefulRequest("route this");
  Response response;
  session.ProcessRequest(request, response);

  EXPECT_EQ(counters->created, 1);
  EXPECT_EQ(streamed_text, malformed);
  EXPECT_TRUE(Calls(response).empty());
  EXPECT_EQ(session.Transcript().Messages().back().VisibleText(), malformed);
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineChatCompletionsRecoveryKeepsOneEnvelopeAndAcceptedUsage) {
  const std::vector<std::string> outputs = {
      "<tool_call>\n<function=lookup>\n<param=city>\nParis\n</param>\n</function>\n</tool_call>",
      R"(<tool_call>[{"name":"lookup","parameters":{"city":"Paris"}}]</tool_call>)",
  };
  auto counters = std::make_shared<GeneratorCounters>();
  size_t creation = 0;
  TextChatGeneratorFactory factory =
      [&](const auto&, const auto&, auto&, const ToolCallContext& context, bool) {
        const auto index = creation++;
        EXPECT_EQ(context.text_output, index == 0);
        EXPECT_TRUE(context.tool_output);
        return std::make_unique<FixedOutputGenerator>(
            outputs.at(index), BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/false, counters,
            index == 0 ? 2 : 13, index == 0 ? 3 : 7);
      };

  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {}, std::move(factory));
  std::vector<nlohmann::json> chunks;
  const auto completion = RunChatCompletions(session, "auto", &chunks);

  EXPECT_EQ(counters->created, 2);
  ASSERT_EQ(completion.at("choices").size(), 1u);
  const auto& choice = completion.at("choices").at(0);
  const auto& calls = choice.at("message").at("tool_calls");
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.at(0).at("function").at("name"), "lookup");
  EXPECT_EQ(calls.at(0).at("function").at("arguments"), R"({"city":"Paris"})");
  EXPECT_EQ(choice.at("finish_reason"), "tool_calls");
  EXPECT_EQ(completion.at("usage").at("prompt_tokens"), 13);
  EXPECT_EQ(completion.at("usage").at("completion_tokens"), 7);
  EXPECT_EQ(completion.at("usage").at("total_tokens"), 20);

  const auto role_chunks = std::ranges::count_if(chunks, [](const auto& chunk) {
    return chunk.at("choices").at(0).at("delta").value("role", "") == "assistant";
  });
  const auto tool_chunks = std::ranges::count_if(chunks, [](const auto& chunk) {
    return chunk.at("choices").at(0).at("delta").contains("tool_calls");
  });
  EXPECT_EQ(role_chunks, 1);
  EXPECT_EQ(tool_chunks, 1);
  EXPECT_FALSE(std::ranges::any_of(chunks, [](const auto& chunk) {
    const auto& delta = chunk.at("choices").at(0).at("delta");
    return delta.contains("content") && delta.at("content").is_string() &&
           !delta.at("content").template get<std::string>().empty();
  }));
  EXPECT_TRUE(session.Transcript().Empty());
}

TEST_F(QwenNativeProductionIntegrationTest,
       EngineChatCompletionsAutoRawEnvelopePreservesMalformedBytesAndNeverRetries) {
  const std::string malformed =
      "<tool_call>\n<function=lookup>\n<param=city>\nParis\n</param>\n</function>\n</tool_call>";
  auto counters = std::make_shared<GeneratorCounters>();
  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *engine_model_, *logger_, telemetry_, {},
                      OutputFactory(malformed, counters));

  std::string streamed_content;
  session.SetStreamingCallback(
      [&streamed_content](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type != FOUNDRY_LOCAL_ITEM_TEXT) {
            continue;
          }

          const auto chunk = nlohmann::json::parse(static_cast<const TextItem&>(*item).text);
          const auto& delta = chunk.at("choices").at(0).at("delta");
          if (delta.contains("content") && delta.at("content").is_string()) {
            streamed_content += delta.at("content").get<std::string>();
          }
        }

        return 0;
      });

  const auto descriptor =
      nlohmann::json{{"type", "raw_envelope"},
                     {"tool_name", "apply_patch"},
                     {"start_marker", "*** Begin Patch"},
                     {"end_marker", "*** End Patch"}}
          .dump();
  const auto body = nlohmann::json{
      {"model", kEngineModelId},
      {"messages", nlohmann::json::array({{{"role", "user"}, {"content", "route this"}}})},
      {"tools",
       nlohmann::json::array(
           {{{"type", "function"},
             {"function",
              {{"name", "lookup"},
               {"description", "Look up a city."},
               {"parameters",
                {{"type", "object"},
                 {"properties", {{"city", {{"type", "string"}}}}},
                 {"required", nlohmann::json::array({"city"})}}}}}},
            {{"type", "custom"},
             {"custom", {{"name", "apply_patch"}, {"format", {{"type", "text"}}}}}}})},
      {"tool_choice", "auto"},
      {"metadata", {{tools::kRawEnvelopeMetadataKey, descriptor}}}};

  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>(
      body.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  Response response;
  session.ProcessRequest(request, response);

  EXPECT_EQ(counters->created, 1);
  ASSERT_EQ(response.items.size(), 1u);
  const auto completion =
      nlohmann::json::parse(static_cast<const TextItem&>(*response.items.front()).text);
  const auto& choice = completion.at("choices").at(0);
  EXPECT_EQ(streamed_content, malformed);
  EXPECT_EQ(choice.at("message").at("content"), malformed);
  EXPECT_FALSE(choice.at("message").contains("tool_calls"));
  EXPECT_EQ(choice.at("finish_reason"), "stop");
  EXPECT_TRUE(session.Transcript().Empty());
}

TEST_F(QwenNativeProductionIntegrationTest,
       NonNaturalFinalizationPreservesCompleteQwenXmlInStatefulAndStatelessOutput) {
  auto catalog_model = MakeCatalogModel();
  auto factory = [&] {
    return OutputFactory(std::string(kLookupCall), {},
                         BackendTerminationCause::kOutputTokenLimit);
  };

  ChatSession stateful(catalog_model, *model_, *logger_, telemetry_, {}, factory());
  AddFunctionTools(stateful);
  auto stateful_request = MakeStatefulRequest("route this");
  Response stateful_response;
  stateful.ProcessRequest(stateful_request, stateful_response);

  EXPECT_TRUE(Calls(stateful_response).empty());
  EXPECT_EQ(stateful.Transcript().Messages().back().VisibleText(), kLookupCall);
  EXPECT_EQ(stateful_response.finish_reason, FOUNDRY_LOCAL_FINISH_LENGTH);

  ChatSession stateless(catalog_model, *model_, *logger_, telemetry_, {}, factory());
  const auto completion = RunChatCompletions(stateless, "auto");
  const auto& choice = completion.at("choices").at(0);
  const auto& message = choice.at("message");
  EXPECT_EQ(message.at("content").get<std::string>(), kLookupCall);
  EXPECT_FALSE(message.contains("tool_calls"));
  EXPECT_EQ(choice.at("finish_reason"), "length");
}

TEST_F(QwenNativeProductionIntegrationTest,
       UnnamedVersionOneSerializedToolsProduceAZeroArgumentFunctionCall) {
  constexpr const char* legacy_tools =
      R"([{"type":"function","function":{"name":"zero","description":"No arguments.",)"
      R"("parameters":{"type":"object","properties":{}}}}])";
  flToolDefinition legacy_definition{};
  legacy_definition.version = 1;
  legacy_definition.name = "";
  legacy_definition.description = "";
  legacy_definition.json_schema = legacy_tools;

  auto catalog_model = MakeCatalogModel();
  ChatSession session(
      catalog_model, *model_, *logger_, telemetry_, {},
      OutputFactory("<tool_call>\n<function=zero>\n</function>\n</tool_call>"));
  session.AddToolDefinition(ToolDefinitionFromC(legacy_definition));

  auto request = MakeStatefulRequest("call zero");
  Response response;
  session.ProcessRequest(request, response);

  const auto calls = Calls(response);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.front()->name, "zero");
  EXPECT_EQ(calls.front()->arguments, "{}");
  EXPECT_EQ(calls.front()->kind, ToolKind::kFunction);
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_TOOL_CALLS);
  ASSERT_EQ(session.Transcript().Messages().back().ToolCalls().size(), 1u);
  EXPECT_EQ(session.Transcript().Messages().back().ToolCalls().front()->name,
            "zero");
}

TEST_F(QwenNativeProductionIntegrationTest,
       MalformedVersionOneSerializedFunctionSchemaFallsBackToExactText) {
  constexpr std::string_view generated =
      "<tool_call>\n<function=legacy>\n<parameter=value>\ntext\n</parameter>\n"
      "</function>\n</tool_call>";
  flToolDefinition legacy_definition{};
  legacy_definition.version = 1;
  legacy_definition.name = "";
  legacy_definition.description = "";
  legacy_definition.json_schema =
      R"([{"type":"function","function":{"name":"legacy","parameters":{"type":1}}}])";

  bool generator_created = false;
  std::string streamed_text;
  TextChatGeneratorFactory factory =
      [&](const auto&, const auto&, auto&, const ToolCallContext& context, bool) {
        generator_created = true;
        EXPECT_TRUE(context.guidance_disabled);
        return std::make_unique<FixedOutputGenerator>(
            std::string(generated), BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/false);
      };
  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                      std::move(factory));
  session.AddToolDefinition(ToolDefinitionFromC(legacy_definition));
  session.SetStreamingCallback(
      [&](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
            streamed_text += static_cast<const TextItem&>(*item).text;
          }
        }

        return 0;
      });

  auto request = MakeStatefulRequest("call legacy");
  Response response;
  session.ProcessRequest(request, response);

  EXPECT_TRUE(generator_created);
  EXPECT_EQ(streamed_text, generated);
  EXPECT_TRUE(Calls(response).empty());
  EXPECT_EQ(session.Transcript().Messages().back().VisibleText(), generated);
  EXPECT_TRUE(session.Transcript().Messages().back().ToolCalls().empty());
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
}

TEST_F(QwenNativeProductionIntegrationTest,
       MalformedVersionOneSchemaDoesNotSuppressExplicitGuidance) {
  flToolDefinition legacy_definition{};
  legacy_definition.version = 1;
  legacy_definition.name = "";
  legacy_definition.description = "";
  legacy_definition.json_schema =
      R"([{"type":"function","function":{"name":"legacy","parameters":{"type":1}}}])";

  TextChatGeneratorFactory factory =
      [](const auto&, const auto&, auto&, const ToolCallContext& context, bool) {
        EXPECT_FALSE(context.guidance_disabled);
        EXPECT_EQ(context.guidance_type, "json_schema");
        EXPECT_EQ(context.guidance_data, R"({"type":"string"})");
        return std::make_unique<FixedOutputGenerator>(
            R"("guided")", BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/false);
      };
  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                      std::move(factory));
  session.AddToolDefinition(ToolDefinitionFromC(legacy_definition));

  auto request = MakeStatefulRequest("use explicit guidance");
  request.options.Add("guidance_type", "json_schema");
  request.options.Add("guidance_data", R"({"type":"string"})");
  Response response;
  session.ProcessRequest(request, response);

  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
}

TEST_F(QwenNativeProductionIntegrationTest,
       CreateQwenXmlPayloadParserRequiresExactEffectiveMarkers) {
  ToolCallContext context;
  context.tool_output = true;
  context.text_output = true;
  context.tools_json =
      R"([{"type":"function","function":{"name":"lookup","parameters":{"type":"object","properties":{}}}}])";
  context.tool_kinds = {{"lookup", ToolKind::kFunction}};
  context.tool_call_start = std::string(kQwenXmlToolCallStartMarker);
  context.tool_call_end = std::string(kQwenXmlToolCallEndMarker);

  EXPECT_TRUE(static_cast<bool>(
      chat_session_internal::CreateToolCallPayloadParser(context, *model_)));

  context.tool_call_start = "<tool>";
  EXPECT_FALSE(static_cast<bool>(
      chat_session_internal::CreateToolCallPayloadParser(context, *model_)));
  context.tool_call_start = std::string(kQwenXmlToolCallStartMarker);

  context.tool_call_end = "</tool>";
  EXPECT_FALSE(static_cast<bool>(
      chat_session_internal::CreateToolCallPayloadParser(context, *model_)));

  context.tool_call_end = std::string(kQwenXmlToolCallEndMarker);
  context.tools_json =
      R"([{"type":"function","function":{"name":"lookup","parameters":{"type":"object","properties":{)"
      R"("value":{"type":"string","pattern":"^[a-z]+$"}}}}}])";
  EXPECT_FALSE(static_cast<bool>(
      chat_session_internal::CreateToolCallPayloadParser(context, *model_)));
}

TEST_F(QwenNativeProductionIntegrationTest,
       PartialExplicitGuidanceIsRejectedBeforeGeneratorConstruction) {
  struct Case {
    const char* name;
    const char* option;
    const char* value;
  };
  const std::vector<Case> cases = {
      {"type_only", "guidance_type", "json_schema"},
      {"data_only", "guidance_data", R"({"type":"string"})"},
  };

  for (const auto& test_case : cases) {
    for (const bool with_tools : {false, true}) {
      SCOPED_TRACE(std::string(test_case.name) +
                   (with_tools ? " with tools" : " without tools"));
      bool generator_created = false;
      TextChatGeneratorFactory factory =
          [&](const auto&, const auto&, auto&, const ToolCallContext&, bool) {
            generator_created = true;
            return std::make_unique<FixedOutputGenerator>(
                "ordinary text", BackendTerminationCause::kNaturalEnd,
                /*prompt_opens_reasoning=*/false);
          };
      auto catalog_model = MakeCatalogModel();
      ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                          std::move(factory));
      if (with_tools) {
        AddFunctionTools(session);
      }

      auto request = MakeStatefulRequest("use partial guidance");
      request.options.Add(test_case.option, test_case.value);
      Response response;
      try {
        session.ProcessRequest(request, response);
        FAIL() << "expected partial explicit guidance to be rejected";
      } catch (const fl::Exception& ex) {
        EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
        EXPECT_NE(std::string(ex.what()).find("must be provided together"),
                  std::string::npos)
            << ex.what();
      }

      EXPECT_FALSE(generator_created);
    }
  }
}

TEST_F(QwenNativeProductionIntegrationTest,
       JsonObjectResponseFormatReachesGeneratorAsConcreteSchema) {
  bool generator_created = false;
  TextChatGeneratorFactory factory =
      [&](const auto&, const auto&, auto&, const ToolCallContext& context, bool) {
        generator_created = true;
        EXPECT_EQ(context.guidance_type, "json_schema");
        EXPECT_EQ(nlohmann::json::parse(context.guidance_data),
                  nlohmann::json({{"type", "object"}}));
        return std::make_unique<FixedOutputGenerator>(
            "{}", BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/false);
      };
  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                      std::move(factory));
  const auto body = nlohmann::json{
      {"model", kModelId},
      {"messages", nlohmann::json::array(
                       {{{"role", "user"}, {"content", "return an object"}}})},
      {"response_format", {{"type", "json_object"}}},
  };
  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>(
      body.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  Response response;
  EXPECT_NO_THROW(session.ProcessRequest(request, response));
  EXPECT_TRUE(generator_created);
}

TEST_F(QwenNativeProductionIntegrationTest,
       RequiredOrForcedMalformedVersionOneSchemaIsRejectedBeforeGeneration) {
  const std::vector<std::string> malformed_schemas = {
      R"([{"type":"function","function":{"name":"legacy","parameters":{"type":1}}}])",
      R"([{"type":"function","function":{"name":"legacy"}},{"type":"function","function":{"name":"legacy"}}])",
  };

  for (const auto& schema : malformed_schemas) {
    for (const bool forced : {false, true}) {
      SCOPED_TRACE((forced ? "forced: " : "required: ") + schema);
      flToolDefinition legacy_definition{};
      legacy_definition.version = 1;
      legacy_definition.name = "";
      legacy_definition.description = "";
      legacy_definition.json_schema = schema.c_str();

      bool generator_created = false;
      TextChatGeneratorFactory factory =
          [&](const auto&, const auto&, auto&, const ToolCallContext&, bool) {
            generator_created = true;
            return std::make_unique<FixedOutputGenerator>(
                "ordinary text", BackendTerminationCause::kNaturalEnd,
                /*prompt_opens_reasoning=*/false);
          };
      auto catalog_model = MakeCatalogModel();
      ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                          std::move(factory));
      session.AddToolDefinition(ToolDefinitionFromC(legacy_definition));

      auto request = MakeStatefulRequest("call legacy");
      if (forced) {
        request.forced_tool_choice =
            ForcedToolChoice{"legacy", ToolKind::kFunction};
      } else {
        request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, "required");
      }

      Response response;
      try {
        session.ProcessRequest(request, response);
        FAIL() << "expected malformed tool-only output to be rejected";
      } catch (const fl::Exception& ex) {
        EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
        EXPECT_NE(std::string(ex.what()).find("valid tool definitions"),
                  std::string::npos)
            << ex.what();
      }

      EXPECT_FALSE(generator_created);
    }
  }
}

TEST_F(QwenNativeProductionIntegrationTest,
       ParameterlessFunctionRoutesThroughProductionChatAndResponsesShapes) {
  const std::vector<std::optional<nlohmann::json>> parameter_shapes = {
      std::nullopt,
      nlohmann::json(nullptr),
      nlohmann::json{{"type", "object"},
                     {"properties", nlohmann::json::object()}},
  };
  constexpr std::string_view zero_call =
      "<tool_call>\n<function=zero>\n</function>\n</tool_call>";

  for (const auto& parameters : parameter_shapes) {
    SCOPED_TRACE(parameters.has_value() ? parameters->dump() : "omitted");
    auto function = nlohmann::json{{"name", "zero"},
                                   {"description", "No arguments."}};
    if (parameters.has_value()) {
      function["parameters"] = *parameters;
    }

    const auto body = nlohmann::json{
        {"model", kModelId},
        {"messages", nlohmann::json::array(
                         {{{"role", "user"}, {"content", "call zero"}}})},
        {"tools", nlohmann::json::array(
                      {{{"type", "function"}, {"function", function}}})},
        {"tool_choice", "auto"}};
    auto catalog_model = MakeCatalogModel();
    ChatSession chat(catalog_model, *model_, *logger_, telemetry_, {},
                     OutputFactory(std::string(zero_call)));
    Request request;
    request.AddOwnedItem(std::make_unique<TextItem>(
        body.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    Response response;
    chat.ProcessRequest(request, response);

    ASSERT_EQ(response.items.size(), 1u);
    const auto completion = nlohmann::json::parse(
        static_cast<const TextItem&>(*response.items.front()).text);
    const auto& call = completion.at("choices")
                           .at(0)
                           .at("message")
                           .at("tool_calls")
                           .at(0);
    EXPECT_EQ(call.at("function").at("name"), "zero");
    EXPECT_EQ(call.at("function").at("arguments"), "{}");
    EXPECT_EQ(completion.at("choices").at(0).at("finish_reason"),
              "tool_calls");
  }

  auto catalog_model = MakeCatalogModel();
  ChatSession responses_session(catalog_model, *model_, *logger_, telemetry_, {},
                                OutputFactory(std::string(zero_call)));
  responses_session.AddToolDefinition(
      {"zero", "No arguments.", "{}", ToolKind::kFunction});
  auto request = MakeStatefulRequest("call zero");
  Response response;
  responses_session.ProcessRequest(request, response);
  const auto calls = Calls(response);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.front()->name, "zero");
  EXPECT_EQ(calls.front()->arguments, "{}");

  auto [output, output_text] =
      ResponseConverter::FromSessionResponse(response, "msg_zero");
  responses::ResponseCreateParams params;
  params.model = kModelId;
  params.input = "call zero";
  const nlohmann::json completed = ResponseConverter::BuildResponseObject(
      "resp_zero", 123, kModelId, params, std::move(output), output_text,
      response.usage);
  ASSERT_EQ(completed.at("output").size(), 1u);
  EXPECT_EQ(completed.at("output").at(0).at("type"), "function_call");
  EXPECT_EQ(completed.at("output").at(0).at("name"), "zero");
  EXPECT_EQ(completed.at("output").at(0).at("arguments"), "{}");
}

TEST_F(QwenNativeProductionIntegrationTest,
       NonNaturalDefaultJsonKeepsBaseRecoveryBytesAndToolCallsFinish) {
  constexpr std::string_view output =
      R"(<tool_call>{"name":"lookup","arguments":{"city":"Paris"}})";
  auto catalog_model = MakeCatalogModel();
  ChatSession stateful(
      catalog_model, *model_, *logger_, telemetry_, {},
      OutputFactory(std::string(output), {},
                    BackendTerminationCause::kOutputTokenLimit));
  AddFunctionTools(stateful);
  auto request = MakeStatefulRequest("route this");
  request.forced_tool_choice = ForcedToolChoice{"lookup", ToolKind::kFunction};
  Response response;
  stateful.ProcessRequest(request, response);

  const auto calls = Calls(response);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.front()->name, "lookup");
  EXPECT_EQ(calls.front()->arguments, R"({"city":"Paris"})");
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_TOOL_CALLS);

  ChatSession stateless(
      catalog_model, *model_, *logger_, telemetry_, {},
      OutputFactory(std::string(output), {},
                    BackendTerminationCause::kOutputTokenLimit));
  const auto completion = RunChatCompletions(stateless, "forced");
  const auto& choice = completion.at("choices").at(0);
  const auto& call =
      choice.at("message").at("tool_calls").at(0).at("function");
  EXPECT_EQ(call.at("name"), "lookup");
  EXPECT_EQ(call.at("arguments"), R"({"city":"Paris"})");
  EXPECT_EQ(choice.at("finish_reason"), "tool_calls");
}

TEST_F(QwenNativeProductionIntegrationTest,
       NonNaturalRawEnvelopeKeepsBaseVisibleBytesAndLengthFinish) {
  const std::string patch =
      "*** Begin Patch\n*** Update File: note.txt\n@@\n-old\n+new\n*** End Patch";
  auto catalog_model = MakeCatalogModel();
  ChatSession session(
      catalog_model, *model_, *logger_, telemetry_, {},
      OutputFactory(patch, {}, BackendTerminationCause::kOutputTokenLimit));
  session.AddToolDefinition(tools::MakeCustomTool(
      "apply_patch", "Apply a patch.", /*description_present=*/true,
      std::string(tools::kStockGhcpApplyPatchLarkGrammar)));

  auto request = MakeStatefulRequest("patch the file");
  Response response;
  session.ProcessRequest(request, response);

  EXPECT_TRUE(Calls(response).empty());
  EXPECT_EQ(session.Transcript().Messages().back().VisibleText(), patch);
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_LENGTH);
}

TEST_F(QwenNativeProductionIntegrationTest,
       StatefulStreamFinalTranscriptAndResponsesProjectionKeepCallIdentity) {
  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                      OutputFactory(std::string(kLookupCall)));
  AddFunctionTools(session);

  std::vector<std::tuple<std::string, std::string, std::string>> streamed_calls;
  session.SetStreamingCallback(
      [&streamed_calls](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
            const auto& call = static_cast<const ToolCallItem&>(*item);
            streamed_calls.emplace_back(call.call_id, call.name, call.arguments);
          }
        }

        return 0;
      });

  auto request = MakeStatefulRequest("look up Paris");
  Response response;
  session.ProcessRequest(request, response);

  const auto calls = Calls(response);
  ASSERT_EQ(calls.size(), 1u);
  ASSERT_EQ(streamed_calls.size(), 1u);
  EXPECT_EQ(std::get<0>(streamed_calls.front()), calls.front()->call_id);
  EXPECT_EQ(std::get<1>(streamed_calls.front()), "lookup");
  EXPECT_EQ(std::get<2>(streamed_calls.front()), R"({"city":"Paris"})");
  EXPECT_EQ(calls.front()->arguments, R"({"city":"Paris"})");
  EXPECT_EQ(calls.front()->generated_encoding,
            GeneratedCallEncoding::kStructured);

  ASSERT_EQ(session.Transcript().Messages().back().ToolCalls().size(), 1u);
  const auto* transcript_call =
      session.Transcript().Messages().back().ToolCalls().front();
  EXPECT_EQ(transcript_call->call_id, calls.front()->call_id);
  EXPECT_EQ(transcript_call->arguments, calls.front()->arguments);

  auto [output, output_text] =
      ResponseConverter::FromSessionResponse(response, "msg_native");
  ASSERT_EQ(output.size(), 1u);
  responses::ResponseCreateParams params;
  params.model = kModelId;
  params.input = "look up Paris";
  const auto completed = ResponseConverter::BuildResponseObject(
      "resp_native", 123, kModelId, params, std::move(output), output_text,
      response.usage);
  const nlohmann::json completed_json = completed;
  ASSERT_EQ(completed_json.at("output").size(), 1u);
  EXPECT_EQ(completed_json.at("output")[0].at("call_id"),
            calls.front()->call_id);
  EXPECT_EQ(completed_json.at("output")[0].at("name"), "lookup");
  EXPECT_EQ(completed_json.at("output")[0].at("arguments"),
            R"({"city":"Paris"})");

  int sequence_number = 2;
  auto stream_output = ResponseConverter::BuildToolCallStreamOutput(
      *calls.front(), 0, sequence_number);
  ASSERT_EQ(stream_output.events.size(), 4u);
  EXPECT_EQ(stream_output.events[1].tool_call_id, calls.front()->call_id);
  EXPECT_EQ(stream_output.events[1].delta, calls.front()->arguments);
  EXPECT_EQ(stream_output.events[2].tool_call_id, calls.front()->call_id);
  EXPECT_EQ(stream_output.events[2].tool_payload, calls.front()->arguments);
}

TEST_F(QwenNativeProductionIntegrationTest,
       RequiredAndForcedChatCompletionsKeepJsonParsingAndStableStreamData) {
  constexpr std::string_view output =
      R"(<tool_call>{"name":"lookup","arguments":{"city":"Paris"}}</tool_call>)";

  for (const auto& choice : {"required", "forced"}) {
    SCOPED_TRACE(choice);
    auto catalog_model = MakeCatalogModel();
    ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                        OutputFactory(std::string(output)));
    std::vector<nlohmann::json> chunks;
    const auto completion = RunChatCompletions(session, choice, &chunks);

    const auto& final_call = completion.at("choices")
                                 .at(0)
                                 .at("message")
                                 .at("tool_calls")
                                 .at(0);
    EXPECT_EQ(final_call.at("function").at("name"), "lookup");
    EXPECT_EQ(final_call.at("function").at("arguments"),
              R"({"city":"Paris"})");
    EXPECT_EQ(completion.at("choices").at(0).at("finish_reason"),
              "tool_calls");

    std::vector<nlohmann::json> streamed_tool_calls;
    for (const auto& chunk : chunks) {
      EXPECT_EQ(chunk.at("id"), completion.at("id"));
      const auto& delta = chunk.at("choices").at(0).at("delta");
      if (delta.contains("tool_calls")) {
        streamed_tool_calls.push_back(delta.at("tool_calls").at(0));
      }
    }

    ASSERT_EQ(streamed_tool_calls.size(), 1u);
    EXPECT_EQ(streamed_tool_calls.front().at("id"), final_call.at("id"));
    EXPECT_EQ(streamed_tool_calls.front().at("function").at("name"),
              final_call.at("function").at("name"));
    EXPECT_EQ(streamed_tool_calls.front().at("function").at("arguments"),
              final_call.at("function").at("arguments"));
    EXPECT_TRUE(session.Transcript().Empty());
  }
}

TEST_F(QwenNativeProductionIntegrationTest,
       OpenedApplyPatchEnvelopeWinsOverNestedNativeXml) {
  const std::string patch =
      "*** Begin Patch\n"
      "*** Update File: note.txt\n"
      "@@\n"
      "-old\n"
      "+" +
      std::string(kLookupCall) +
      "\n*** End Patch";
  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                      OutputFactory(patch));
  AddFunctionTools(session);
  session.AddToolDefinition(tools::MakeCustomTool(
      "apply_patch", "Apply a patch.", /*description_present=*/true,
      std::string(tools::kStockGhcpApplyPatchLarkGrammar)));

  auto request = MakeStatefulRequest("patch the file");
  Response response;
  session.ProcessRequest(request, response);

  const auto calls = Calls(response);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.front()->name, "apply_patch");
  EXPECT_EQ(calls.front()->arguments, patch);
  EXPECT_EQ(calls.front()->kind, ToolKind::kCustom);
  EXPECT_EQ(calls.front()->generated_encoding,
            GeneratedCallEncoding::kRawEnvelope);
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_TOOL_CALLS);
}

TEST_F(QwenNativeProductionIntegrationTest,
       StoredNativeCallsReplayThroughAColdResponsesSession) {
  const auto two_calls = std::string(kLookupCall) + std::string(kSecondCall);
  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                      OutputFactory(two_calls));
  AddFunctionTools(session);

  auto request = MakeStatefulRequest("get both values");
  Response response;
  session.ProcessRequest(request, response);
  const auto calls = Calls(response);
  ASSERT_EQ(calls.size(), 2u);

  auto [output, output_text] =
      ResponseConverter::FromSessionResponse(response, "msg_replay");
  responses::ResponseCreateParams params;
  params.model = kModelId;
  params.input = "get both values";
  const auto completed = ResponseConverter::BuildResponseObject(
      "resp_replay", 456, kModelId, params, std::move(output), output_text,
      response.usage);
  const nlohmann::json completed_json = completed;

  ResponseStore store;
  store.Store(
      "resp_replay", completed_json,
      nlohmann::json::array(
          {{{"role", "user"}, {"content", "get both values"}}}),
      kModelId);
  const auto context = store.BuildChainContext("resp_replay");
  ASSERT_TRUE(context.has_value());

  auto replay_params = nlohmann::json{
      {"model", kModelId},
      {"input",
       nlohmann::json::array(
           {{{"type", "function_call_output"},
             {"call_id", calls[0]->call_id},
             {"output", "first result"}},
            {{"type", "function_call_output"},
             {"call_id", calls[1]->call_id},
             {"output", "second result"}}})}}
                           .get<responses::ResponseCreateParams>();
  auto replay_request =
      ResponseConverter::ToSessionRequest(replay_params, &*context);
  ChatSession replay_session(catalog_model, *model_, *logger_, telemetry_, {},
                             OutputFactory("replay complete"));
  Response replay_response;
  EXPECT_NO_THROW(
      replay_session.ProcessRequest(replay_request, replay_response));
  EXPECT_EQ(replay_session.Transcript().Messages().back().VisibleText(),
            "replay complete");
}

TEST_F(QwenNativeProductionIntegrationTest,
       ReverseArrivalResultsReachProductionGeneratorInOriginalCallOrder) {
  const auto two_calls = std::string(kLookupCall) + std::string(kSecondCall);
  std::vector<std::string> outputs{two_calls, "results complete"};
  std::vector<std::string> prepared_messages;
  std::vector<std::string> rendered_prompts;
  size_t creation = 0;
  TextChatGeneratorFactory factory =
      [&](const chat_internal::PreparedChatMessages& messages, const auto&,
          auto& model, const auto& tool_context, bool) {
        prepared_messages.push_back(BuildChatMessagesJson(messages.Messages()));
        rendered_prompts.push_back(
            BuildChatPrompt(messages, model, tool_context.tools_json));
        const auto output = outputs.at(creation++);
        return std::make_unique<FixedOutputGenerator>(
            output, BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/false);
      };

  auto catalog_model = MakeCatalogModel();
  ChatSession session(catalog_model, *model_, *logger_, telemetry_, {},
                      std::move(factory));
  AddFunctionTools(session);

  auto request = MakeStatefulRequest("get both values");
  Response response;
  session.ProcessRequest(request, response);
  const auto calls = Calls(response);
  ASSERT_EQ(calls.size(), 2u);
  ASSERT_NE(calls[0]->call_id, calls[1]->call_id);

  Request reverse_results;
  reverse_results.AddOwnedItem(std::make_unique<ToolResultItem>(
      calls[1]->call_id, "SECOND_RESULT_SENTINEL"));
  reverse_results.AddOwnedItem(std::make_unique<ToolResultItem>(
      calls[0]->call_id, "FIRST_RESULT_SENTINEL"));
  Response result_response;
  session.ProcessRequest(reverse_results, result_response);

  ASSERT_EQ(prepared_messages.size(), 2u);
  const auto prepared = nlohmann::json::parse(prepared_messages.back());
  ASSERT_GE(prepared.size(), 4u);
  EXPECT_EQ(prepared[2].at("tool_call_id"), calls[0]->call_id);
  EXPECT_EQ(prepared[2].at("content"), "FIRST_RESULT_SENTINEL");
  EXPECT_EQ(prepared[3].at("tool_call_id"), calls[1]->call_id);
  EXPECT_EQ(prepared[3].at("content"), "SECOND_RESULT_SENTINEL");

  const auto& prompt = rendered_prompts.back();
  const auto first_result = prompt.find("FIRST_RESULT_SENTINEL");
  const auto second_result = prompt.find("SECOND_RESULT_SENTINEL");
  ASSERT_NE(first_result, std::string::npos);
  ASSERT_NE(second_result, std::string::npos);
  EXPECT_LT(first_result, second_result);

  const auto& canonical = session.Transcript().Messages();
  ASSERT_GE(canonical.size(), 5u);
  EXPECT_EQ(canonical[2].tool_call_id, calls[1]->call_id);
  EXPECT_EQ(canonical[2].VisibleText(), "SECOND_RESULT_SENTINEL");
  EXPECT_EQ(canonical[3].tool_call_id, calls[0]->call_id);
  EXPECT_EQ(canonical[3].VisibleText(), "FIRST_RESULT_SENTINEL");
  EXPECT_EQ(canonical.back().VisibleText(), "results complete");
}

// ===========================================================================
// Construction
// ===========================================================================

TEST_F(ChatSessionTest, ConstructWithModelOnly) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  EXPECT_EQ(session.MessageCount(), 0u);
  EXPECT_TRUE(session.Transcript().Empty());
  EXPECT_EQ(session.TurnCount(), 0u);
}

TEST_F(ChatSessionTest, AdditionalPropertyRequirementsRemainAcceptedAcrossSharedToolChoicePaths) {
  ASSERT_FALSE(GetModel().HasNativeQwenXmlToolCalls());
  const std::string parameters =
      R"({"type":"object","properties":{)"
      R"("name":{"type":"string","pattern":"^[a-z]+$"},)"
      R"("timestamp":{"type":"string","format":"date-time"},)"
      R"("anything":true},)"
      R"("required":["dynamic"]})";
  const std::vector<std::string> modes = {"auto", "required", "forced"};

  for (const auto& mode : modes) {
    SCOPED_TRACE(mode);
    bool generator_created = false;
    TextChatGeneratorFactory factory =
        [&](const auto&, const auto&, auto&, const ToolCallContext& context, bool) {
          generator_created = true;
          EXPECT_FALSE(context.guidance_disabled);
          EXPECT_FALSE(context.tools_json.empty());
          return std::make_unique<FixedOutputGenerator>(
              "[]", BackendTerminationCause::kNaturalEnd,
              /*prompt_opens_reasoning=*/false);
        };
    ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_,
                        {}, std::move(factory));
    session.AddToolDefinition(
        {"lookup", "Look up a value.", parameters, ToolKind::kFunction});

    Request request;
    request.AddOwnedItem(
        std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "route this"));
    request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE,
                        mode == "forced" ? "auto" : mode);
    if (mode == "forced") {
      request.forced_tool_choice =
          ForcedToolChoice{"lookup", ToolKind::kFunction};
    }

    Response response;
    EXPECT_NO_THROW(session.ProcessRequest(request, response));
    EXPECT_TRUE(generator_created);
  }
}

TEST_F(ChatSessionTest, ReferenceBearingSchemasFailClosedAcrossSharedToolChoicePaths) {
  ASSERT_FALSE(GetModel().HasNativeQwenXmlToolCalls());
  const std::vector<std::string> schemas = {
      R"({"type":"object","properties":{"value":{"$ref":"#/$defs/value"}},"$defs":{"value":{"type":"string"}}})",
      R"({"type":"object","properties":{"values":{"type":"array","items":{"$ref":"#/$defs/value"}}},)"
      R"("$defs":{"value":{"type":"string"}}})",
  };

  for (const auto& schema : schemas) {
    for (const auto& mode : {"auto", "required", "forced"}) {
      SCOPED_TRACE(std::string(mode) + ": " + schema);
      bool generator_created = false;
      TextChatGeneratorFactory factory =
          [&](const auto&, const auto&, auto&, const ToolCallContext& context, bool) {
            generator_created = true;
            EXPECT_TRUE(context.guidance_disabled);
            return std::make_unique<FixedOutputGenerator>(
                "ordinary text", BackendTerminationCause::kNaturalEnd,
                /*prompt_opens_reasoning=*/false);
          };
      ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_,
                          {}, std::move(factory));
      session.AddToolDefinition(
          {"lookup", "Look up a value.", schema, ToolKind::kFunction});

      Request request;
      request.AddOwnedItem(
          std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "route this"));
      request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE,
                          mode == std::string_view("forced") ? "auto" : mode);
      if (mode == std::string_view("forced")) {
        request.forced_tool_choice =
            ForcedToolChoice{"lookup", ToolKind::kFunction};
      }

      Response response;
      if (mode == std::string_view("auto")) {
        EXPECT_NO_THROW(session.ProcessRequest(request, response));
        EXPECT_TRUE(generator_created);
      } else {
        try {
          session.ProcessRequest(request, response);
          FAIL() << "expected reference-bearing tool-only output to be rejected";
        } catch (const fl::Exception& ex) {
          EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
          EXPECT_NE(std::string(ex.what()).find("valid tool definitions"),
                    std::string::npos)
              << ex.what();
        }

        EXPECT_FALSE(generator_created);
      }
    }
  }
}

TEST_F(ChatSessionTest, AmbiguousPositionalResultsLeaveWarmGeneratorAndTranscriptUnchanged) {
  auto counters = std::make_shared<GeneratorCounters>();
  TextChatGeneratorFactory factory =
      [counters](const chat_internal::PreparedChatMessages&, const auto&, auto&, const auto&, bool) {
        return std::make_unique<FixedOutputGenerator>(
            "", BackendTerminationCause::kNaturalEnd, /*prompt_opens_reasoning=*/false, counters);
      };
  ChatMessagePreparer positional_preparer =
      [](std::vector<TranscriptMessage> messages, bool) {
        return chat_internal::PrepareChatMessages(std::move(messages),
                                                  /*positional_tool_results=*/true);
      };
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_, {},
                      std::move(factory), std::move(positional_preparer));

  Request seed;
  seed.AddOwnedItem(std::make_unique<ToolCallItem>("call_first", "first", "{}"));
  seed.AddOwnedItem(std::make_unique<ToolCallItem>("call_second", "second", "{}"));
  Response seed_response;
  session.ProcessRequest(seed, seed_response);

  ASSERT_EQ(session.MessageCount(), 2u);
  ASSERT_EQ(session.Transcript().Messages().front().ToolCalls().size(), 2u);
  ASSERT_EQ(counters->created, 1);
  ASSERT_EQ(counters->destroyed, 0);
  ASSERT_EQ(counters->appended, 0);
  const auto transcript_before = BuildChatMessagesJson(session.Transcript().Messages());

  Request partial_results;
  partial_results.AddOwnedItem(std::make_unique<ToolResultItem>("call_first", "first result"));
  Response rejected_response;
  try {
    session.ProcessRequest(partial_results, rejected_response);
    FAIL() << "expected partial positional tool results to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("positional tool results"), std::string::npos) << ex.what();
  }

  EXPECT_EQ(counters->created, 1);
  EXPECT_EQ(counters->destroyed, 0);
  EXPECT_EQ(counters->appended, 0);
  EXPECT_EQ(BuildChatMessagesJson(session.Transcript().Messages()), transcript_before);
  EXPECT_EQ(session.MessageCount(), 2u);
  EXPECT_EQ(session.TurnCount(), 1u);
}

// ===========================================================================
// Session::Run helpers
// ===========================================================================

namespace {
std::unique_ptr<Item> MakeMessage(flMessageRole role, const std::string& content) {
  return std::make_unique<MessageItem>(role, content);
}

// Returns the content of the first MESSAGE item with role "assistant",
// or empty string if none found.
std::string GetAssistantText(const Response& response) {
  for (const auto& item : response.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      const MessageItem& msg = static_cast<const MessageItem&>(*item);
      if (msg.role == FOUNDRY_LOCAL_ROLE_ASSISTANT) {
        return msg.GetSimpleText();
      }
    }
  }

  return {};
}
}  // namespace

// ===========================================================================
// Session::Run (integration — requires loaded model)
// ===========================================================================

TEST_F(ChatSessionTest, RunBasic) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "What is 2+2? Answer with just the number."));
  request.options.Add("max_output_tokens", "32");
  request.options.Add("temperature", "0");

  Response response;
  session.ProcessRequest(request, response);
  auto text = GetAssistantText(response);

  EXPECT_FALSE(text.empty());
  EXPECT_NE(text.find("4"), std::string::npos)
      << "Expected '4' in response. Got: " << text;
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
  EXPECT_GT(response.usage.prompt_tokens, 0);
  EXPECT_GT(response.usage.completion_tokens, 0);
  EXPECT_EQ(response.usage.total_tokens,
            response.usage.prompt_tokens + response.usage.completion_tokens);

  // History should contain user + assistant
  EXPECT_EQ(session.MessageCount(), 2u);
  const auto& messages = session.Transcript().Messages();
  EXPECT_EQ(messages[0].role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(messages[1].VisibleText(), text);
}

TEST_F(ChatSessionTest, AssistantInputAndReplyFollowNativeTemplateBoundaries) {
  TextChatGeneratorFactory factory =
      [](const auto& messages, const auto&, auto& model, const auto&, bool) {
        const auto prompt = BuildChatPrompt(messages, model);
        EXPECT_TRUE(prompt.ends_with(
            "<|im_start|>assistant\nThe answer is<|im_end|>\n<|im_start|>assistant\n"));
        return std::make_unique<FixedOutputGenerator>(" 42.", BackendTerminationCause::kNaturalEnd);
      };
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_, {}, std::move(factory));
  Request request;
  request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "What is it?"));
  request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_ASSISTANT, "The answer is"));

  Response response;
  session.ProcessRequest(request, response);

  ASSERT_EQ(session.MessageCount(), 3u);
  EXPECT_EQ(session.Transcript().Messages()[1].VisibleText(), "The answer is");
  EXPECT_EQ(session.Transcript().Messages()[2].VisibleText(), " 42.");
  EXPECT_TRUE(BuildChatPrompt(session.Transcript().Messages(), GetModel()).ends_with(
      "<|im_start|>assistant\nThe answer is<|im_end|>\n"
      "<|im_start|>assistant\n 42.<|im_end|>\n<|im_start|>assistant\n"));
}

TEST_F(ChatSessionTest, SuppliedCallsDoNotCloseTheGeneratedTurnsVisibleText) {
  for (bool json_request : {false, true}) {
    SCOPED_TRACE(json_request ? "Chat Completions" : "typed request");
    TextChatGeneratorFactory factory = [](const auto&, const auto&, auto&, const auto&, bool) {
      return std::make_unique<FixedOutputGenerator>("New answer.", BackendTerminationCause::kNaturalEnd);
    };
    ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_, {}, std::move(factory));
    std::string streamed;
    session.SetStreamingCallback([&](flStreamingCallbackData event, void*) {
      auto* queue = reinterpret_cast<ItemQueue*>(event.item_queue);
      while (auto item = queue->TryPop()) {
        if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
          const auto& text = static_cast<const TextItem&>(*item);
          if (json_request) {
            const auto chunk = nlohmann::json::parse(text.text);
            const auto& delta = chunk.at("choices").at(0).at("delta");
            if (delta.contains("content") && delta["content"].is_string()) {
              streamed += delta["content"].get<std::string>();
            }
          } else {
            streamed += text.text;
          }
        }
      }
      return 0;
    });

    Request request;
    if (json_request) {
      request.AddOwnedItem(std::make_unique<TextItem>(R"({
        "model":"test-model", "stream":true,
        "messages":[{"role":"user","content":"Again?"},
          {"role":"assistant","content":"","tool_calls":[
            {"id":"prior_call","type":"function","function":{"name":"lookup","arguments":"{}"}}]}]
      })",
                                                      FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    } else {
      request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Again?"));
      request.AddOwnedItem(std::make_unique<ToolCallItem>("prior_call", "lookup", "{}"));
    }

    Response response;
    session.ProcessRequest(request, response);
    EXPECT_EQ(streamed, "New answer.");
    EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
    if (json_request) {
      ASSERT_EQ(response.items.size(), 1u);
      const auto completion = nlohmann::json::parse(static_cast<const TextItem&>(*response.items[0]).text);
      EXPECT_EQ(completion.at("choices").at(0).at("message").at("content"), "New answer.");
    } else {
      EXPECT_EQ(GetAssistantText(response), "New answer.");
      ASSERT_EQ(session.MessageCount(), 3u);
      EXPECT_TRUE(session.Transcript().IsOutstanding("prior_call"));
      EXPECT_EQ(session.Transcript().Messages()[2].VisibleText(), "New answer.");
    }
  }
}

TEST_F(ChatSessionTest, HostEndedTurnsCancelBeforeWaitingForBackendUsage) {
  enum class End { kPostCallText,
                   kStopString,
                   kCallback,
                   kTokenLimit };
  struct Case {
    bool json;
    bool streaming;
    End end;
  };
  const std::vector<Case> cases{
      {false, false, End::kPostCallText},
      {false, true, End::kPostCallText},
      {true, false, End::kPostCallText},
      {true, true, End::kPostCallText},
      {false, false, End::kStopString},
      {false, true, End::kStopString},
      {true, false, End::kStopString},
      {true, true, End::kStopString},
      {false, true, End::kCallback},
      {true, true, End::kCallback},
      {false, false, End::kTokenLimit},
      {false, true, End::kTokenLimit},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(::testing::Message() << "json=" << test.json << " streaming=" << test.streaming
                                      << " end=" << static_cast<int>(test.end));
    // Model an asynchronous backend that is still running after the host stops consuming. Usage records ordering
    // instead of blocking, so a missing cancellation fails deterministically rather than hanging the test.
    GenerationTrace trace;
    trace.requires_cancellation = test.end != End::kCallback;
    TextChatGeneratorFactory factory =
        [&](const auto&, const auto&, auto&, const ToolCallContext& tools, bool) {
          std::string output = "Visible.";
          if (test.end == End::kPostCallText) {
            EXPECT_FALSE(tools.tool_call_start.empty());
            EXPECT_FALSE(tools.tool_call_end.empty());
            output += tools.tool_call_start + R"({"name":"lookup","arguments":{}})" +
                      tools.tool_call_end + "Discarded.";
          } else if (test.end == End::kStopString) {
            output += "STOPDiscarded.";
          }

          return std::make_unique<FixedOutputGenerator>(
              output, BackendTerminationCause::kNaturalEnd, false, &trace);
        };
    ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_, {}, std::move(factory));
    if (!test.json) {
      session.AddToolDefinition({"lookup", "Look up a value", R"({"type":"object","properties":{}})"});
    }

    std::string streamed;
    if (test.streaming) {
      session.SetStreamingCallback([&](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
            streamed += static_cast<const TextItem&>(*item).text;
          }
        }
        return test.end == End::kCallback ? 1 : 0;
      });
    }

    Request request;
    if (test.json) {
      auto body = nlohmann::json::parse(R"({
        "model":"test-model",
        "messages":[{"role":"user","content":"Look it up."}],
        "tools":[{"type":"function","function":{
          "name":"lookup","parameters":{"type":"object","properties":{}}}}]
      })");
      body["stream"] = test.streaming;
      if (test.end == End::kStopString) {
        body["stop"] = "STOP";
      }

      request.AddOwnedItem(std::make_unique<TextItem>(
          body.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    } else {
      request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Look it up."));
      if (test.end == End::kStopString) {
        StoreStopStringsOption({"STOP"}, request.options);
      } else if (test.end == End::kTokenLimit) {
        request.options.Add("max_output_tokens", "1");
      }
    }

    Response response;
    if (test.end == End::kCallback) {
      ExpectOperationCancelled([&] { session.ProcessRequest(request, response); });
    } else {
      ASSERT_NO_THROW(session.ProcessRequest(request, response));
    }
    EXPECT_TRUE(trace.usage_requested);
    EXPECT_TRUE(trace.canceled);
    EXPECT_TRUE(trace.canceled_before_usage);
    EXPECT_EQ(streamed.find("Discarded."), std::string::npos);
    if (test.end == End::kCallback) {
      EXPECT_TRUE(request.IsCompleted());
      EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
      EXPECT_TRUE(session.Transcript().Empty());
      continue;
    }

    EXPECT_EQ(trace.generated_tokens, 1);
    EXPECT_FALSE(request.IsCancellationRequested());
    const auto expected_finish = test.end == End::kPostCallText ? FOUNDRY_LOCAL_FINISH_TOOL_CALLS
                                 : test.end == End::kTokenLimit ? FOUNDRY_LOCAL_FINISH_LENGTH
                                                                : FOUNDRY_LOCAL_FINISH_STOP;
    EXPECT_EQ(response.finish_reason, expected_finish);
    if (test.json) {
      ASSERT_EQ(response.items.size(), 1u);
      const auto completion = nlohmann::json::parse(static_cast<const TextItem&>(*response.items[0]).text);
      const auto& choice = completion.at("choices").at(0);
      EXPECT_EQ(choice.at("message").at("content"), "Visible.");
      if (test.end == End::kPostCallText) {
        EXPECT_EQ(choice.at("finish_reason"), "tool_calls");
        ASSERT_EQ(choice.at("message").at("tool_calls").size(), 1u);
      }
    } else {
      EXPECT_EQ(GetAssistantText(response), "Visible.");
      EXPECT_EQ(session.TurnCount(), 1u);
      EXPECT_EQ(session.Transcript().OutstandingCallCount(), test.end == End::kPostCallText ? 1u : 0u);
    }
  }
}

TEST_F(ChatSessionTest, ForcedBuiltInRawCallRemainsVisibleWhenPromptOpensReasoning) {
  const std::string envelope =
      "*** Begin Patch\n*** Delete File: old.txt\n*** End Patch";
  TextChatGeneratorFactory factory =
      [envelope](const auto&, const auto&, auto&, const auto&, bool use_full_context) {
        EXPECT_TRUE(use_full_context);
        return std::make_unique<FixedOutputGenerator>(
            envelope, BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/true);
      };
  auto catalog_model = MakeReasoningCatalogModel();
  ChatSession session(catalog_model, GetModel(), *logger_, null_telemetry_, {},
                      std::move(factory));
  session.AddToolDefinition(tools::MakeCustomTool(
      "apply_patch", "Apply a patch.", /*description_present=*/true,
      std::string(tools::kStockGhcpApplyPatchLarkGrammar)));

  std::string callback_call_id;
  std::string callback_arguments;
  session.SetStreamingCallback([&](flStreamingCallbackData event, void*) {
    auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
    const auto item = queue->TryPop();
    if (item && item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
      const auto& call = static_cast<const ToolCallItem&>(*item);
      callback_call_id = call.call_id;
      callback_arguments = call.arguments;
    }

    return 0;
  });

  Request request;
  request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "make a patch"));
  request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, "required");
  request.forced_tool_choice = ForcedToolChoice{"apply_patch", ToolKind::kCustom};

  Response response;
  session.ProcessRequest(request, response);

  const auto response_call = std::ranges::find_if(response.items, [](const auto& item) {
    return item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL;
  });
  ASSERT_NE(response_call, response.items.end());
  const auto& call = static_cast<const ToolCallItem&>(**response_call);
  EXPECT_EQ(callback_call_id, call.call_id);
  EXPECT_EQ(callback_arguments, envelope);
  EXPECT_EQ(call.arguments, envelope);
  EXPECT_EQ(call.generated_encoding, GeneratedCallEncoding::kRawEnvelope);

  ASSERT_EQ(session.Transcript().Messages().back().ToolCalls().size(), 1u);
  const auto& transcript_call = *session.Transcript().Messages().back().ToolCalls().front();
  EXPECT_EQ(transcript_call.call_id, call.call_id);
  EXPECT_EQ(transcript_call.arguments, envelope);
}

TEST_F(ChatSessionTest, ChatCompletionsForcedRawCallPreservesJsonShapedPayloadAndPromptOpenedReasoning) {
  const std::string envelope = "{\n\"input\":\"replacement text\"\n}";
  const std::string output = "private reasoning</think>\n" + envelope;
  TextChatGeneratorFactory factory =
      [output](const auto&, const auto&, auto&, const auto&, bool use_full_context) {
        EXPECT_FALSE(use_full_context);
        return std::make_unique<FixedOutputGenerator>(
            output, BackendTerminationCause::kNaturalEnd,
            /*prompt_opens_reasoning=*/true);
      };
  auto catalog_model = MakeReasoningCatalogModel();
  ChatSession session(catalog_model, GetModel(), *logger_, null_telemetry_, {},
                      std::move(factory));

  const auto descriptor =
      nlohmann::json{{"type", "raw_envelope"},
                     {"tool_name", "edit"},
                     {"start_marker", "{"},
                     {"end_marker", "}"}}
          .dump();
  const auto request_json =
      nlohmann::json{
          {"model", GetModel().ModelId()},
          {"messages", nlohmann::json::array({
                           {{"role", "user"}, {"content", "replace the text"}},
                       })},
          {"tools", nlohmann::json::array({
                        {{"type", "custom"},
                         {"custom", {{"name", "edit"}, {"format", {{"type", "text"}}}}}},
                    })},
          {"tool_choice", {{"type", "custom"}, {"custom", {{"name", "edit"}}}}},
          {"metadata", {{tools::kRawEnvelopeMetadataKey, descriptor}}}};

  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>(
      request_json.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  Response response;
  session.ProcessRequest(request, response);

  ASSERT_EQ(response.items.size(), 1u);
  ASSERT_EQ(response.items.front()->type, FOUNDRY_LOCAL_ITEM_TEXT);
  const auto& response_item = static_cast<const TextItem&>(*response.items.front());
  const auto completion = nlohmann::json::parse(response_item.text);
  const auto& message = completion.at("choices").at(0).at("message");
  EXPECT_EQ(message.at("reasoning_content"), "private reasoning");
  ASSERT_EQ(message.at("tool_calls").size(), 1u);
  EXPECT_EQ(message.at("tool_calls").at(0).at("custom").at("name"), "edit");
  EXPECT_EQ(message.at("tool_calls").at(0).at("custom").at("input"), envelope);
  EXPECT_EQ(completion.at("choices").at(0).at("finish_reason"), "tool_calls");
}

TEST_F(ChatSessionTest, ChatCompletionRejectsAudioInput) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<AudioItem>(std::vector<std::uint8_t>(32000), "pcm"));

  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(parts)));

  Response response;
  try {
    session.ProcessRequest(request, response);
    FAIL() << "Expected unsupported audio input to be rejected";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(e.what()).find("AUDIO input is not supported"), std::string::npos);
    EXPECT_NE(std::string(e.what()).find("chat-completion"), std::string::npos);
  }
}

TEST_F(ChatSessionTest, ChatCompletionRejectsImageInput) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<ImageItem>(std::vector<std::uint8_t>{1}, "png"));

  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(parts)));

  Response response;
  try {
    session.ProcessRequest(request, response);
    FAIL() << "Expected image input to be rejected by a chat-completion model";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(e.what()).find("IMAGE input is not supported"), std::string::npos);
    EXPECT_NE(std::string(e.what()).find("chat-completion"), std::string::npos);
  }
}

TEST_F(ChatSessionTest, RunWithStreaming) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  // Use a multi-token prompt with deterministic substrings so we can validate:
  //   1. Streaming actually delivers multiple deltas (callback_count >= 2),
  //      not a single coalesced item.
  //   2. The streamed content matches expectations (at least 2 of the 4 UK
  //      constituent country names appear). A 0.5B model may abbreviate or
  //      reorder; requiring a subset stays robust.
  Request request;
  request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Name the countries in the United Kingdom."));
  request.options.Add("max_output_tokens", "128");
  request.options.Add("temperature", "0");

  std::string streamed_text;
  int callback_count = 0;
  int local_user_data = 123;  // example user data to pass to callback
  fl::Session::StreamingCallbackFn callback_fn = [&](flStreamingCallbackData event, void* user_data) -> int {
    EXPECT_EQ(user_data, &local_user_data) << "User data pointer mismatch in callback";

    fl::ItemQueue* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
    auto item = queue->TryPop();
    if (!item) {
      // should never happen
      return 0;
    }

    EXPECT_EQ(item->type, FOUNDRY_LOCAL_ITEM_TEXT);
    auto& text_item = static_cast<fl::TextItem&>(*item);
    std::string delta_text = text_item.text;
    streamed_text += delta_text;
    ++callback_count;

    return 0;
  };

  session.SetStreamingCallback(callback_fn, &local_user_data);

  Response response;
  session.ProcessRequest(request, response);
  auto text = GetAssistantText(response);

  EXPECT_FALSE(text.empty());

  // Lowercase the final text for case-insensitive substring matches.
  std::string lower = fl::test::ToLower(text);

  const std::vector<std::string> uk_countries = {"england", "scotland", "wales", "ireland"};
  int found = 0;
  for (const auto& name : uk_countries) {
    if (lower.find(name) != std::string::npos) {
      ++found;
    }
  }

  EXPECT_GE(found, 2)
      << "Expected at least 2 UK country names in response. Got: " << text;

  // Streamed text should match the final result.
  EXPECT_EQ(streamed_text, text);

  // Real streaming must deliver more than a single coalesced delta.
  EXPECT_GE(callback_count, 2)
      << "Expected multiple streaming callbacks (real token-by-token streaming), "
      << "got " << callback_count << ". Final text: " << text;

  // Turn 2 — a context-dependent follow-up. Asking for the capital of each
  // exercises history-aware generation and gives a second deterministic
  // content check. Streaming state is reused on the same session.
  streamed_text.clear();
  callback_count = 0;

  Request request2;
  request2.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "What is the capital of each?"));
  request2.options.Add("max_output_tokens", "128");
  request2.options.Add("temperature", "0");

  Response response2;
  session.ProcessRequest(request2, response2);
  auto text2 = GetAssistantText(response2);

  EXPECT_FALSE(text2.empty());

  std::string lower2 = fl::test::ToLower(text2);

  const std::vector<std::string> uk_capitals = {"london", "edinburgh", "cardiff", "belfast"};
  int found2 = 0;
  for (const auto& name : uk_capitals) {
    if (lower2.find(name) != std::string::npos) {
      ++found2;
    }
  }

  EXPECT_GE(found2, 2)
      << "Turn 2: expected at least 2 UK capital names in response. Got: " << text2;
  EXPECT_EQ(streamed_text, text2);
  EXPECT_GE(callback_count, 2)
      << "Turn 2: expected multiple streaming callbacks, got " << callback_count
      << ". Final text: " << text2;
}

TEST_F(ChatSessionTest, RunMultiTurn) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  // Turn 1
  Request req1;
  req1.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "What is 2+2? Answer with just the number."));
  req1.options.Add("max_output_tokens", "32");
  req1.options.Add("temperature", "0");

  Response r1;
  session.ProcessRequest(req1, r1);
  auto t1 = GetAssistantText(r1);
  EXPECT_NE(t1.find("4"), std::string::npos)
      << "Turn 1: expected '4'. Got: " << t1;
  EXPECT_EQ(session.MessageCount(), 2u);
  EXPECT_EQ(session.TurnCount(), 1u);

  // Turn 2 — Run adds to existing history
  Request req2;
  req2.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Now add 1 to that. Answer with just the number."));
  req2.options.Add("max_output_tokens", "32");
  req2.options.Add("temperature", "0");

  Response r2;
  session.ProcessRequest(req2, r2);
  auto t2 = GetAssistantText(r2);
  EXPECT_NE(t2.find("5"), std::string::npos)
      << "Turn 2: expected '5'. Got: " << t2;
  EXPECT_EQ(session.MessageCount(), 4u);
}

#if FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS
TEST_F(ChatSessionTest, ChatTemplateKwargsControlCachedGeneratorReuse) {
  struct TurnResult {
    std::string text;
    int64_t prompt_tokens;
  };

  auto run_turn = [&](ChatSession& session,
                      const std::vector<std::pair<flMessageRole, std::string>>& messages,
                      const char* template_kwargs) {
    Request request;
    for (const auto& [role, content] : messages) {
      request.AddOwnedItem(MakeMessage(role, content));
    }

    request.options.Add("max_output_tokens", "32");
    request.options.Add("temperature", "0");
    if (template_kwargs) {
      request.options.Add("chat_template_kwargs", template_kwargs);
    }

    Response response;
    session.ProcessRequest(request, response);
    EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
    return TurnResult{GetAssistantText(response), response.usage.prompt_tokens};
  };

  constexpr const char* kFirstPrompt = "Reply briefly: one.";
  constexpr const char* kSecondPrompt = "Reply briefly: two.";
  constexpr const char* kThirdPrompt = "Reply briefly: three.";
  constexpr const char* kFourthPrompt = "Reply briefly: four.";
  constexpr const char* kNoThinking = R"({"enable_thinking":false})";
  constexpr const char* kThinking = R"({"enable_thinking":true})";

  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  auto first = run_turn(session, {{FOUNDRY_LOCAL_ROLE_USER, kFirstPrompt}}, kNoThinking);
  auto same_kwargs = run_turn(session, {{FOUNDRY_LOCAL_ROLE_USER, kSecondPrompt}}, kNoThinking);
  ASSERT_EQ(session.Transcript().Turns().size(), 2u);
  EXPECT_TRUE(session.Transcript().Turns()[1].tokens.pre_turn.has_value())
      << "Identical template kwargs should retain the continuous-decoding path";

  ChatSession same_kwargs_baseline(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  auto same_kwargs_full_history = run_turn(
      same_kwargs_baseline,
      {{FOUNDRY_LOCAL_ROLE_USER, kFirstPrompt},
       {FOUNDRY_LOCAL_ROLE_ASSISTANT, first.text},
       {FOUNDRY_LOCAL_ROLE_USER, kSecondPrompt}},
      kNoThinking);
  EXPECT_EQ(same_kwargs.text, same_kwargs_full_history.text);
  EXPECT_EQ(same_kwargs.prompt_tokens, same_kwargs_full_history.prompt_tokens);

  auto changed_kwargs = run_turn(session, {{FOUNDRY_LOCAL_ROLE_USER, kThirdPrompt}}, kThinking);
  ASSERT_EQ(session.Transcript().Turns().size(), 3u);
  EXPECT_FALSE(session.Transcript().Turns()[2].tokens.pre_turn.has_value())
      << "A kwargs-triggered rebuild has no valid rewind boundary in the previous generator";

  ChatSession changed_kwargs_baseline(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  auto changed_kwargs_full_history = run_turn(
      changed_kwargs_baseline,
      {{FOUNDRY_LOCAL_ROLE_USER, kFirstPrompt},
       {FOUNDRY_LOCAL_ROLE_ASSISTANT, first.text},
       {FOUNDRY_LOCAL_ROLE_USER, kSecondPrompt},
       {FOUNDRY_LOCAL_ROLE_ASSISTANT, same_kwargs.text},
       {FOUNDRY_LOCAL_ROLE_USER, kThirdPrompt}},
      kThinking);
  EXPECT_EQ(changed_kwargs.text, changed_kwargs_full_history.text);
  EXPECT_EQ(changed_kwargs.prompt_tokens, changed_kwargs_full_history.prompt_tokens)
      << "Changing template kwargs must rebuild the generator from full history";

  session.UndoTurns(1);
  EXPECT_EQ(session.TurnCount(), 2u);
  auto replayed_changed_kwargs =
      run_turn(session, {{FOUNDRY_LOCAL_ROLE_USER, kThirdPrompt}}, kThinking);
  EXPECT_EQ(replayed_changed_kwargs.text, changed_kwargs_full_history.text);
  EXPECT_EQ(replayed_changed_kwargs.prompt_tokens, changed_kwargs_full_history.prompt_tokens)
      << "Undoing a kwargs-triggered rebuild must discard stale rewind state and replay full history";

  auto removed_kwargs = run_turn(session, {{FOUNDRY_LOCAL_ROLE_USER, kFourthPrompt}}, nullptr);
  ASSERT_EQ(session.Transcript().Turns().size(), 4u);
  EXPECT_FALSE(session.Transcript().Turns()[3].tokens.pre_turn.has_value());

  ChatSession removed_kwargs_baseline(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  auto removed_kwargs_full_history = run_turn(
      removed_kwargs_baseline,
      {{FOUNDRY_LOCAL_ROLE_USER, kFirstPrompt},
       {FOUNDRY_LOCAL_ROLE_ASSISTANT, first.text},
       {FOUNDRY_LOCAL_ROLE_USER, kSecondPrompt},
       {FOUNDRY_LOCAL_ROLE_ASSISTANT, same_kwargs.text},
       {FOUNDRY_LOCAL_ROLE_USER, kThirdPrompt},
       {FOUNDRY_LOCAL_ROLE_ASSISTANT, replayed_changed_kwargs.text},
       {FOUNDRY_LOCAL_ROLE_USER, kFourthPrompt}},
      nullptr);
  EXPECT_EQ(removed_kwargs.text, removed_kwargs_full_history.text);
  EXPECT_EQ(removed_kwargs.prompt_tokens, removed_kwargs_full_history.prompt_tokens)
      << "Removing template kwargs must rebuild from full history and clear tokenizer state";

  session.UndoTurns(1);
  EXPECT_EQ(session.TurnCount(), 3u);
  auto replayed_removed_kwargs = run_turn(session, {{FOUNDRY_LOCAL_ROLE_USER, kFourthPrompt}}, nullptr);
  EXPECT_EQ(replayed_removed_kwargs.text, removed_kwargs_full_history.text);
  EXPECT_EQ(replayed_removed_kwargs.prompt_tokens, removed_kwargs_full_history.prompt_tokens);
}

TEST_F(ChatSessionTest, SessionTemplateDefaultsInvalidateOnlyWhenEffectiveKwargsChange) {
  constexpr const char* kThinking = R"({"enable_thinking":true})";
  constexpr const char* kNoThinking = R"({"enable_thinking":false})";
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  KeyValuePairs defaults;
  defaults.Add("chat_template_kwargs", kNoThinking);
  session.SetSessionOptions(defaults);

  auto run_turn = [&](const char* prompt, const char* kwargs = nullptr) {
    Request request;
    request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, prompt));
    request.options.Add("max_output_tokens", "32");
    request.options.Add("temperature", "0");
    if (kwargs) {
      request.options.Add("chat_template_kwargs", kwargs);
    }

    Response response;
    session.ProcessRequest(request, response);
    EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
    return session.Transcript().Turns().back().tokens.pre_turn.has_value();
  };

  EXPECT_FALSE(run_turn("Reply briefly: one."));
  EXPECT_TRUE(run_turn("Reply briefly: two.", kNoThinking))
      << "Explicitly repeating the inherited default must not rebuild the generator";

  defaults.Add("chat_template_kwargs", kThinking);
  session.SetSessionOptions(defaults);
  EXPECT_TRUE(run_turn("Reply briefly: three.", kNoThinking))
      << "A request override keeps the effective kwargs unchanged when the session default changes";
  EXPECT_FALSE(run_turn("Reply briefly: four."))
      << "Using a changed session default must rebuild without a stale rewind boundary";

  session.SetSessionOptions({});
  EXPECT_FALSE(run_turn("Reply briefly: five."))
      << "Removing the session default must invalidate retained template state";
}

TEST_F(ChatSessionTest, ChatTemplateKwargsCancellationReplaysFullHistory) {
  struct TurnResult {
    std::string text;
    int64_t prompt_tokens;
  };

  auto run_turn = [&](ChatSession& session,
                      const std::vector<std::pair<flMessageRole, std::string>>& messages,
                      const char* template_kwargs,
                      int max_output_tokens = 32) {
    Request request;
    for (const auto& [role, content] : messages) {
      request.AddOwnedItem(MakeMessage(role, content));
    }

    request.options.Add("max_output_tokens", std::to_string(max_output_tokens));
    request.options.Add("temperature", "0");
    if (template_kwargs) {
      request.options.Add("chat_template_kwargs", template_kwargs);
    }

    Response response;
    session.ProcessRequest(request, response);
    return std::pair{TurnResult{GetAssistantText(response), response.usage.prompt_tokens},
                     response.finish_reason};
  };

  constexpr const char* kFirstPrompt = "Reply briefly: one.";
  constexpr const char* kSecondPrompt = "Using the previous turn, reply briefly: two.";
  constexpr const char* kThinking = R"({"enable_thinking":true})";
  constexpr const char* kNoThinking = R"({"enable_thinking":false})";

  const char* next_kwargs_values[] = {kNoThinking, nullptr};
  for (const char* next_kwargs : next_kwargs_values) {
    SCOPED_TRACE(next_kwargs ? "changed kwargs" : "removed kwargs");
    ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
    auto [first, first_finish] =
        run_turn(session, {{FOUNDRY_LOCAL_ROLE_USER, kFirstPrompt}}, kThinking);
    ASSERT_EQ(first_finish, FOUNDRY_LOCAL_FINISH_STOP);

    int streamed_items = 0;
    bool cancel_enabled = true;
    session.SetStreamingCallback(
        [&](flStreamingCallbackData event, void*) -> int {
          auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
          if (queue->TryPop()) {
            ++streamed_items;
          }
          return cancel_enabled && streamed_items >= 1 ? 1 : 0;
        });

    try {
      run_turn(session, {{FOUNDRY_LOCAL_ROLE_USER, kSecondPrompt}}, next_kwargs);
      FAIL() << "Expected streaming callback cancellation";
    } catch (const fl::Exception& error) {
      EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
    }
    
    EXPECT_EQ(session.TurnCount(), 1u);
    EXPECT_EQ(session.MessageCount(), 2u);

    cancel_enabled = false;
    auto [replayed, replayed_finish] =
        run_turn(session, {{FOUNDRY_LOCAL_ROLE_USER, kSecondPrompt}}, next_kwargs);
    EXPECT_NE(replayed_finish, FOUNDRY_LOCAL_FINISH_ERROR);
    ASSERT_EQ(session.Transcript().Turns().size(), 2u);
    EXPECT_FALSE(session.Transcript().Turns()[1].tokens.pre_turn.has_value());

    ChatSession baseline(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
    auto [full_history, baseline_finish] = run_turn(
        baseline,
        {{FOUNDRY_LOCAL_ROLE_USER, kFirstPrompt},
         {FOUNDRY_LOCAL_ROLE_ASSISTANT, first.text},
         {FOUNDRY_LOCAL_ROLE_USER, kSecondPrompt}},
        next_kwargs);
    EXPECT_NE(baseline_finish, FOUNDRY_LOCAL_FINISH_ERROR);
    EXPECT_EQ(replayed.text, full_history.text);
    EXPECT_EQ(replayed.prompt_tokens, full_history.prompt_tokens)
        << "Cancellation after a kwargs-triggered rebuild must discard retained state before replay";
  }
}
#endif

TEST_F(ChatSessionTest, AppendedClassicGeneratorIsDiscardedAfterStreamingCancellation) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  ASSERT_EQ(GetModel().GetGenAIConfig().GetChatBackendKind(), ChatBackendKind::kGenerator);

  Request seed;
  seed.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Remember the word sapphire. Reply OK."));
  seed.options.Add("max_output_tokens", "32");
  seed.options.Add("temperature", "0");

  Response seed_response;
  session.ProcessRequest(seed, seed_response);
  ASSERT_EQ(session.MessageCount(), 2u);
  ASSERT_EQ(session.TurnCount(), 1u);

  Request request;
  request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Count from 1 to 100."));
  request.options.Add("max_output_tokens", "256");
  request.options.Add("temperature", "0");

  int tokens_received = 0;
  bool cancel_enabled = true;

  fl::Session::StreamingCallbackFn callback_fn = [&](flStreamingCallbackData event, void* /*user_data*/) -> int {
    fl::ItemQueue* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
    auto item = queue->TryPop();
    if (!item) {
      // should never happen
      return 0;
    }

    ++tokens_received;
    bool cancel = cancel_enabled && tokens_received >= 3;  // cancel the first request after receiving 3 tokens
    return cancel ? 1 : 0;
  };

  session.SetStreamingCallback(callback_fn);

  Response response;
  ExpectOperationCancelled([&] { session.ProcessRequest(request, response); });

  // The canceled response is not published.
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_TRUE(response.items.empty());
  // Cancellation is observed between token steps, so allow for a couple of extra tokens after the callback.
  EXPECT_LE(tokens_received, 6);

  // The appended canceled turn commits nothing; only the seed turn remains.
  EXPECT_EQ(session.MessageCount(), 2u);
  EXPECT_EQ(session.TurnCount(), 1u);

  cancel_enabled = false;
  Request retry;
  retry.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "What is 2+2? Answer with just the number."));
  retry.options.Add("max_output_tokens", "32");
  retry.options.Add("temperature", "0");

  Response retry_response;
  session.ProcessRequest(retry, retry_response);
  const auto retry_text = GetAssistantText(retry_response);

  EXPECT_NE(retry_text.find("4"), std::string::npos)
      << "The generator rebuilt from committed history should recover after cancellation. Got: " << retry_text;
  EXPECT_EQ(retry_response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
  EXPECT_EQ(session.MessageCount(), 4u);
  EXPECT_EQ(session.TurnCount(), 2u);
}

TEST_F(ChatSessionTest, CancellationFromTheLastQueuedCallbackPreventsCommit) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Reply with one word."));
  request.options.Add("max_output_tokens", "1");
  request.options.Add("temperature", "0");

  session.SetStreamingCallback([](flStreamingCallbackData event, void*) {
    auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
    auto item = queue->TryPop();
    if (!item) {
      return 0;
    }

    // Generation has only one token to produce, so this callback remains outstanding after the generator finishes.
    // ProcessRequest must drain it and observe cancellation before committing the turn.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 1;
  });

  Response response;
  ExpectOperationCancelled([&] { session.ProcessRequest(request, response); });

  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_TRUE(response.items.empty());
  EXPECT_EQ(session.MessageCount(), 0u);
  EXPECT_EQ(session.TurnCount(), 0u);
}

TEST_F(ChatSessionTest, OpenAIJsonCancellationFromLastContentCallbackPublishesNoTerminalSuccess) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  nlohmann::json request_json = {
      {"model", GetModel().ModelId()},
      {"messages", nlohmann::json::array({
                       {{"role", "user"}, {"content", "Reply with one word."}},
                   })},
      {"max_tokens", 1},
      {"temperature", 0}};

  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>(
      request_json.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  std::vector<nlohmann::json> delivered;
  session.SetStreamingCallback([&delivered](flStreamingCallbackData event, void*) {
    auto* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
    auto item = queue->TryPop();
    if (!item) {
      return 0;
    }

    const auto& text = static_cast<const TextItem&>(*item);
    auto chunk = nlohmann::json::parse(text.text);
    const bool has_content =
        chunk["choices"][0]["delta"].contains("content") &&
        !chunk["choices"][0]["delta"]["content"].get<std::string>().empty();
    delivered.push_back(std::move(chunk));

    if (has_content) {
      // Keep the final content delivery outstanding until generation has naturally completed.
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      return 1;
    }

    return 0;
  });

  Response response;
  ExpectOperationCancelled([&] { session.ProcessRequest(request, response); });

  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_TRUE(response.items.empty());
  ASSERT_EQ(delivered.size(), 2u);
  EXPECT_EQ(delivered[0]["choices"][0]["delta"]["role"], "assistant");
  EXPECT_FALSE(delivered[0]["choices"][0]["finish_reason"].is_string());
  EXPECT_TRUE(delivered[1]["choices"][0]["delta"].contains("content"));
  EXPECT_FALSE(delivered[1]["choices"][0]["finish_reason"].is_string());
  EXPECT_EQ(session.MessageCount(), 0u);
  EXPECT_EQ(session.TurnCount(), 0u);
}

TEST_F(ChatSessionTest, TranscriptUndoFailureLeavesGeneratorAndConversationUsable) {
  bool fail_undo_before_publish = false;
  ChatSession session(
      GetCatalogModel(), GetModel(), *logger_, null_telemetry_,
      [&fail_undo_before_publish](ChatTranscript::CommitPhase phase) {
        if (fail_undo_before_publish && phase == ChatTranscript::CommitPhase::kUndoBeforePublish) {
          throw std::runtime_error("injected undo failure");
        }
      });

  Request first;
  first.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "What is 2+2? Answer with just the number."));
  first.options.Add("max_output_tokens", "32");
  first.options.Add("temperature", "0");
  Response first_response;
  session.ProcessRequest(first, first_response);

  Request second;
  second.AddOwnedItem(MakeMessage(
      FOUNDRY_LOCAL_ROLE_USER, "Now add 1 to that. Answer with just the number."));
  second.options.Add("max_output_tokens", "32");
  second.options.Add("temperature", "0");
  Response second_response;
  session.ProcessRequest(second, second_response);
  const auto original_second_text = GetAssistantText(second_response);
  const auto prompt_before = BuildChatMessagesJson(session.Transcript().Messages());

  fail_undo_before_publish = true;
  EXPECT_THROW(session.UndoTurns(1), std::runtime_error);
  EXPECT_EQ(session.TurnCount(), 2u);
  EXPECT_EQ(session.MessageCount(), 4u);
  EXPECT_EQ(BuildChatMessagesJson(session.Transcript().Messages()), prompt_before);

  fail_undo_before_publish = false;
  EXPECT_NO_THROW(session.UndoTurns(1));
  EXPECT_EQ(session.TurnCount(), 1u);
  EXPECT_EQ(session.MessageCount(), 2u);

  Request retry;
  retry.AddOwnedItem(MakeMessage(
      FOUNDRY_LOCAL_ROLE_USER, "Now add 1 to that. Answer with just the number."));
  retry.options.Add("max_output_tokens", "32");
  retry.options.Add("temperature", "0");
  Response retry_response;
  session.ProcessRequest(retry, retry_response);

  EXPECT_EQ(GetAssistantText(retry_response), original_second_text);
  EXPECT_EQ(session.TurnCount(), 2u);
  EXPECT_EQ(session.MessageCount(), 4u);
}

// ===========================================================================
// SearchOptions::FromParameters
// ===========================================================================

TEST_F(ChatSessionTest, SearchOptionsFromParameters) {
  fl::KeyValuePairs params;
  params.Add(FOUNDRY_LOCAL_PARAM_TEMPERATURE, "0.7");
  params.Add(FOUNDRY_LOCAL_PARAM_TOP_P, "0.9");
  params.Add(FOUNDRY_LOCAL_PARAM_MAX_OUTPUT_TOKENS, "128");
  params.Add(FOUNDRY_LOCAL_PARAM_SEED, "42");

  auto opts = SearchOptions::FromParameters(params);

  EXPECT_FLOAT_EQ(*opts.temperature, 0.7f);
  EXPECT_FLOAT_EQ(*opts.top_p, 0.9f);
  EXPECT_EQ(*opts.max_output_tokens, 128);
  EXPECT_EQ(*opts.seed, 42);
  EXPECT_FALSE(opts.top_k.has_value());
  EXPECT_FALSE(opts.frequency_penalty.has_value());
}

TEST_F(ChatSessionTest, SearchOptionsFromEmptyParameters) {
  fl::KeyValuePairs params;
  auto opts = SearchOptions::FromParameters(params);

  EXPECT_FALSE(opts.temperature.has_value());
  EXPECT_FALSE(opts.max_output_tokens.has_value());
}

// ===========================================================================
// Request-scoped system prefix (`instructions`) against a warm session.
//
// The prefix is baked into a generator's prompt, so a turn that changes it must rebuild while a turn that repeats it
// keeps the KV cache. Public usage always reports the complete logical prompt, so warm results are compared with an
// equivalent freshly rebuilt session rather than exposing the internal suffix-token optimization.
// ===========================================================================

namespace {

/// Run one turn. `instructions` is the request-scoped system prefix; empty means none.
Response RunTurnResponse(ChatSession& session, const std::string& user_text, const std::string& instructions,
                         bool disable_tools = false, int max_output_tokens = 8) {
  Request request;
  if (!user_text.empty()) {
    request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, user_text));
  }

  if (!instructions.empty()) {
    request.options.Add(kSystemPromptOption, instructions);
  }
  if (disable_tools) {
    request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, "none");
  }

  request.options.Add("max_output_tokens", std::to_string(max_output_tokens));
  request.options.Add("temperature", "0");

  Response response;
  session.ProcessRequest(request, response);
  return response;
}

fl::TokenUsage RunTurn(ChatSession& session, const std::string& user_text, const std::string& instructions) {
  return RunTurnResponse(session, user_text, instructions).usage;
}

}  // namespace

TEST_F(ChatSessionTest, UnchangedInstructionsKeepTheCachedGeneratorAndDoNotRepeatThePrefix) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  const std::string instructions = "You are a terse assistant that answers in one word.";
  const std::string first_user = "Say ok.";
  const std::string second_user = "Say ok again.";

  auto first_response = RunTurnResponse(session, first_user, instructions);
  const auto second = RunTurn(session, second_user, instructions);

  EXPECT_GT(first_response.usage.prompt_tokens, 0);
  EXPECT_GT(second.prompt_tokens, 0);
  EXPECT_GT(second.prompt_tokens, first_response.usage.prompt_tokens)
      << "the second turn's input sequence includes the conversation already held by the generator";
  EXPECT_LE(second.completion_tokens, 8);
  ASSERT_EQ(session.Transcript().Turns().size(), 2u);
  EXPECT_TRUE(session.Transcript().Turns()[1].tokens.pre_turn.has_value())
      << "unchanged instructions should append to the existing generator rather than rebuild it";

  // The prefix is request state and never enters the conversation record: two turns, four messages, no system
  // message among them.
  ASSERT_EQ(session.MessageCount(), 4u);
  for (const auto& message : session.Transcript().Messages()) {
    EXPECT_NE(message.role, FOUNDRY_LOCAL_ROLE_SYSTEM) << "the system prefix must not be committed to history";
  }
}

TEST_F(ChatSessionTest, OrdinaryWarmContinuationMatchesFreshFullTranscriptReplay) {
  ChatSession warm_session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  const std::string first_user = "What is 2+2? Answer with just the number.";
  const std::string second_user = "Add 1 to the previous answer. Answer with just the number.";

  const auto first = RunTurnResponse(warm_session, first_user, "", /*disable_tools=*/true,
                                     /*max_output_tokens=*/32);
  ASSERT_EQ(first.finish_reason, FOUNDRY_LOCAL_FINISH_STOP)
      << "the first turn must finish naturally so its Generator can be retained";
  auto full_messages = warm_session.Transcript().Messages();
  full_messages.emplace_back(FOUNDRY_LOCAL_ROLE_USER, second_user);
  const auto expected_prompt = BuildChatPrompt(full_messages, GetModel());

  const auto warm = RunTurnResponse(warm_session, second_user, "", /*disable_tools=*/true,
                                    /*max_output_tokens=*/32);
  ASSERT_TRUE(warm_session.Transcript().Turns()[1].tokens.pre_turn.has_value())
      << "the ordinary continuation must exercise the retained Generator path";

  ChatSession fresh_session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  Request fresh_request;
  fresh_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, first_user));
  fresh_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_ASSISTANT, GetAssistantText(first)));
  fresh_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, second_user));
  fresh_request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, "none");
  fresh_request.options.Add("max_output_tokens", "32");
  fresh_request.options.Add("temperature", "0");

  const auto fresh_input = BuildTranscriptMessages(fresh_request.items);
  EXPECT_EQ(BuildChatPrompt(fresh_input, GetModel()), expected_prompt)
      << "the warm logical prompt and fresh rendered prompt must be identical";

  Response fresh;
  fresh_session.ProcessRequest(fresh_request, fresh);

  EXPECT_EQ(warm.usage.prompt_tokens, fresh.usage.prompt_tokens);
  EXPECT_EQ(GetAssistantText(warm), GetAssistantText(fresh));
  EXPECT_EQ(warm.finish_reason, fresh.finish_reason);
  EXPECT_EQ(warm.usage.completion_tokens, fresh.usage.completion_tokens);
}

TEST_F(ChatSessionTest, WarmTurnThatStopsBeforeItsLimitReportsStop) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  const std::string instructions = "Answer each request with only the word ok.";
  RunTurnResponse(session, "Say ok.", instructions, /*disable_tools=*/false, /*max_output_tokens=*/64);
  const auto second =
      RunTurnResponse(session, "Say ok again.", instructions, /*disable_tools=*/false, /*max_output_tokens=*/64);

  EXPECT_EQ(second.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
  EXPECT_GT(second.usage.completion_tokens, 0);
  EXPECT_LT(second.usage.completion_tokens, 64);
  EXPECT_EQ(second.usage.total_tokens, second.usage.prompt_tokens + second.usage.completion_tokens);
  EXPECT_TRUE(session.Transcript().Turns()[1].tokens.pre_turn.has_value());
}

TEST_F(ChatSessionTest, ChangedInstructionsRebuildTheGeneratorWithTheNewPrefix) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  const std::string first_instructions = "You are a terse assistant that answers in one word.";
  // Deliberately much longer: a rebuilt prompt has to carry it, so the token count cannot be mistaken for an append.
  const std::string second_instructions =
      "You are an extremely careful assistant. Answer in one word. Do not explain yourself. Do not apologise. "
      "Do not add pleasantries. Do not restate the question. Keep every answer as short as it can possibly be.";

  const auto first = RunTurnResponse(session, "Say ok.", first_instructions);
  const auto second = RunTurnResponse(session, "Say ok again.", first_instructions);
  const auto rebuilt = RunTurnResponse(session, "Say ok once more.", second_instructions);

  ChatSession fresh_session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  Request fresh_request;
  fresh_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Say ok."));
  fresh_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_ASSISTANT, GetAssistantText(first)));
  fresh_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Say ok again."));
  fresh_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_ASSISTANT, GetAssistantText(second)));
  fresh_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Say ok once more."));
  fresh_request.options.Add(kSystemPromptOption, second_instructions);
  fresh_request.options.Add("max_output_tokens", "8");
  fresh_request.options.Add("temperature", "0");

  Response fresh;
  fresh_session.ProcessRequest(fresh_request, fresh);

  EXPECT_EQ(rebuilt.usage.prompt_tokens, fresh.usage.prompt_tokens)
      << "changing instructions must rebuild the same prompt as a fresh session";
  EXPECT_EQ(rebuilt.usage.total_tokens, rebuilt.usage.prompt_tokens + rebuilt.usage.completion_tokens);

  // Still nothing in the record: a changed prefix replaces the old one rather than stacking another system message.
  ASSERT_EQ(session.MessageCount(), 6u);
  for (const auto& message : session.Transcript().Messages()) {
    EXPECT_NE(message.role, FOUNDRY_LOCAL_ROLE_SYSTEM) << "the system prefix must not be committed to history";
  }
}

TEST_F(ChatSessionTest, RemovingToolDefinitionsRebuildsWithTheCurrentToolSet) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  session.AddToolDefinition({"lookup", "Look up a value",
                             R"({"type":"object","properties":{"key":{"type":"string"}}})"});

  const std::string first_user = "Reply with the single word ok.";
  const std::string second_user = "Reply with the single word done.";
  auto first_response = RunTurnResponse(session, first_user, "", /*disable_tools=*/true);
  ASSERT_TRUE(session.RemoveToolDefinition("lookup"));

  const auto after_removal = RunTurn(session, second_user, "");

  ChatSession rebuilt_session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  Request rebuilt_request;
  rebuilt_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, first_user));
  rebuilt_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_ASSISTANT, GetAssistantText(first_response)));
  rebuilt_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, second_user));
  rebuilt_request.options.Add("max_output_tokens", "8");
  rebuilt_request.options.Add("temperature", "0");

  Response rebuilt_response;
  rebuilt_session.ProcessRequest(rebuilt_request, rebuilt_response);

  EXPECT_EQ(after_removal.prompt_tokens, rebuilt_response.usage.prompt_tokens)
      << "removing tools must rebuild with the same prompt a fresh session sees";
}

TEST_F(ChatSessionTest, AutoModeGeneratedToolCallInvalidatesTheCachedGenerator) {
  const ToolDefinition tool{"lookup", "Look up a value",
                            R"({"type":"object","properties":{"key":{"type":"string"}},"required":["key"]})"};
  const std::string first_user = "Call lookup with key alpha. Do not answer without calling the tool.";
  const std::string second_user = "Reply with the single word done without calling a tool.";

  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  session.AddToolDefinition(tool);

  Request first_request;
  first_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, first_user));
  first_request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, "auto");
  first_request.options.Add("max_output_tokens", "64");
  first_request.options.Add("temperature", "0");

  Response first_response;
  session.ProcessRequest(first_request, first_response);

  const auto generated_call = std::find_if(first_response.items.begin(), first_response.items.end(),
                                           [](const auto& item) {
                                             return item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL;
                                           });
  ASSERT_NE(generated_call, first_response.items.end()) << "the deterministic prompt must produce a tool call";

  Request second_request;
  second_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, second_user));
  second_request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, "auto");
  second_request.options.Add("max_output_tokens", "8");
  second_request.options.Add("temperature", "0");

  Response warm_response;
  session.ProcessRequest(second_request, warm_response);

  ChatSession rebuilt_session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  rebuilt_session.AddToolDefinition(tool);

  Request rebuilt_request;
  rebuilt_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, first_user));
  for (const auto& item : first_response.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      rebuilt_request.AddOwnedItem(std::make_unique<MessageItem>(static_cast<const MessageItem&>(*item)));
    } else if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
      rebuilt_request.AddOwnedItem(std::make_unique<ToolCallItem>(static_cast<const ToolCallItem&>(*item)));
    }
  }
  rebuilt_request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, second_user));
  rebuilt_request.options.Add(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE, "auto");
  rebuilt_request.options.Add("max_output_tokens", "8");
  rebuilt_request.options.Add("temperature", "0");

  Response rebuilt_response;
  rebuilt_session.ProcessRequest(rebuilt_request, rebuilt_response);

  EXPECT_EQ(warm_response.usage.prompt_tokens, rebuilt_response.usage.prompt_tokens)
      << "a generated auto-mode call must force the next turn to rebuild from structured history";
}

// ===========================================================================
// Turns that carry no message of their own.
// ===========================================================================

TEST_F(ChatSessionTest, InstructionsAloneOnANewConversationGenerate) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  const auto usage = RunTurn(session, "", "Reply with the single word: ok");

  EXPECT_GT(usage.prompt_tokens, 0) << "the system prefix is the prompt";
  ASSERT_EQ(session.MessageCount(), 1u) << "only the assistant turn is recorded";
  EXPECT_EQ(session.Transcript().Messages()[0].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
}

TEST_F(ChatSessionTest, AnEmptyInputContinuesAWarmConversation) {
  // The warm half of the previous_response_id-with-empty-input case. The cached generator cannot append an empty
  // message list, so the turn rebuilds from committed history and produces the next assistant turn — which is what
  // the cold path does with the same request.
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  RunTurn(session, "Name a colour. Answer with one word.", "");
  ASSERT_EQ(session.MessageCount(), 2u);

  const auto continued = RunTurn(session, "", "");

  EXPECT_GT(continued.prompt_tokens, 0);
  // No input message was added, so the turn contributes exactly one assistant message.
  ASSERT_EQ(session.MessageCount(), 3u);
  EXPECT_EQ(session.Transcript().Messages()[2].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(session.Transcript().TurnCount(), 2u);
}

TEST_F(ChatSessionTest, ARequestWithNothingAtAllIsAClientError) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  Response response;

  try {
    session.ProcessRequest(request, response);
    FAIL() << "expected a request with no content, no media, no instructions and no history to be rejected";
  } catch (const fl::Exception& ex) {
    // A caller mistake, so it must map to HTTP 400 — never a 500.
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("nothing to generate from"), std::string::npos) << ex.what();
  }
}

TEST_F(ChatSessionTest, AnEmptyToolResultForAnUnknownCallReportsTheCorrelationError) {
  // Precedence: an empty tool result carries no content, but "unknown call id" is the real problem and must not be
  // reported as an empty request.
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  request.AddOwnedItem(std::make_unique<ToolResultItem>("call_missing", ""));

  Response response;
  try {
    session.ProcessRequest(request, response);
    FAIL() << "expected the unknown tool call id to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("unknown tool call id"), std::string::npos) << ex.what();
  }
}
