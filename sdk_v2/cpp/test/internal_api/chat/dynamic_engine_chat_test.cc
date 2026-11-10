// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/onnx_chat_engine.h"
#include "inferencing/generative/chat/onnx_engine_chat_stream.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/qwen_xml_tool_call_decoder.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"
#include "inferencing/model_load_manager.h"
#include "c_api_types.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "items/text_item.h"
#include "model.h"
#include "telemetry/telemetry_logger.h"

#include <gtest/gtest.h>
#include <ort_genai.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace fl {
namespace {

constexpr const char* kDynamicEngineModelId = "tiny-paged-attention";

std::string WithoutOptionalJsonWhitespace(std::string grammar) {
  // The untrained fixture otherwise prefers whitespace indefinitely. The separate whitespace test forces that branch.
  const std::string rule = "tool_json_ws: TOOL_JSON_WS?";
  grammar.replace(grammar.find(rule), rule.size(), "tool_json_ws:");
  return grammar;
}

TEST(OnnxEngineChatStreamDecisionTest, PreservesEveryOgaFinishCauseForRawFinalization) {
  using onnx_engine_chat_stream_internal::MapTerminationCause;

  EXPECT_EQ(MapTerminationCause(OgaFinishReason_Eos), BackendTerminationCause::kNaturalEnd);
  EXPECT_EQ(MapTerminationCause(OgaFinishReason_StopString), BackendTerminationCause::kStopSequence);
  EXPECT_EQ(MapTerminationCause(OgaFinishReason_MaxGeneratedTokens),
            BackendTerminationCause::kOutputTokenLimit);
  EXPECT_EQ(MapTerminationCause(OgaFinishReason_MaxSessionTokens),
            BackendTerminationCause::kSessionTokenLimit);
  EXPECT_EQ(MapTerminationCause(OgaFinishReason_Cancelled),
            BackendTerminationCause::kCancellation);
  EXPECT_EQ(MapTerminationCause(OgaFinishReason_Failed), BackendTerminationCause::kFailure);
  EXPECT_EQ(MapTerminationCause(9999), std::nullopt);
}

TEST(OnnxEngineChatStreamDecisionTest, MapsCachedPromptTokensIntoTurnUsage) {
  const OnnxChatEngine::TurnResult result{
      /*prompt_tokens=*/12,
      /*generated_tokens=*/4,
      /*cached_prompt_tokens=*/8,
      /*finish_reason=*/OgaFinishReason_Eos,
  };

  const auto usage = onnx_engine_chat_stream_internal::BuildTurnUsage(12, result);

  EXPECT_EQ(usage.prompt_tokens, 12);
  EXPECT_EQ(usage.generated_tokens, 4);
  EXPECT_EQ(usage.cached_prompt_tokens, 8);
  EXPECT_EQ(usage.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
  EXPECT_EQ(usage.termination_cause, BackendTerminationCause::kNaturalEnd);
}

std::unique_ptr<Item> UserMessage(std::string text) {
  return std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(text));
}

std::string AssistantText(const Response& response) {
  for (const auto& item : response.items) {
    if (item->type != FOUNDRY_LOCAL_ITEM_MESSAGE) {
      continue;
    }

    const auto& message = static_cast<const MessageItem&>(*item);
    if (message.role == FOUNDRY_LOCAL_ROLE_ASSISTANT) {
      return message.GetSimpleText();
    }
  }

  return {};
}

Request MakeRequest(std::string prompt, int max_output_tokens = 32) {
  Request request;
  request.AddOwnedItem(UserMessage(std::move(prompt)));
  request.options.Add("max_output_tokens", std::to_string(max_output_tokens));
  request.options.Add("temperature", "0");
  return request;
}

std::vector<int32_t> EncodeUserPrompt(std::string prompt, GenAIModelInstance& model) {
  std::vector<TranscriptMessage> messages;
  messages.emplace_back(FOUNDRY_LOCAL_ROLE_USER, std::move(prompt));
  auto sequences = EncodePrompt(BuildChatPrompt(messages, model), model);
  const auto count = sequences->SequenceCount(0);
  const auto* data = sequences->SequenceData(0);
  return {data, data + count};
}

std::vector<int32_t> EncodeLiteral(std::string text, GenAIModelInstance& model) {
  auto sequences = EncodePrompt(text, model);
  const auto count = sequences->SequenceCount(0);
  const auto* data = sequences->SequenceData(0);
  return {data, data + count};
}

std::vector<int32_t> EncodeMessages(const std::vector<TranscriptMessage>& messages, GenAIModelInstance& model) {
  auto sequences = EncodePrompt(BuildChatPrompt(messages, model), model);
  const auto count = sequences->SequenceCount(0);
  const auto* data = sequences->SequenceData(0);
  return {data, data + count};
}

std::vector<int32_t> ReferenceTokens(std::span<const int32_t> prompt, int output_count) {
  int64_t total = std::accumulate(prompt.begin(), prompt.end(), int64_t{0});
  int64_t length = static_cast<int64_t>(prompt.size());
  std::vector<int32_t> output;
  for (int i = 0; i < output_count; ++i) {
    const auto token = static_cast<int32_t>(3 + (total + 7 * length) % 95);
    output.push_back(token);
    total += token;
    ++length;
  }

  return output;
}

std::string ReferenceText(std::span<const int32_t> prompt, int output_count = 32) {
  std::string output;
  for (const auto token : ReferenceTokens(prompt, output_count)) {
    output += static_cast<char>(token - 3 + 32);
  }

  return output;
}

std::vector<int32_t> FinishConversation(OnnxChatEngine& engine,
                                        const std::shared_ptr<OnnxChatEngine::Conversation>& conversation) {
  std::vector<int32_t> tokens;
  while (const auto token = engine.WaitForToken(conversation)) {
    tokens.push_back(*token);
  }

  return tokens;
}

struct StreamTurnOutput {
  std::string text;
  ChatTurnUsage usage;
};

StreamTurnOutput FinishStream(OnnxEngineChatStream& stream) {
  StreamTurnOutput output;
  while (!stream.IsDone()) {
    stream.GenerateNextToken();
    output.text += stream.Decode();
  }

  const auto usage = stream.GetTurnUsage();
  if (!usage.has_value()) {
    throw std::runtime_error("Engine stream did not report final turn usage");
  }
  output.usage = *usage;
  return output;
}

class DynamicEngineChatTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    const auto model_path = test::GetTestDataPath(kDynamicEngineModelId);
    for (const auto* file : {"genai_config.json", "decoder.onnx", "tokenizer.json", "tokenizer_config.json"}) {
      ASSERT_TRUE(std::filesystem::is_regular_file(model_path / file)) << "Missing Engine fixture: " << model_path / file;
    }

    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);

    const auto result = load_manager_->LoadModel(model_path.string(), kDynamicEngineModelId);
    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess);
    model_ = result.model;
    ASSERT_EQ(model_->GetGenAIConfig().GetChatBackendKind(), ChatBackendKind::kEngine);
    ASSERT_NE(model_->GetChatEngine(), nullptr);
    ASSERT_EQ(model_->GetChatEngine()->MaxRequestLength(), 1024u);
  }

  static void TearDownTestSuite() {
    if (load_manager_ && model_) {
      load_manager_->UnloadModel(kDynamicEngineModelId);
    }

    model_ = nullptr;
    load_manager_.reset();
    ep_detector_.reset();
    logger_.reset();
  }

  static GenAIModelInstance& ModelInstance() { return *model_; }

  static const Model& CatalogModel() {
    static test::FakeServiceBindings services;
    static Model model = [] {
      ModelInfo info;
      info.task = "chat-completion";
      return Model::FromModelInfo(std::move(info), "", services.download_manager, services.model_load_manager);
    }();
    return model;
  }

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  TelemetryLogger telemetry_{"dynamic-engine-test", test::NullLog()};
};

// The tiny tokenizer has single-character markers; neither marker occurs in the grammar's required XML literals.
TEST_F(DynamicEngineChatTest, QwenRegionGuidanceConstrainsNativeBodyAndRestoresFreeText) {
  const auto opening = EncodeLiteral("~", ModelInstance());
  const auto closing = EncodeLiteral("!", ModelInstance());
  ASSERT_EQ(opening.size(), 1u);
  ASSERT_EQ(closing.size(), 1u);

  ToolCallContext context;
  context.tools_json =
      R"([{"type":"function","function":{"name":"grep","parameters":{"type":"object","properties":{}}}}])";
  context.tool_kinds = {{"grep", ToolKind::kFunction}};
  context.tool_body_grammar = BuildQwenXmlToolBodyGrammar(context.tools_json, context.tool_kinds);
  ASSERT_TRUE(context.tool_body_grammar.has_value());
  context.tool_call_start_token_id = opening.front();
  context.tool_call_end_token_id = closing.front();

  const auto prompt = EncodeUserPrompt("Try it.", ModelInstance());
  const auto baseline = ReferenceTokens(prompt, 100);
  const auto first_open = std::ranges::find(baseline, opening.front());
  ASSERT_NE(first_open, baseline.end());
  const auto free_count = static_cast<size_t>(first_open - baseline.begin());
  ASSERT_GT(free_count, 0u);

  SearchOptions options;
  options.max_output_tokens = 100;
  options.do_sample = false;
  OnnxChatEngine engine(ModelInstance());
  auto conversation = engine.CreateConversation(options, context, static_cast<int>(prompt.size()));
  engine.BeginTurn(conversation, prompt, options, context, false);
  const auto output = FinishConversation(engine, conversation);

  const auto body = EncodeLiteral("\n<function=grep>\n</function>\n", ModelInstance());
  ASSERT_GT(output.size(), free_count + body.size() + 1);
  EXPECT_TRUE(std::equal(output.begin(), output.begin() + free_count + 1, baseline.begin()));
  EXPECT_NE(baseline[free_count + 1], body.front());
  EXPECT_TRUE(std::equal(body.begin(), body.end(), output.begin() + free_count + 1));
  const auto closing_index = free_count + body.size() + 1;
  EXPECT_EQ(output[closing_index], closing.front());

  const auto resident = engine.ResidentTokens(conversation);
  std::vector<int32_t> history(prompt);
  history.insert(history.end(), output.begin(), output.begin() + closing_index + 1);
  EXPECT_EQ(output[closing_index + 1], ReferenceTokens(history, 1).front());

  const auto next_prompt = EncodeUserPrompt("Next turn.", ModelInstance());
  history = resident;
  history.insert(history.end(), next_prompt.begin(), next_prompt.end());
  options.max_output_tokens = 5;
  engine.BeginTurn(conversation, next_prompt, options, context, false);
  const auto next_output = FinishConversation(engine, conversation);
  ASSERT_EQ(next_output.size(), 5u);
  EXPECT_EQ(next_output, ReferenceTokens(history, 5));
  engine.Close(conversation);
}

TEST_F(DynamicEngineChatTest, QwenRegionGuidanceConstrainsRequiredParameterName) {
  const auto opening = EncodeLiteral("~", ModelInstance());
  const auto closing = EncodeLiteral("!", ModelInstance());
  ASSERT_EQ(opening.size(), 1u);
  ASSERT_EQ(closing.size(), 1u);

  ToolCallContext context;
  context.tools_json =
      R"([{"type":"function","function":{"name":"grep","parameters":{"type":"object",)"
      R"("properties":{"paths":{"type":"string"}},"required":["paths"]}}}])";
  context.tool_kinds = {{"grep", ToolKind::kFunction}};
  context.tool_body_grammar = BuildQwenXmlToolBodyGrammar(context.tools_json, context.tool_kinds);
  ASSERT_TRUE(context.tool_body_grammar.has_value());
  context.tool_call_start_token_id = opening.front();
  context.tool_call_end_token_id = closing.front();

  const auto prompt = EncodeUserPrompt("Try it.", ModelInstance());
  const auto baseline = ReferenceTokens(prompt, 100);
  const auto first_open = std::ranges::find(baseline, opening.front());
  ASSERT_NE(first_open, baseline.end());
  const auto free_count = static_cast<size_t>(first_open - baseline.begin());
  ASSERT_GT(free_count, 0u);
  const auto header = EncodeLiteral("\n<function=grep>\n<parameter=paths>\n", ModelInstance());

  SearchOptions options;
  options.max_output_tokens = static_cast<int>(free_count + header.size() + 2);
  options.do_sample = false;
  OnnxChatEngine engine(ModelInstance());
  auto conversation = engine.CreateConversation(options, context, static_cast<int>(prompt.size()));
  engine.BeginTurn(conversation, prompt, options, context, false);
  const auto output = FinishConversation(engine, conversation);

  ASSERT_GE(output.size(), free_count + header.size() + 1);
  EXPECT_TRUE(std::equal(output.begin(), output.begin() + free_count + 1, baseline.begin()));
  EXPECT_TRUE(std::equal(header.begin(), header.end(), output.begin() + free_count + 1));
  EXPECT_NE(output[free_count + header.size()], closing.front());
  engine.Close(conversation);
}

TEST_F(DynamicEngineChatTest, CanonicalToolRegionPreservesFreeTextAndConstrainsTheEnvelope) {
  const auto opening = EncodeLiteral("~", ModelInstance());
  const auto closing = EncodeLiteral("!", ModelInstance());
  ASSERT_EQ(opening.size(), 1u);
  ASSERT_EQ(closing.size(), 1u);

  ToolCallContext context;
  context.tools_json = R"([{"type":"function","function":{"name":"echo","parameters":{"type":"object"}}}])";
  context.tool_plan = ToolCallPlan::Create(context.tools_json, false);
  context.tool_body_grammar = WithoutOptionalJsonWhitespace(context.tool_plan->Grammar());
  context.tool_call_start_token_id = opening.front();
  context.tool_call_end_token_id = closing.front();

  const auto prompt = EncodeUserPrompt("Try it.", ModelInstance());
  const auto baseline = ReferenceTokens(prompt, 256);
  const auto first_open = std::ranges::find(baseline, opening.front());
  ASSERT_NE(first_open, baseline.end());
  const auto free_count = static_cast<size_t>(first_open - baseline.begin());

  SearchOptions options;
  options.max_output_tokens = 256;
  options.do_sample = false;
  OnnxChatEngine engine(ModelInstance());
  auto conversation = engine.CreateConversation(options, context, static_cast<int>(prompt.size()));
  engine.BeginTurn(conversation, prompt, options, context, false);
  const auto output = FinishConversation(engine, conversation);
  ASSERT_GT(output.size(), free_count + 2);
  EXPECT_TRUE(std::equal(output.begin(), output.begin() + free_count + 1, baseline.begin()));

  const auto body_begin = output.begin() + free_count + 1;
  auto decoder = ModelInstance().GetPreprocessor().CreateTokenizerStream();
  std::string body;
  for (auto token = body_begin; token != output.end(); ++token) {
    body += decoder->Decode(*token);
  }

  // This untrained decoder need not finish its argument object. Semantic fidelity uses the real-model gate.
  EXPECT_TRUE(body.starts_with(R"([{"name":"echo","parameters":{)")) << body;
  engine.Close(conversation);
}

TEST_F(DynamicEngineChatTest, CanonicalToolGrammarAcceptsWhitespaceAtStructuralBoundaries) {
  const auto plan = ToolCallPlan::Create(
      R"([{"type":"function","function":{"name":"echo","parameters":{"type":"object"}}}])", false);
  ToolCallContext context;
  context.guidance_type = "lark_grammar";
  context.guidance_data = plan->Grammar();
  const auto replace_rule = [&](const std::string& name, const std::string& expression) {
    const auto begin = context.guidance_data.find("\n" + name + ":");
    if (begin == std::string::npos) {
      return false;
    }

    const auto end = context.guidance_data.find('\n', begin + 1);
    context.guidance_data.replace(begin + 1, end - begin - 1, name + ": " + expression);
    return true;
  };

  // Narrow legal alternatives so this untrained decoder exercises whitespace rather than extending strings forever.
  const std::string whitespace_rule = R"(" \n")";
  const std::string string_rule = R"("\"x\"")";
  const std::string object_rule = R"("{" tool_json_ws tool_json_member tool_json_comma )"
                                  R"("\"other\"" tool_json_colon TOOL_JSON_STRING tool_json_ws "}")";
  ASSERT_TRUE(replace_rule("tool_json_ws", whitespace_rule));
  ASSERT_TRUE(replace_rule("tool_json_value", "TOOL_JSON_STRING"));
  ASSERT_TRUE(replace_rule("TOOL_JSON_STRING", string_rule));
  ASSERT_TRUE(replace_rule("tool_json_object", object_rule));
  const auto prompt = EncodeUserPrompt("Try it.", ModelInstance());
  SearchOptions options;
  options.max_output_tokens = 256;
  options.do_sample = false;
  OnnxChatEngine engine(ModelInstance());
  auto conversation = engine.CreateConversation(options, context, static_cast<int>(prompt.size()));
  engine.BeginTurn(conversation, prompt, options, context, false);
  const auto output = FinishConversation(engine, conversation);
  auto decoder = ModelInstance().GetPreprocessor().CreateTokenizerStream();
  std::string body;
  for (const auto token : output) {
    body += decoder->Decode(token);
  }

  EXPECT_TRUE(body.starts_with(" \n[ \n{ \n\"name\" \n: \n\"echo\" \n, \n\"parameters\" \n: \n{")) << body;
  EXPECT_NE(body.find("\"x\" \n, \n\"other\" \n: \n\"x\" \n}"), std::string::npos) << body;
  const auto calls = plan->ParseCalls(body);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(nlohmann::json::parse(calls.front().arguments), (nlohmann::json{{"x", "x"}, {"other", "x"}}));
  engine.Close(conversation);
}

TEST_F(DynamicEngineChatTest, InvalidRegionGuidanceDoesNotCommitPromptOrDestroyResidentHistory) {
  OnnxChatEngine engine(ModelInstance());
  SearchOptions options;
  options.max_output_tokens = 5;
  options.do_sample = false;
  ToolCallContext context;
  const auto prompt = EncodeUserPrompt("Keep this history.", ModelInstance());
  auto conversation = engine.CreateConversation(options, context, static_cast<int>(prompt.size()));
  engine.BeginTurn(conversation, prompt, options, context, false);
  (void)FinishConversation(engine, conversation);
  const auto resident = engine.ResidentTokens(conversation);

  auto invalid_context = context;
  invalid_context.tool_call_start_token_id = EncodeLiteral("~", ModelInstance()).front();
  invalid_context.tool_call_end_token_id = EncodeLiteral("!", ModelInstance()).front();
  invalid_context.tool_body_grammar = "this is not a Lark grammar";
  const auto next_prompt = EncodeUserPrompt("Continue.", ModelInstance());
  EXPECT_THROW(engine.BeginTurn(conversation, next_prompt, options, invalid_context, false), std::runtime_error);
  EXPECT_EQ(engine.ResidentTokens(conversation), resident);

  engine.BeginTurn(conversation, next_prompt, options, context, false);
  auto expected = resident;
  expected.insert(expected.end(), next_prompt.begin(), next_prompt.end());
  EXPECT_EQ(FinishConversation(engine, conversation), ReferenceTokens(expected, 5));
  engine.Close(conversation);
}

TEST_F(DynamicEngineChatTest, CanonicalWholeTurnSeparatesNonSpecialControlTokensFromReasoningAndEnvelope) {
  ToolCallContext context;
  context.tools_json = R"([{"type":"function","function":{"name":"echo","parameters":{"type":"object"}}}])";
  context.tool_plan = ToolCallPlan::Create(context.tools_json, false);
  context.text_output = false;
  context.tool_output = true;
  context.supports_reasoning = true;
  context.tool_call_start = "~";
  context.tool_call_end = "!";
  context.reasoning_start = "(";
  context.reasoning_end = "O";
  context.tool_call_start_token_id = EncodeLiteral("~", ModelInstance()).front();
  context.tool_call_end_token_id = EncodeLiteral("!", ModelInstance()).front();
  context.reasoning_start_token_id = EncodeLiteral("(", ModelInstance()).front();
  context.reasoning_end_token_id = EncodeLiteral("O", ModelInstance()).front();
  context.eos_token_ids = ModelInstance().GetPreprocessor().GetEosTokenIds();
  context.guidance_type = "lark_grammar";
  context.guidance_data = WithoutOptionalJsonWhitespace(BuildLarkGrammar(context));
  const auto prompt = EncodeUserPrompt("Try it.", ModelInstance());
  SearchOptions options;
  options.max_output_tokens = 256;
  options.do_sample = false;
  OnnxChatEngine engine(ModelInstance());
  auto conversation = engine.CreateConversation(options, context, static_cast<int>(prompt.size()));
  engine.BeginTurn(conversation, prompt, options, context, false);
  const auto output = FinishConversation(engine, conversation);

  ASSERT_FALSE(output.empty());
  EXPECT_EQ(output.front(), *context.reasoning_start_token_id);
  const auto reasoning_end = std::ranges::find(output, *context.reasoning_end_token_id);
  const auto opening = std::ranges::find(output, *context.tool_call_start_token_id);
  ASSERT_NE(reasoning_end, output.end()) << ::testing::PrintToString(output);
  ASSERT_NE(opening, output.end());
  EXPECT_LT(reasoning_end, opening);
  const auto angle_bracket = EncodeLiteral("<", ModelInstance()).front();
  EXPECT_NE(std::find(output.begin(), reasoning_end, angle_bracket), reasoning_end);
  auto decoder = ModelInstance().GetPreprocessor().CreateTokenizerStream();
  std::string body;
  for (auto token = opening + 1; token != output.end(); ++token) {
    body += decoder->Decode(*token);
  }
  EXPECT_TRUE(body.starts_with(R"([{"name":"echo","parameters":{)")) << body;
  engine.Close(conversation);
}

TEST_F(DynamicEngineChatTest, CanonicalAutomaticWholeTurnCannotTreatAnOpeningIdAsOrdinaryText) {
  const auto prompt = EncodeUserPrompt("Try it.", ModelInstance());
  const auto baseline = ReferenceTokens(prompt, 4);
  ASSERT_FALSE(baseline.empty());
  auto decoder = ModelInstance().GetPreprocessor().CreateTokenizerStream();
  ToolCallContext context;
  context.tools_json = R"([{"type":"function","function":{"name":"echo","parameters":{"type":"object"}}}])";
  context.tool_plan = ToolCallPlan::Create(context.tools_json, false);
  context.tool_output = true;
  context.tool_call_start_token_id = baseline.front();
  context.tool_call_start = decoder->Decode(baseline.front());
  context.tool_call_end = context.tool_call_start == "!" ? "~" : "!";
  context.tool_call_end_token_id = EncodeLiteral(context.tool_call_end, ModelInstance()).front();
  context.eos_token_ids = ModelInstance().GetPreprocessor().GetEosTokenIds();
  context.guidance_type = "lark_grammar";
  context.guidance_data = WithoutOptionalJsonWhitespace(BuildLarkGrammar(context));
  SearchOptions options;
  options.max_output_tokens = 256;
  options.do_sample = false;
  OnnxChatEngine engine(ModelInstance());
  auto conversation = engine.CreateConversation(options, context, static_cast<int>(prompt.size()));
  engine.BeginTurn(conversation, prompt, options, context, false);
  const auto output = FinishConversation(engine, conversation);
  ASSERT_GT(output.size(), 2u);
  EXPECT_EQ(output.front(), *context.tool_call_start_token_id);
  decoder = ModelInstance().GetPreprocessor().CreateTokenizerStream();
  std::string body;
  for (auto token = output.begin() + 1; token != output.end(); ++token) {
    body += decoder->Decode(*token);
  }
  EXPECT_TRUE(body.starts_with(R"([{"name":"echo","parameters":{)")) << body;
  engine.Close(conversation);
}

TEST_F(DynamicEngineChatTest, AutomaticWholeTurnRequiresAnExplicitTextOrToolModeBeforeAnyBody) {
  const auto prompt = EncodeUserPrompt("Choose a response.", ModelInstance());
  const auto baseline = ReferenceTokens(prompt, 1);
  ASSERT_EQ(baseline.size(), 1u);
  ToolCallContext context;
  context.tools_json = R"([{"type":"function","function":{"name":"echo","parameters":{"type":"object"}}}])";
  context.tool_plan = ToolCallPlan::Create(context.tools_json, false);
  context.tool_output = true;
  context.tool_call_start = "~";
  context.tool_call_end = "!";
  context.tool_call_start_token_id = EncodeLiteral("~", ModelInstance()).front();
  context.tool_call_end_token_id = EncodeLiteral("!", ModelInstance()).front();
  context.eos_token_ids = ModelInstance().GetPreprocessor().GetEosTokenIds();
  const auto message_prefix = EncodeLiteral("M", ModelInstance());
  ASSERT_EQ(message_prefix.size(), 1u);
  ASSERT_NE(baseline.front(), message_prefix.front());
  ASSERT_NE(baseline.front(), *context.tool_call_start_token_id);
  SearchOptions options;
  options.max_output_tokens = 1;
  options.do_sample = false;
  OnnxChatEngine engine(ModelInstance());
  auto conversation = engine.CreateConversation(options, context, static_cast<int>(prompt.size()));
  engine.BeginTurn(conversation, prompt, options, context, false);
  const auto output = FinishConversation(engine, conversation);
  ASSERT_FALSE(output.empty());
  EXPECT_TRUE(output.front() == message_prefix.front() || output.front() == *context.tool_call_start_token_id);
  engine.Close(conversation);
}

TEST_F(DynamicEngineChatTest, NativeCAbiPreflightCapturesAndExecutesExactlyOnce) {
  const auto* api = FoundryLocalGetApi(2);
  ASSERT_NE(api, nullptr);
  const auto* inference_api = api->GetInferenceApi();

  auto session = std::make_unique<ChatSession>(CatalogModel(), ModelInstance(), *logger_, telemetry_);
  auto request = std::make_unique<Request>(MakeRequest("Count this exact prompt.", 17));
  ASSERT_TRUE(request->TryBegin());
  ASSERT_TRUE(request->Cancel());
  EXPECT_FALSE(request->CaptureChatSnapshot().IsCancellationRequested());
  const auto expected_prompt_tokens =
      static_cast<int64_t>(EncodeUserPrompt("Count this exact prompt.", ModelInstance()).size());
  auto session_backend = fl::api::CreateEmbeddedSession(std::move(session));

  flRequestPreflight* preflight = nullptr;
  ASSERT_EQ(inference_api->Session_CreateRequestPreflight(
                AsHandle<flSession>(session_backend.get()), AsHandle<flRequest>(request.get()), &preflight),
            nullptr);
  ASSERT_NE(preflight, nullptr);

  request.reset();
  session_backend.reset();

  flRequestPreflightResult result{};
  for (const uint32_t unsupported : {0u, FOUNDRY_LOCAL_REQUEST_PREFLIGHT_RESULT_VERSION + 1u}) {
    result.version = unsupported;
    auto* invalid = inference_api->RequestPreflight_Execute(preflight, &result);
    ASSERT_NE(invalid, nullptr);
    EXPECT_EQ(api->Status_GetErrorCode(invalid), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    api->Status_Release(invalid);
  }

  result.version = 2;
  ASSERT_EQ(inference_api->RequestPreflight_Execute(preflight, &result), nullptr);
  EXPECT_EQ(result.version, 2u);
  EXPECT_EQ(result.prompt_tokens, expected_prompt_tokens);
  EXPECT_EQ(result.output_reserve_tokens, 17);
  EXPECT_EQ(result.required_tokens, result.prompt_tokens + result.output_reserve_tokens);
  EXPECT_EQ(result.context_limit_tokens, 1024);
  EXPECT_TRUE(result.fits);
  EXPECT_EQ(result.deficit_tokens, 0);

  auto* repeated = inference_api->RequestPreflight_Execute(preflight, &result);
  ASSERT_NE(repeated, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(repeated), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  api->Status_Release(repeated);

  inference_api->RequestPreflight_Release(preflight);
}

TEST_F(DynamicEngineChatTest, PreflightUsesMergedSessionTemplateOptions) {
  ChatSession session(CatalogModel(), ModelInstance(), *logger_, telemetry_);
  KeyValuePairs defaults;
  defaults.Add("chat_template_kwargs", "[]");
  session.SetSessionOptions(defaults);

  auto ordinary = MakeRequest("Hello.");
  Request json_request;
  json_request.AddOwnedItem(std::make_unique<TextItem>(
      R"({"model":"tiny-paged-attention","messages":[{"role":"user","content":"Hello."}]})",
      FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  for (auto* request : {&ordinary, &json_request}) {
    EXPECT_THROW((void)session.CreateRequestPreflight(*request)->Execute(), fl::Exception);
    request->options.Add("chat_template_kwargs", "{}");
    EXPECT_TRUE(session.CreateRequestPreflight(*request)->Execute().fits);
  }
}

TEST_F(DynamicEngineChatTest, RetainedContinuationReportsFreshPromptUsageParity) {
  ChatSession session(CatalogModel(), ModelInstance(), *logger_, telemetry_);

  constexpr const char* kFirstPrompt = "Remember the word sapphire. Reply OK.";
  constexpr const char* kSecondPrompt = "What word did I ask you to remember?";
  auto first = MakeRequest(kFirstPrompt);
  Response first_response;
  session.ProcessRequest(first, first_response);
  EXPECT_EQ(AssistantText(first_response), ReferenceText(EncodeUserPrompt(kFirstPrompt, ModelInstance())));
  EXPECT_EQ(first_response.usage.completion_tokens, 32);
  EXPECT_EQ(first_response.usage.prompt_tokens,
            static_cast<int>(EncodeMessages({{FOUNDRY_LOCAL_ROLE_USER, kFirstPrompt}}, ModelInstance()).size()));
  EXPECT_EQ(first_response.usage.total_tokens,
            first_response.usage.prompt_tokens + first_response.usage.completion_tokens);

  auto full_history = session.Transcript().Messages();
  full_history.emplace_back(FOUNDRY_LOCAL_ROLE_USER, kSecondPrompt);
  const auto expected_second_prompt_tokens = EncodeMessages(full_history, ModelInstance()).size();

  auto second = MakeRequest(kSecondPrompt);
  const auto second_budget = session.CreateRequestPreflight(second)->Execute();
  EXPECT_EQ(second_budget.prompt_tokens, expected_second_prompt_tokens);
  EXPECT_EQ(second_budget.output_reserve_tokens, 32);
  EXPECT_EQ(session.TurnCount(), 1u);

  Response second_response;
  session.ProcessRequest(second, second_response);

  EXPECT_EQ(AssistantText(second_response), ReferenceText(EncodeMessages(full_history, ModelInstance())));
  EXPECT_EQ(session.TurnCount(), 2u);
  EXPECT_EQ(second_response.usage.prompt_tokens, expected_second_prompt_tokens)
      << "resident suffix admission and fresh replay must report the same complete logical prompt";
  EXPECT_EQ(second_response.usage.completion_tokens, 32);
  EXPECT_EQ(second_response.usage.total_tokens,
            second_response.usage.prompt_tokens + second_response.usage.completion_tokens);

  ChatSession fresh_session(CatalogModel(), ModelInstance(), *logger_, telemetry_);
  Request fresh_request;
  fresh_request.AddOwnedItem(UserMessage(kFirstPrompt));
  fresh_request.AddOwnedItem(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, AssistantText(first_response)));
  fresh_request.AddOwnedItem(UserMessage(kSecondPrompt));
  fresh_request.options.Add("max_output_tokens", "32");
  fresh_request.options.Add("temperature", "0");
  Response fresh_response;
  fresh_session.ProcessRequest(fresh_request, fresh_response);

  EXPECT_EQ(AssistantText(second_response), AssistantText(fresh_response));
  EXPECT_EQ(second_response.usage.prompt_tokens, fresh_response.usage.prompt_tokens);
  EXPECT_EQ(second_response.usage.completion_tokens, fresh_response.usage.completion_tokens);
  EXPECT_EQ(fresh_response.usage.total_tokens,
            fresh_response.usage.prompt_tokens + fresh_response.usage.completion_tokens);
}

TEST_F(DynamicEngineChatTest, RetainedSuffixReadsPriorPagedAttentionState) {
  SearchOptions options;
  options.max_output_tokens = 5;
  options.do_sample = false;
  ToolCallContext tool_context;
  std::vector<TranscriptMessage> history = {{FOUNDRY_LOCAL_ROLE_USER, "A history crossing several pages."}};
  auto warm = OnnxEngineChatStream::Create(history, options, ModelInstance(), tool_context);
  auto output = FinishStream(*warm);
  ASSERT_EQ(output.text, ReferenceText(EncodeMessages(history, ModelInstance()), 5));

  for (const auto* prompt : {"One.", "Two tokens?", "Another page boundary."}) {
    const auto resident_length = warm->TokenCount();
    history.emplace_back(FOUNDRY_LOCAL_ROLE_ASSISTANT, output.text);
    history.emplace_back(FOUNDRY_LOCAL_ROLE_USER, prompt);
    const auto full_prompt = EncodeMessages(history, ModelInstance());
    const auto submitted = warm->AppendMessages({history.back()}, history, ModelInstance(), tool_context, options);
    ASSERT_GT(submitted, 0);
    ASSERT_LT(submitted, static_cast<int>(full_prompt.size()));
    EXPECT_EQ(submitted, static_cast<int>(full_prompt.size()) - resident_length);
    output = FinishStream(*warm);
    EXPECT_EQ(output.text, ReferenceText(full_prompt, 5));
    EXPECT_EQ(output.usage.prompt_tokens, full_prompt.size());

    auto fresh = OnnxEngineChatStream::Create(history, options, ModelInstance(), tool_context);
    const auto replay = FinishStream(*fresh);
    EXPECT_EQ(output.text, replay.text);
    EXPECT_EQ(output.usage.prompt_tokens, replay.usage.prompt_tokens);
    EXPECT_EQ(output.usage.generated_tokens, replay.usage.generated_tokens);
  }
}

TEST_F(DynamicEngineChatTest, MismatchedResidentPromptIsReplacedAndMatchesFreshReplay) {
  SearchOptions first_options;
  first_options.max_output_tokens = 4;
  first_options.do_sample = false;
  ToolCallContext tool_context;

  std::vector<TranscriptMessage> first_messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Write a detailed explanation of why the sky is blue."}};
  auto warm = OnnxEngineChatStream::Create(first_messages, first_options, ModelInstance(), tool_context);
  const auto first = FinishStream(*warm);
  ASSERT_EQ(first.usage.finish_reason, FOUNDRY_LOCAL_FINISH_LENGTH);
  ASSERT_FALSE(first.text.empty());

  // Deliberately replay different history into the retained stream. This exercises the defensive replacement branch
  // without assuming generated token bytes survive decode/re-encode as an identical token sequence.
  std::vector<TranscriptMessage> full_history = {
      {FOUNDRY_LOCAL_ROLE_USER, "Give a concise explanation of why leaves are green."},
      {FOUNDRY_LOCAL_ROLE_ASSISTANT, first.text}};
  full_history.emplace_back(FOUNDRY_LOCAL_ROLE_USER, "Summarize that in one sentence.");
  const auto full_prompt = EncodeMessages(full_history, ModelInstance());

  SearchOptions second_options;
  second_options.max_output_tokens = 16;
  second_options.do_sample = false;
  const std::vector<TranscriptMessage> new_messages = {full_history.back()};
  const auto submitted =
      warm->AppendMessages(new_messages, full_history, ModelInstance(), tool_context, second_options);
  EXPECT_EQ(submitted, static_cast<int>(full_prompt.size()))
      << "a mismatched resident prompt must submit a complete replacement, not an unsafe suffix";
  const auto warm_second = FinishStream(*warm);

  auto fresh = OnnxEngineChatStream::Create(full_history, second_options, ModelInstance(), tool_context);
  const auto fresh_second = FinishStream(*fresh);

  EXPECT_EQ(warm->PromptTokenCount(), fresh->PromptTokenCount());
  EXPECT_EQ(warm_second.text, ReferenceText(full_prompt, 16));
  EXPECT_EQ(warm_second.text, fresh_second.text);
  EXPECT_EQ(warm_second.usage.finish_reason, fresh_second.usage.finish_reason);
  EXPECT_EQ(warm_second.usage.prompt_tokens, fresh_second.usage.prompt_tokens);
  EXPECT_EQ(warm_second.usage.generated_tokens, fresh_second.usage.generated_tokens);
}

TEST_F(DynamicEngineChatTest, RunsTwoConcurrentSessions) {
  const std::string first_prompt = "History A must remain isolated across pages.";
  const std::string second_prompt = "History B must remain isolated across pages.";
  const auto first_expected = ReferenceText(EncodeUserPrompt(first_prompt, ModelInstance()));
  const auto second_expected = ReferenceText(EncodeUserPrompt(second_prompt, ModelInstance()));
  ASSERT_NE(first_expected, second_expected);
  std::barrier start(3);
  auto run = [&](std::string prompt) {
    ChatSession session(CatalogModel(), ModelInstance(), *logger_, telemetry_);
    auto request = MakeRequest(std::move(prompt));
    Response response;
    start.arrive_and_wait();
    session.ProcessRequest(request, response);
    return AssistantText(response);
  };

  auto first = std::async(std::launch::async, run, first_prompt);
  auto second = std::async(std::launch::async, run, second_prompt);
  start.arrive_and_wait();

  EXPECT_EQ(first.get(), first_expected);
  EXPECT_EQ(second.get(), second_expected);
}

TEST_F(DynamicEngineChatTest, CapacityWaitSurvivesProgressBeyondItsStallTimeout) {
  OnnxChatEngine engine(ModelInstance(), 1ms);
  SearchOptions options;
  options.max_output_tokens = 512;
  options.temperature = 0.0f;
  ToolCallContext tool_context;
  const auto tokens = EncodeUserPrompt("Write a detailed history of mathematics.", ModelInstance());

  auto first = engine.CreateConversation(options, tool_context, static_cast<int>(tokens.size()));
  auto second = engine.CreateConversation(options, tool_context, static_cast<int>(tokens.size()));
  auto waiting = engine.CreateConversation(options, tool_context, static_cast<int>(tokens.size()));
  engine.BeginTurn(first, tokens, options, tool_context, false);
  engine.BeginTurn(second, tokens, options, tool_context, false);
  const auto queued_at = std::chrono::steady_clock::now();
  engine.BeginTurn(waiting, tokens, options, tool_context, false);

  auto wait_result = std::async(std::launch::async, [&]() {
    return FinishConversation(engine, waiting);
  });

  ASSERT_EQ(wait_result.wait_for(5s), std::future_status::ready)
      << "A queued request must run after healthy resident turns complete";
  EXPECT_EQ(wait_result.get(), ReferenceTokens(tokens, 512));
  EXPECT_GT(std::chrono::steady_clock::now() - queued_at, 1ms);
  EXPECT_EQ(FinishConversation(engine, first), ReferenceTokens(tokens, 512));
  EXPECT_EQ(FinishConversation(engine, second), ReferenceTokens(tokens, 512));

  engine.Close(first);
  engine.Close(second);
  engine.Close(waiting);
}

TEST_F(DynamicEngineChatTest, CancelsCapacityWaitingConversationWithoutInterruptingResidents) {
  OnnxChatEngine engine(ModelInstance());
  SearchOptions options;
  options.max_output_tokens = 512;
  options.temperature = 0.0f;
  ToolCallContext tool_context;
  const auto tokens = EncodeUserPrompt("Keep generating while another request waits.", ModelInstance());

  auto first = engine.CreateConversation(options, tool_context, static_cast<int>(tokens.size()));
  auto second = engine.CreateConversation(options, tool_context, static_cast<int>(tokens.size()));
  auto waiting = engine.CreateConversation(options, tool_context, static_cast<int>(tokens.size()));
  engine.BeginTurn(first, tokens, options, tool_context, false);
  engine.BeginTurn(second, tokens, options, tool_context, false);
  engine.BeginTurn(waiting, tokens, options, tool_context, false);
  engine.Cancel(waiting);

  EXPECT_FALSE(engine.WaitForToken(waiting).has_value());
  EXPECT_EQ(engine.GetTurnResult(waiting).finish_reason, OgaFinishReason_Cancelled);
  EXPECT_EQ(FinishConversation(engine, first), ReferenceTokens(tokens, 512));
  EXPECT_EQ(FinishConversation(engine, second), ReferenceTokens(tokens, 512));
  engine.Close(first);
  engine.Close(second);
  engine.Close(waiting);
}

TEST_F(DynamicEngineChatTest, EvictsOldestIdleConversation) {
  OnnxChatEngine engine(ModelInstance());
  SearchOptions options;
  options.max_output_tokens = 5;
  options.do_sample = false;
  ToolCallContext tool_context;
  const auto prompt = EncodeUserPrompt("Populate several pages.", ModelInstance());
  auto run = [&]() {
    auto conversation = engine.CreateConversation(options, tool_context, static_cast<int>(prompt.size()));
    engine.BeginTurn(conversation, prompt, options, tool_context, false);
    EXPECT_EQ(FinishConversation(engine, conversation), ReferenceTokens(prompt, 5));
    return conversation;
  };

  auto oldest = run();
  auto second = run();
  auto newest = run();
  EXPECT_THROW(engine.BeginTurn(oldest, prompt, options, tool_context, false), OnnxChatEngine::ConversationEvictedError);

  auto retained = engine.ResidentTokens(second);
  retained.insert(retained.end(), prompt.begin(), prompt.end());
  engine.BeginTurn(second, prompt, options, tool_context, false);
  EXPECT_EQ(FinishConversation(engine, second), ReferenceTokens(retained, 5));
  engine.Close(oldest);
  engine.Close(second);
  engine.Close(newest);
}

TEST_F(DynamicEngineChatTest, EvictedSessionReplaysCommittedHistory) {
  ChatSession oldest(CatalogModel(), ModelInstance(), *logger_, telemetry_);
  ChatSession second(CatalogModel(), ModelInstance(), *logger_, telemetry_);
  ChatSession newest(CatalogModel(), ModelInstance(), *logger_, telemetry_);
  for (auto* session : {&oldest, &second, &newest}) {
    auto request = MakeRequest("Retain this history across pages.");
    Response response;
    session->ProcessRequest(request, response);
    EXPECT_EQ(AssistantText(response), ReferenceText(EncodeUserPrompt("Retain this history across pages.", ModelInstance())));
  }

  auto history = oldest.Transcript().Messages();
  history.emplace_back(FOUNDRY_LOCAL_ROLE_USER, "Continue after eviction.");
  auto request = MakeRequest("Continue after eviction.");
  Response response;
  oldest.ProcessRequest(request, response);
  EXPECT_EQ(AssistantText(response), ReferenceText(EncodeMessages(history, ModelInstance())));
  EXPECT_EQ(response.usage.prompt_tokens, EncodeMessages(history, ModelInstance()).size());
  EXPECT_EQ(oldest.TurnCount(), 2u);
}

TEST_F(DynamicEngineChatTest, EvictsIdleConversationWhileAnotherTurnIsGenerating) {
  OnnxChatEngine engine(ModelInstance());
  SearchOptions short_options;
  short_options.max_output_tokens = 5;
  short_options.do_sample = false;
  auto long_options = short_options;
  long_options.max_output_tokens = 512;
  ToolCallContext tool_context;
  const auto prompt = EncodeUserPrompt("Keep these pages separate.", ModelInstance());
  const auto prompt_size = static_cast<int>(prompt.size());
  auto idle = engine.CreateConversation(short_options, tool_context, prompt_size);
  engine.BeginTurn(idle, prompt, short_options, tool_context, false);
  EXPECT_EQ(FinishConversation(engine, idle), ReferenceTokens(prompt, 5));

  auto active = engine.CreateConversation(long_options, tool_context, prompt_size);
  engine.BeginTurn(active, prompt, long_options, tool_context, false);
  ASSERT_TRUE(engine.WaitForToken(active).has_value());
  auto waiting = engine.CreateConversation(short_options, tool_context, prompt_size);
  engine.BeginTurn(waiting, prompt, short_options, tool_context, false);
  EXPECT_EQ(FinishConversation(engine, waiting), ReferenceTokens(prompt, 5));
  EXPECT_LT(engine.SequenceLength(active), prompt.size() + 512)
      << "an idle slot must be reclaimed without waiting for the unrelated active turn to finish";
  EXPECT_THROW(engine.BeginTurn(idle, prompt, short_options, tool_context, false),
               OnnxChatEngine::ConversationEvictedError);

  engine.Cancel(active);
  engine.Close(idle);
  engine.Close(active);
  engine.Close(waiting);
}

TEST_F(DynamicEngineChatTest, LateCancelAfterCompletedConversationRemovalIsNoOp) {
  OnnxChatEngine engine(ModelInstance());
  SearchOptions options;
  options.max_output_tokens = 32;
  options.temperature = 0.0f;
  ToolCallContext tool_context;
  const auto tokens = EncodeUserPrompt("Reply OK.", ModelInstance());

  auto completed = engine.CreateConversation(options, tool_context, static_cast<int>(tokens.size()));
  engine.BeginTurn(completed, tokens, options, tool_context, false);
  while (!engine.IsTurnFinished(completed)) {
    (void)engine.WaitForToken(completed);
  }
  const auto result = engine.GetTurnResult(completed);
  engine.Close(completed);

  engine.Cancel(completed);
  auto barrier = engine.CreateConversation(options, tool_context, static_cast<int>(tokens.size()));
  engine.Close(barrier);

  const auto result_after_cancel = engine.GetTurnResult(completed);
  EXPECT_EQ(result_after_cancel.prompt_tokens, result.prompt_tokens);
  EXPECT_EQ(result_after_cancel.generated_tokens, result.generated_tokens);
  EXPECT_EQ(result_after_cancel.finish_reason, result.finish_reason);
}

TEST_F(DynamicEngineChatTest, BackendCancellationIsNotAnApiErrorByItself) {
  SearchOptions options;
  options.max_output_tokens = 512;
  options.temperature = 0.0f;
  ToolCallContext tool_context;
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Write a long essay about mathematics."}};
  auto stream = OnnxEngineChatStream::Create(messages, options, ModelInstance(), tool_context);

  stream->GenerateNextToken();
  stream->Cancel();

  const auto usage = stream->GetTurnUsage();
  ASSERT_TRUE(usage.has_value());
  EXPECT_FALSE(usage->finish_reason.has_value());
}

TEST_F(DynamicEngineChatTest, CancellationRebuildsCommittedHistoryWithinBudget) {
  ChatSession session(CatalogModel(), ModelInstance(), *logger_, telemetry_);

  auto first = MakeRequest("Remember the word sapphire. Reply OK.");
  Response first_response;
  session.ProcessRequest(first, first_response);

  int streamed_tokens = 0;
  bool cancel_enabled = true;
  session.SetStreamingCallback([&](flStreamingCallbackData event, void*) {
    auto* queue = reinterpret_cast<ItemQueue*>(event.item_queue);
    (void)queue->TryPop();
    return cancel_enabled && ++streamed_tokens >= 3 ? 1 : 0;
  });

  auto canceled = MakeRequest("Write a long essay about mathematics.", 512);
  Response canceled_response;
  try {
    session.ProcessRequest(canceled, canceled_response);
    FAIL() << "Expected operation cancellation";
  } catch (const fl::Exception& error) {
    EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  }
  EXPECT_EQ(canceled_response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_TRUE(canceled_response.items.empty());
  EXPECT_EQ(session.TurnCount(), 1u);
  EXPECT_GE(streamed_tokens, 3);

  cancel_enabled = false;
  auto history = session.Transcript().Messages();
  history.emplace_back(FOUNDRY_LOCAL_ROLE_USER, "What word did I ask you to remember?");
  const auto recovery_started = std::chrono::steady_clock::now();
  auto recovery = MakeRequest("What word did I ask you to remember?");
  Response recovery_response;
  session.ProcessRequest(recovery, recovery_response);
  const auto recovery_elapsed = std::chrono::steady_clock::now() - recovery_started;

  EXPECT_EQ(AssistantText(recovery_response), ReferenceText(EncodeMessages(history, ModelInstance())));
  EXPECT_EQ(recovery_response.usage.prompt_tokens, EncodeMessages(history, ModelInstance()).size());
  EXPECT_EQ(recovery_response.usage.completion_tokens, 32);
  EXPECT_LT(recovery_elapsed, 30s);
  EXPECT_EQ(session.TurnCount(), 2u);
}

TEST_F(DynamicEngineChatTest, PreflightFromStreamingCallbackFailsWithoutBlocking) {
  ChatSession session(CatalogModel(), ModelInstance(), *logger_, telemetry_);
  auto request = MakeRequest("Count from one to ten.");
  auto preflight_request = MakeRequest("What comes next?");
  std::atomic<int> callback_count{0};
  std::atomic<int> error_code{0};

  session.SetStreamingCallback([&](flStreamingCallbackData event, void*) {
    auto* queue = reinterpret_cast<ItemQueue*>(event.item_queue);
    (void)queue->TryPop();
    ++callback_count;
    try {
      (void)session.CreateRequestPreflight(preflight_request);
    } catch (const fl::Exception& error) {
      error_code = error.code();
    }
    return 0;
  });

  Response response;
  session.ProcessRequest(request, response);
  EXPECT_GT(callback_count.load(), 0);
  EXPECT_EQ(error_code.load(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_FALSE(AssistantText(response).empty());

  EXPECT_NO_THROW((void)session.CreateRequestPreflight(preflight_request));
}

TEST_F(DynamicEngineChatTest, SessionOptionsFromStreamingCallbackFailWithoutBlocking) {
  const auto* api = FoundryLocalGetApi(FOUNDRY_LOCAL_API_VERSION);
  ASSERT_NE(api, nullptr);
  ChatSession session(CatalogModel(), ModelInstance(), *logger_, telemetry_);
  auto session_backend = fl::api::BorrowEmbeddedSession(session);
  KeyValuePairs initial_options;
  initial_options.Add("max_output_tokens", "11");
  session.SetSessionOptions(initial_options);
  KeyValuePairs changed_options;
  changed_options.Add("max_output_tokens", "7");
  auto request = MakeRequest("Count from one to ten.");
  std::atomic<int> callback_count{0};
  std::atomic<int> error_code{0};

  session.SetStreamingCallback([&](flStreamingCallbackData event, void*) {
    auto* queue = reinterpret_cast<ItemQueue*>(event.item_queue);
    (void)queue->TryPop();
    ++callback_count;
    auto* status = api->GetInferenceApi()->Session_SetOptions(
        AsHandle<flSession>(session_backend.get()), AsHandle<flKeyValuePairs>(&changed_options));
    if (status) {
      error_code = api->Status_GetErrorCode(status);
      api->Status_Release(status);
    }
    return 0;
  });

  Response response;
  session.ProcessRequest(request, response);
  EXPECT_GT(callback_count.load(), 0);
  EXPECT_EQ(error_code.load(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_FALSE(AssistantText(response).empty());

  Request next_request;
  next_request.AddOwnedItem(UserMessage("What comes next?"));
  EXPECT_EQ(session.CreateRequestPreflight(next_request)->Execute().output_reserve_tokens, 11);
  session.SetSessionOptions(changed_options);
  EXPECT_EQ(session.CreateRequestPreflight(next_request)->Execute().output_reserve_tokens, 7);
}

TEST_F(DynamicEngineChatTest, PreflightFromProcessingThreadFailsWithoutReenteringMutex) {
  class ReentrantLogger final : public ILogger {
   public:
    ChatSession* session = nullptr;
    const Request* request = nullptr;
    int preflight_error_code = 0;
    int options_error_code = 0;

    void Log(LogLevel, std::string_view message) override {
      if (!session || !message.starts_with("Completion stats:")) {
        return;
      }
      try {
        (void)session->CreateRequestPreflight(*request);
      } catch (const fl::Exception& error) {
        preflight_error_code = error.code();
      }
      KeyValuePairs options;
      options.Add("max_output_tokens", "7");
      try {
        session->SetSessionOptions(options);
      } catch (const fl::Exception& error) {
        options_error_code = error.code();
      }
    }
  };

  ReentrantLogger logger;
  ChatSession session(CatalogModel(), ModelInstance(), logger, telemetry_);
  auto request = MakeRequest("Count from one to ten.");
  logger.session = &session;
  logger.request = &request;

  Response response;
  session.ProcessRequest(request, response);
  EXPECT_EQ(logger.preflight_error_code, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_EQ(logger.options_error_code, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_FALSE(AssistantText(response).empty());
  EXPECT_NO_THROW((void)session.CreateRequestPreflight(request));
}

TEST_F(DynamicEngineChatTest, UnloadsAfterSessionsClose) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager load_manager(detector, *logger_);
  constexpr const char* kUnloadModelId = "dynamic-engine-unload-test-model";
  const auto result = load_manager.LoadModel(test::GetTestDataPath(kDynamicEngineModelId).string(), kUnloadModelId);
  ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess);

  {
    ChatSession session(CatalogModel(), *result.model, *logger_, telemetry_);
    auto request = MakeRequest("Reply OK.");
    Response response;
    session.ProcessRequest(request, response);
    EXPECT_EQ(AssistantText(response), ReferenceText(EncodeUserPrompt("Reply OK.", *result.model)));
  }

  EXPECT_TRUE(load_manager.UnloadModel(kUnloadModelId));
}

}  // namespace
}  // namespace fl
