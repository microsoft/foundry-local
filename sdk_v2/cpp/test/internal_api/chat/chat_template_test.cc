// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for BuildChatPrompt and EncodePrompt using a real tokenizer.
// Validates message formatting, template application, and token encoding.

#include "inferencing/generative/chat/chat_template.h"
#include "exception.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/model_load_manager.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"

#include <ort_genai.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace fl;

namespace {
#if FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS
constexpr const char* kTestChatTemplateKwargsModelId = "chat-template-kwargs-test";
constexpr const char* kTestChatTemplate =
    "{% for message in messages %}{{ message['content'] }}{% endfor %}"
    "{% if enable_thinking %}thinking{% else %}not-thinking{% endif %}"
    "{% if add_generation_prompt %}assistant{% endif %}";
#endif
}  // namespace

// ---------------------------------------------------------------------------
// Test fixture: loads the shared test model once per suite
// ---------------------------------------------------------------------------

class ChatTemplateTest : public ::testing::Test {
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

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
};

#if FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS
class ChatTemplateKwargsTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    model_directory_ = test::MakeUniqueTempPath("chat_template_kwargs_");
    ASSERT_TRUE(std::filesystem::create_directory(model_directory_));
    const auto source = test::GetTestDataPath("tiny-random-gpt2-fp32-1");
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
      if (entry.is_regular_file()) {
        std::filesystem::copy_file(entry.path(), model_directory_ / entry.path().filename());
      }
    }

    const auto tokenizer_config_path = model_directory_ / "tokenizer_config.json";
    std::ifstream input(tokenizer_config_path);
    ASSERT_TRUE(input);
    auto tokenizer_config = nlohmann::json::parse(input);
    input.close();
    tokenizer_config["chat_template"] = kTestChatTemplate;
    std::ofstream output(tokenizer_config_path, std::ios::trunc);
    output << tokenizer_config.dump(2);
    ASSERT_TRUE(output);
    output.close();

    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);

    auto result = load_manager_->LoadModel(model_directory_.string(), kTestChatTemplateKwargsModelId);

    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess)
        << "Failed to load test model from: " << model_directory_;

    model_ = result.model;
  }

  static void TearDownTestSuite() {
    if (load_manager_) {
      load_manager_->UnloadModel(kTestChatTemplateKwargsModelId);
    }

    load_manager_.reset();
    ep_detector_.reset();
    model_ = nullptr;
    std::error_code error;
    std::filesystem::remove_all(model_directory_, error);
    EXPECT_FALSE(error) << error.message();
    model_directory_.clear();
  }

  GenAIModelInstance& GetModel() { return *model_; }

  static inline std::filesystem::path model_directory_;
  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
};
#endif

// ---------------------------------------------------------------------------
// BuildChatPrompt tests
// ---------------------------------------------------------------------------

TEST_F(ChatTemplateTest, SingleUserMessage) {
  std::vector<TranscriptMessage> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  EXPECT_FALSE(prompt.empty());
  // The prompt should contain the user message content
  EXPECT_NE(prompt.find("Hello!"), std::string::npos)
      << "Prompt should contain user message. Got: " << prompt;
}

TEST_F(ChatTemplateTest, SystemAndUserMessages) {
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a helpful assistant."},
      {FOUNDRY_LOCAL_ROLE_USER, "What is 2+2?"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  EXPECT_FALSE(prompt.empty());
  EXPECT_NE(prompt.find("helpful assistant"), std::string::npos);
  EXPECT_NE(prompt.find("2+2"), std::string::npos);
}

TEST_F(ChatTemplateTest, MultiTurnConversation) {
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a math tutor."},
      {FOUNDRY_LOCAL_ROLE_USER, "What is 2+2?"},
      {FOUNDRY_LOCAL_ROLE_ASSISTANT, "4"},
      {FOUNDRY_LOCAL_ROLE_USER, "What about 3+3?"}};
  std::string prompt = BuildChatPrompt(messages, GetModel());
  EXPECT_FALSE(prompt.empty());
  // Multi-turn should contain all messages
  EXPECT_NE(prompt.find("math tutor"), std::string::npos);
  EXPECT_NE(prompt.find("2+2"), std::string::npos);
  EXPECT_NE(prompt.find("3+3"), std::string::npos);
}

TEST(ChatTemplateUnitTest, EmptyAssistantMessageRendersAsEmptyContent) {
  MessageItem empty_assistant;
  empty_assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;

  EXPECT_EQ(RenderMessageForPrompt(empty_assistant), "");
}

TEST_F(ChatTemplateTest, EmptyMessagesThrows) {
  std::vector<TranscriptMessage> messages;
  EXPECT_THROW(BuildChatPrompt(messages, GetModel()), fl::Exception);
}

TEST_F(ChatTemplateTest, PromptEndsWithAssistantPrefix) {
  // When add_generation_prompt=true, the template should end with the
  // assistant turn prefix so the model continues generating.
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  // Qwen2.5 uses <|im_start|>assistant format
  EXPECT_NE(prompt.find("assistant"), std::string::npos)
      << "Prompt should end with assistant prefix for generation. Got: " << prompt;
}

#if FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS
TEST_F(ChatTemplateKwargsTest, TypedKwargsChangePromptAndOmissionClearsPriorState) {
  std::vector<MessageItem> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};
  ToolCallContext default_context;
  ToolCallContext thinking_context;
  thinking_context.template_kwargs_json = R"({"enable_thinking":true})";
  ToolCallContext no_thinking_context;
  no_thinking_context.template_kwargs_json = R"({"enable_thinking":false})";

  std::string default_prompt = BuildChatPrompt(messages, GetModel(), default_context);
  std::string thinking_prompt = BuildChatPrompt(messages, GetModel(), thinking_context);
  std::string no_thinking_prompt = BuildChatPrompt(messages, GetModel(), no_thinking_context);
  std::string default_prompt_after_kwargs =
      BuildChatPrompt(messages, GetModel(), default_context);

  EXPECT_EQ(thinking_prompt, "Hello!thinkingassistant");
  EXPECT_EQ(no_thinking_prompt, "Hello!not-thinkingassistant");
  EXPECT_EQ(default_prompt_after_kwargs, default_prompt)
      << "Omitting kwargs must clear tokenizer state from the previous render";
}
#else
TEST_F(ChatTemplateTest, TemplateKwargsRequireSupportedGenAI) {
  std::vector<MessageItem> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};
  ToolCallContext empty_context;
  empty_context.template_kwargs_json = "{}";

  EXPECT_EQ(BuildChatPrompt(messages, GetModel(), empty_context),
            BuildChatPrompt(messages, GetModel()))
      << "An empty kwargs object should remain a no-op with the stable GenAI dependency";

  try {
    (void)BuildChatPrompt(messages, GetModel(), "", R"({"enable_thinking":false})");
    FAIL() << "Expected chat_template_kwargs to be rejected by the stable GenAI dependency";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_NE(std::string(e.what()).find("requires a newer ONNX Runtime GenAI package"),
              std::string::npos);
  }
}
#endif

// ---------------------------------------------------------------------------
// EncodePrompt tests
// ---------------------------------------------------------------------------

TEST_F(ChatTemplateTest, EncodeProducesTokens) {
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  auto sequences = EncodePrompt(prompt, GetModel());

  ASSERT_NE(sequences, nullptr);
  size_t token_count = sequences->SequenceCount(0);
  EXPECT_GT(token_count, 0u) << "Encoded prompt should have at least 1 token";
}

TEST_F(ChatTemplateTest, LongerMessageProducesMoreTokens) {
  std::vector<TranscriptMessage> short_msgs = {
      {FOUNDRY_LOCAL_ROLE_USER, "Hi"}};
  std::vector<TranscriptMessage> long_msgs = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a detailed technical writer who explains everything thoroughly."},
      {FOUNDRY_LOCAL_ROLE_USER,
       "Explain the theory of relativity in detail, covering both special and general relativity."}};
  std::string short_prompt = BuildChatPrompt(short_msgs, GetModel());
  std::string long_prompt = BuildChatPrompt(long_msgs, GetModel());

  auto short_seq = EncodePrompt(short_prompt, GetModel());
  auto long_seq = EncodePrompt(long_prompt, GetModel());

  EXPECT_GT(long_seq->SequenceCount(0), short_seq->SequenceCount(0))
      << "Longer message should produce more tokens";
}

TEST_F(ChatTemplateTest, EmptyStringEncodesSuccessfully) {
  // Even an empty string should encode without crashing
  auto sequences = EncodePrompt("", GetModel());
  ASSERT_NE(sequences, nullptr);
}
