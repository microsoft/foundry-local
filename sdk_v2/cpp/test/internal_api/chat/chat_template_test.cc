// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for BuildChatPrompt and EncodePrompt using a real tokenizer.
// Validates message formatting, template application, and token encoding.

#include "inferencing/generative/chat/chat_template.h"
#include "exception.h"
#include "inferencing/model_load_manager.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"

#include <ort_genai.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace fl;

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

// ---------------------------------------------------------------------------
// BuildChatPrompt tests
// ---------------------------------------------------------------------------

TEST_F(ChatTemplateTest, SingleUserMessage) {
  std::vector<MessageItem> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  EXPECT_FALSE(prompt.empty());
  // The prompt should contain the user message content
  EXPECT_NE(prompt.find("Hello!"), std::string::npos)
      << "Prompt should contain user message. Got: " << prompt;
}

TEST_F(ChatTemplateTest, SystemAndUserMessages) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a helpful assistant."},
      {FOUNDRY_LOCAL_ROLE_USER, "What is 2+2?"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  EXPECT_FALSE(prompt.empty());
  EXPECT_NE(prompt.find("helpful assistant"), std::string::npos);
  EXPECT_NE(prompt.find("2+2"), std::string::npos);
}

TEST_F(ChatTemplateTest, MultiTurnConversation) {
  std::vector<MessageItem> messages = {
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
  std::vector<MessageItem> messages;
  EXPECT_THROW(BuildChatPrompt(messages, GetModel()), fl::Exception);
}

TEST_F(ChatTemplateTest, PromptEndsWithAssistantPrefix) {
  // When add_generation_prompt=true, the template should end with the
  // assistant turn prefix so the model continues generating.
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  // Qwen2.5 uses <|im_start|>assistant format
  EXPECT_NE(prompt.find("assistant"), std::string::npos)
      << "Prompt should end with assistant prefix for generation. Got: " << prompt;
}

// ---------------------------------------------------------------------------
// EncodePrompt tests
// ---------------------------------------------------------------------------

TEST_F(ChatTemplateTest, EncodeProducesTokens) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  auto sequences = EncodePrompt(prompt, GetModel());

  ASSERT_NE(sequences, nullptr);
  size_t token_count = sequences->SequenceCount(0);
  EXPECT_GT(token_count, 0u) << "Encoded prompt should have at least 1 token";
}

TEST_F(ChatTemplateTest, LongerMessageProducesMoreTokens) {
  std::vector<MessageItem> short_msgs = {
      {FOUNDRY_LOCAL_ROLE_USER, "Hi"}};
  std::vector<MessageItem> long_msgs = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a detailed technical writer who explains everything thoroughly."},
      {FOUNDRY_LOCAL_ROLE_USER, "Explain the theory of relativity in detail, covering both special and general relativity."}};
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
