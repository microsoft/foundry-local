// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/onnx_chat_engine.h"
#include "inferencing/generative/chat/onnx_engine_chat_stream.h"
#include "inferencing/model_load_manager.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "items/text_item.h"
#include "model.h"
#include "telemetry/telemetry_logger.h"

#include <gtest/gtest.h>
#include <ort_genai.h>

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

TEST_F(DynamicEngineChatTest, CapacityTimeoutFailsOnlyNewestWaitingConversation) {
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
  engine.BeginTurn(waiting, tokens, options, tool_context, false);

  auto wait_result = std::async(std::launch::async, [&]() {
    try {
      (void)engine.WaitForToken(waiting);
      return std::string{};
    } catch (const std::exception& error) {
      return std::string(error.what());
    }
  });

  ASSERT_EQ(wait_result.wait_for(5s), std::future_status::ready)
      << "The capacity-blocked request exceeded its bounded wait";
  EXPECT_NE(wait_result.get().find("capacity remained unavailable"), std::string::npos);

  EXPECT_TRUE(engine.WaitForToken(first).has_value());
  EXPECT_TRUE(engine.WaitForToken(second).has_value());
  engine.Cancel(first);
  engine.Cancel(second);
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
  session.ProcessRequest(canceled, canceled_response);
  EXPECT_EQ(canceled_response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
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
