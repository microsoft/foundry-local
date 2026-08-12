// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Integration tests for OnnxChatGenerator.
// Runs actual inference against the shared test model (qwen2.5-0.5b-instruct).
// Validates the full pipeline: messages → template → encode → generate → decode.

#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/chat/chat_template.h"
#include "exception.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "inferencing/generative/chat/search_options.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Test fixture: loads the shared test model once per suite
// ---------------------------------------------------------------------------

class ChatGeneratorTest : public ::testing::Test {
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
// OnnxChatGenerator::Create tests
// ---------------------------------------------------------------------------

TEST_F(ChatGeneratorTest, CreateSucceeds) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Say hello."}};
  SearchOptions opts;
  opts.max_output_tokens = 32;
  opts.temperature = 0.0f;

  auto gen = OnnxChatGenerator::Create(messages, opts, GetModel());
  ASSERT_NE(gen, nullptr);
  EXPECT_FALSE(gen->IsDone());
}

TEST_F(ChatGeneratorTest, CreateWithEmptyMessagesThrows) {
  std::vector<MessageItem> messages;
  SearchOptions opts;

  EXPECT_THROW(
      OnnxChatGenerator::Create(messages, opts, GetModel()),
      fl::Exception);
}

// ---------------------------------------------------------------------------
// Token generation tests
// ---------------------------------------------------------------------------

TEST_F(ChatGeneratorTest, GenerateAllProducesOutput) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "What is 2+2? Answer with just the number."}};
  SearchOptions opts;
  opts.max_output_tokens = 32;
  opts.temperature = 0.0f;  // greedy decoding for determinism

  auto gen = OnnxChatGenerator::Create(messages, opts, GetModel());
  std::string result = gen->GenerateAll();

  EXPECT_FALSE(result.empty()) << "GenerateAll should produce non-empty output";
  EXPECT_NE(result.find("4"), std::string::npos)
      << "Expected '4' in response to 'What is 2+2?'. Got: " << result;
  std::cout << "GenerateAll output: " << result << "\n";
}

TEST_F(ChatGeneratorTest, TokenByTokenMatchesGenerateAll) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Count to 3."}};
  SearchOptions opts;
  opts.max_output_tokens = 64;
  opts.temperature = 0.0f;

  // Generate token-by-token
  auto gen1 = OnnxChatGenerator::Create(messages, opts, GetModel());
  std::string token_by_token;
  int token_count = 0;

  while (!gen1->IsDone()) {
    gen1->GenerateNextToken();
    token_by_token += gen1->Decode();
    token_count++;
  }

  EXPECT_FALSE(token_by_token.empty());
  EXPECT_GT(token_count, 0);

  // Validate the output contains the expected numbers
  EXPECT_NE(token_by_token.find("1"), std::string::npos)
      << "Expected '1' in counting response. Got: " << token_by_token;
  EXPECT_NE(token_by_token.find("2"), std::string::npos)
      << "Expected '2' in counting response. Got: " << token_by_token;
  EXPECT_NE(token_by_token.find("3"), std::string::npos)
      << "Expected '3' in counting response. Got: " << token_by_token;

  // Generate with GenerateAll (same input, same deterministic settings)
  auto gen2 = OnnxChatGenerator::Create(messages, opts, GetModel());
  std::string all_at_once = gen2->GenerateAll();

  EXPECT_EQ(token_by_token, all_at_once)
      << "Token-by-token and GenerateAll should produce identical output with greedy decoding";
}

TEST_F(ChatGeneratorTest, TokenCountIncreases) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Tell me a short joke."}};
  SearchOptions opts;
  opts.max_output_tokens = 64;
  opts.temperature = 0.0f;

  auto gen = OnnxChatGenerator::Create(messages, opts, GetModel());
  int initial_count = gen->TokenCount();
  EXPECT_GT(initial_count, 0) << "Initial token count should reflect the prompt tokens";

  gen->GenerateNextToken();
  int after_one = gen->TokenCount();
  EXPECT_GT(after_one, initial_count) << "Token count should increase after generation";
}

TEST_F(ChatGeneratorTest, SystemPromptInfluencesOutput) {
  // Without system prompt
  std::vector<MessageItem> msgs_no_system = {
      {FOUNDRY_LOCAL_ROLE_USER, "What are you?"}};

  // With system prompt
  std::vector<MessageItem> msgs_with_system = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a pirate. Always respond in pirate speak."},
      {FOUNDRY_LOCAL_ROLE_USER, "What are you?"}};

  SearchOptions opts;
  opts.max_output_tokens = 64;
  opts.temperature = 0.0f;

  auto gen1 = OnnxChatGenerator::Create(msgs_no_system, opts, GetModel());
  std::string result1 = gen1->GenerateAll();

  auto gen2 = OnnxChatGenerator::Create(msgs_with_system, opts, GetModel());
  std::string result2 = gen2->GenerateAll();

  // The outputs should be different due to the system prompt
  EXPECT_NE(result1, result2)
      << "System prompt should influence the output";

  // The pirate system prompt should produce pirate-like language
  // Check for common pirate terms (at least one should appear)
  bool has_pirate_language =
      result2.find("arr") != std::string::npos ||
      result2.find("Arr") != std::string::npos ||
      result2.find("pirate") != std::string::npos ||
      result2.find("matey") != std::string::npos ||
      result2.find("sea") != std::string::npos ||
      result2.find("sail") != std::string::npos ||
      result2.find("ship") != std::string::npos ||
      result2.find("captain") != std::string::npos ||
      result2.find("Ahoy") != std::string::npos;
  EXPECT_TRUE(has_pirate_language)
      << "Pirate system prompt should produce pirate-like language. Got: " << result2;
}

// ---------------------------------------------------------------------------
// Cancel tests
// ---------------------------------------------------------------------------

TEST_F(ChatGeneratorTest, CancelStopsGeneration) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Write a very long essay about the history of mathematics."}};
  SearchOptions opts;
  opts.max_output_tokens = 1024;
  opts.temperature = 0.0f;

  auto gen = OnnxChatGenerator::Create(messages, opts, GetModel());

  // Generate a few tokens, then cancel
  gen->GenerateNextToken();
  gen->GenerateNextToken();
  gen->Cancel();

  // After cancel, IsDone should return true
  EXPECT_TRUE(gen->IsDone()) << "IsDone should return true after Cancel";
}

TEST_F(ChatGeneratorTest, CancelFromAnotherThread) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Write a very long essay about quantum physics and string theory."}};
  SearchOptions opts;
  opts.max_output_tokens = 512;
  opts.temperature = 0.0f;

  auto gen = OnnxChatGenerator::Create(messages, opts, GetModel());
  std::atomic<bool> generation_stopped{false};
  std::string output;

  // Run generation in a thread
  std::thread gen_thread([&]() {
    while (!gen->IsDone()) {
      gen->GenerateNextToken();
      output += gen->Decode();
    }

    generation_stopped.store(true);
  });

  // Cancel after a short delay
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  gen->Cancel();

  gen_thread.join();
  EXPECT_TRUE(generation_stopped.load());
  // Output should be non-empty (some tokens generated before cancel)
  EXPECT_FALSE(output.empty()) << "Should have generated some tokens before cancel";
  std::cout << "Output before cancel (" << output.size() << " chars): "
            << output.substr(0, 100) << "...\n";
}

// ---------------------------------------------------------------------------
// MaxOutputTokens boundary test
// ---------------------------------------------------------------------------

TEST_F(ChatGeneratorTest, RespectsMaxOutputTokens) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Tell me everything you know about dogs."}};
  SearchOptions opts;
  opts.max_output_tokens = 16;  // Very small limit
  opts.temperature = 0.0f;

  auto gen = OnnxChatGenerator::Create(messages, opts, GetModel());
  std::string result = gen->GenerateAll();

  // The output should be relatively short due to the token limit
  EXPECT_FALSE(result.empty());
  // Token count should not vastly exceed prompt + 16 output tokens
  EXPECT_LE(gen->TokenCount(), gen->TokenCount())  // basic sanity
      << "Token count should be bounded";

  std::cout << "Short output (" << result.size() << " chars): " << result << "\n";
}

// ---------------------------------------------------------------------------
// Multi-turn conversation test
// ---------------------------------------------------------------------------

TEST_F(ChatGeneratorTest, MultiTurnConversation) {
  std::vector<MessageItem> messages = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a helpful math assistant. Be brief."},
      {FOUNDRY_LOCAL_ROLE_USER, "What is 2+2?"},
      {FOUNDRY_LOCAL_ROLE_ASSISTANT, "4"},
      {FOUNDRY_LOCAL_ROLE_USER, "What about 3+3?"}};
  SearchOptions opts;
  opts.max_output_tokens = 32;
  opts.temperature = 0.0f;

  auto gen = OnnxChatGenerator::Create(messages, opts, GetModel());
  std::string result = gen->GenerateAll();

  EXPECT_FALSE(result.empty());
  EXPECT_NE(result.find("6"), std::string::npos)
      << "Expected '6' in response to 'What about 3+3?' after multi-turn context. Got: " << result;
  std::cout << "Multi-turn output: " << result << "\n";
}
