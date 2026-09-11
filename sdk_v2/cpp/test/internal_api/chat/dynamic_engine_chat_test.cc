// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/onnx_chat_engine.h"
#include "inferencing/generative/chat/onnx_engine_chat_stream.h"
#include "inferencing/model_load_manager.h"
#include "internal_api/test_helpers.h"
#include "items/text_item.h"
#include "model.h"
#include "telemetry/telemetry_logger.h"
#include "utils/safe_getenv.h"
#include "utils/string_utils.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <ort_genai.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

using namespace std::chrono_literals;

namespace fl {
namespace {

constexpr const char* kDynamicEngineModelPathVariable = "FOUNDRY_LOCAL_DYNAMIC_ENGINE_TEST_MODEL_PATH";
constexpr const char* kDynamicEngineModelId = "dynamic-engine-test-model";

class DynamicEngineEpDetector final : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {
        {"CPU", {"CPUExecutionProvider"}},
        {"GPU",
         {"CUDAExecutionProvider", "WebGpuExecutionProvider", "OpenVINOExecutionProvider",
          "NvTensorRTRTXExecutionProvider", "VitisAIExecutionProvider", "RyzenAIExecutionProvider",
          "QNNExecutionProvider"}},
    };
  }
};

void StageDynamicEngineModel(const std::filesystem::path& source, const std::filesystem::path& destination) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
    const auto relative = std::filesystem::relative(entry.path(), source);
    const auto destination_path = destination / relative;
    if (entry.is_directory()) {
      std::filesystem::create_directories(destination_path);
      continue;
    }
    if (!entry.is_regular_file()) {
      continue;
    }

    std::filesystem::create_directories(destination_path.parent_path());
    if (relative == "genai_config.json") {
      std::filesystem::copy_file(entry.path(), destination_path, std::filesystem::copy_options::overwrite_existing);
      continue;
    }

    std::error_code hard_link_error;
    std::filesystem::create_hard_link(entry.path(), destination_path, hard_link_error);
    if (hard_link_error) {
      std::filesystem::copy_file(entry.path(), destination_path, std::filesystem::copy_options::overwrite_existing);
    }
  }

  const auto config_path = destination / "genai_config.json";
  std::ifstream input(config_path);
  if (!input) {
    throw std::runtime_error("Failed to open staged dynamic Engine genai_config.json");
  }
  auto config = nlohmann::json::parse(input);
  input.close();

  if (!config.contains("engine") || !config["engine"].contains("dynamic_batching")) {
    throw std::runtime_error("Dynamic Engine test model must define engine.dynamic_batching");
  }
  config["engine"]["dynamic_batching"]["max_batch_size"] = 2;

  std::ofstream output(config_path, std::ios::trunc);
  if (!output || !(output << config.dump(2))) {
    throw std::runtime_error("Failed to update staged dynamic Engine genai_config.json");
  }
}

class DynamicEngineModelStaging {
 public:
  explicit DynamicEngineModelStaging(const std::filesystem::path& source)
      : root_(test::TempPath::CreateTempDir("dynamic-engine-chat-")) {
    StageDynamicEngineModel(source, root_.path());
  }

  const std::filesystem::path& path() const { return root_.path(); }

 private:
  test::TempPath root_;
};

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
    const auto configured_path = test::SafeGetEnv(kDynamicEngineModelPathVariable);
    if (configured_path.empty()) {
      skip_reason_ = std::string(kDynamicEngineModelPathVariable) +
                     " is not set to a model with engine.dynamic_batching";
      return;
    }

    const std::filesystem::path source(configured_path);
    if (!std::filesystem::exists(source / "genai_config.json")) {
      skip_reason_ = std::string(kDynamicEngineModelPathVariable) +
                     " must point to a directory containing genai_config.json";
      return;
    }

    staged_model_ = std::make_unique<DynamicEngineModelStaging>(source);
    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<DynamicEngineEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);

    const auto result = load_manager_->LoadModel(staged_model_->path().string(), kDynamicEngineModelId);
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
    staged_model_.reset();
  }

  void SetUp() override {
    if (!skip_reason_.empty()) {
      GTEST_SKIP() << skip_reason_;
    }
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

  static inline std::string skip_reason_;
  static inline std::unique_ptr<DynamicEngineModelStaging> staged_model_;
  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<DynamicEngineEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  TelemetryLogger telemetry_{"dynamic-engine-test", test::NullLog()};
};

TEST_F(DynamicEngineChatTest, GeneratesWithinOutputLimitAndRetainsContinuation) {
  ChatSession session(CatalogModel(), ModelInstance(), *logger_, telemetry_);

  auto first = MakeRequest("Remember the word sapphire. Reply OK.");
  Response first_response;
  session.ProcessRequest(first, first_response);
  EXPECT_FALSE(AssistantText(first_response).empty());
  EXPECT_LE(first_response.usage.completion_tokens, 32);

  auto full_history = session.Transcript().Messages();
  full_history.emplace_back(FOUNDRY_LOCAL_ROLE_USER, "What word did I ask you to remember?");
  const auto expected_second_prompt_tokens = EncodeMessages(full_history, ModelInstance()).size();

  auto second = MakeRequest("What word did I ask you to remember?");
  Response second_response;
  session.ProcessRequest(second, second_response);

  EXPECT_NE(test::ToLower(AssistantText(second_response)).find("sapphire"), std::string::npos);
  EXPECT_EQ(session.TurnCount(), 2u);
  EXPECT_EQ(second_response.usage.prompt_tokens, expected_second_prompt_tokens)
      << "resident suffix admission and fresh replay must report the same complete logical prompt";
  EXPECT_LE(second_response.usage.completion_tokens, 32);
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
  EXPECT_EQ(warm_second.text, fresh_second.text);
  EXPECT_EQ(warm_second.usage.finish_reason, fresh_second.usage.finish_reason);
  EXPECT_EQ(warm_second.usage.prompt_tokens, fresh_second.usage.prompt_tokens);
  EXPECT_EQ(warm_second.usage.generated_tokens, fresh_second.usage.generated_tokens);
}

TEST_F(DynamicEngineChatTest, RunsTwoConcurrentSessions) {
  auto run = [&](std::string prompt) {
    ChatSession session(CatalogModel(), ModelInstance(), *logger_, telemetry_);
    auto request = MakeRequest(std::move(prompt));
    Response response;
    session.ProcessRequest(request, response);
    return AssistantText(response);
  };

  auto first = std::async(std::launch::async, run, "What is 2+2? Answer with just the number.");
  auto second = std::async(std::launch::async, run, "What is 3+3? Answer with just the number.");

  EXPECT_NE(first.get().find("4"), std::string::npos);
  EXPECT_NE(second.get().find("6"), std::string::npos);
}

TEST_F(DynamicEngineChatTest, CapacityTimeoutFailsOnlyNewestWaitingConversation) {
  OnnxChatEngine engine(ModelInstance(), 100ms);
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

  engine.Cancel(first);
  engine.Cancel(second);
  engine.Close(first);
  engine.Close(second);
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

  cancel_enabled = false;
  const auto recovery_started = std::chrono::steady_clock::now();
  auto recovery = MakeRequest("What word did I ask you to remember?");
  Response recovery_response;
  session.ProcessRequest(recovery, recovery_response);
  const auto recovery_elapsed = std::chrono::steady_clock::now() - recovery_started;

  EXPECT_NE(test::ToLower(AssistantText(recovery_response)).find("sapphire"), std::string::npos);
  EXPECT_LT(recovery_elapsed, 30s);
  EXPECT_EQ(session.TurnCount(), 2u);
}

TEST_F(DynamicEngineChatTest, UnloadsAfterSessionsClose) {
  DynamicEngineEpDetector detector;
  ModelLoadManager load_manager(detector, *logger_);
  constexpr const char* kUnloadModelId = "dynamic-engine-unload-test-model";
  const auto result = load_manager.LoadModel(staged_model_->path().string(), kUnloadModelId);
  ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess);

  {
    ChatSession session(CatalogModel(), *result.model, *logger_, telemetry_);
    auto request = MakeRequest("Reply OK.");
    Response response;
    session.ProcessRequest(request, response);
    EXPECT_FALSE(AssistantText(response).empty());
  }

  EXPECT_TRUE(load_manager.UnloadModel(kUnloadModelId));
}

}  // namespace
}  // namespace fl
