// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for Session (base) and ChatSession.
// Unit tests validate state management without a model.
// Integration tests run actual inference against the shared test model.

#include "inferencing/generative/chat/chat_session.h"
#include "exception.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/generative/chat/search_options.h"
#include "items/text_item.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "model.h"
#include "internal_api/null_session_manager.h"
#include "internal_api/null_telemetry.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "utils/string_utils.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace fl;

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

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline fl::test::FakeServiceBindings svc_;
  static inline Model catalog_model_ = Model::FromModelInfo(
      ModelInfo{}, "", svc_.download_manager, svc_.model_load_manager);
  fl::test::NullTelemetry null_telemetry_;
  fl::test::NullSessionManager null_session_manager_;
};

// ===========================================================================
// Construction
// ===========================================================================

TEST_F(ChatSessionTest, ConstructWithModelOnly) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  EXPECT_EQ(session.MessageCount(), 0u);
  EXPECT_TRUE(session.GetHistory().empty());
  EXPECT_EQ(session.TurnCount(), 0u);
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
  EXPECT_EQ(session.GetHistory()[0].role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(session.GetHistory()[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(session.GetHistory()[1].GetSimpleText(), text);
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

TEST_F(ChatSessionTest, RunStreamingCancellation) {
  ChatSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  request.AddOwnedItem(MakeMessage(FOUNDRY_LOCAL_ROLE_USER, "Count from 1 to 100."));
  request.options.Add("max_output_tokens", "256");
  request.options.Add("temperature", "0");

  int tokens_received = 0;

  fl::Session::StreamingCallbackFn callback_fn = [&](flStreamingCallbackData event, void* /*user_data*/) -> int {
    fl::ItemQueue* queue = reinterpret_cast<fl::ItemQueue*>(event.item_queue);
    auto item = queue->TryPop();
    if (!item) {
      // should never happen
      return 0;
    }

    ++tokens_received;
    bool cancel = tokens_received >= 3;  // example cancellation condition: after receiving 3 tokens
    return cancel ? 1 : 0;               // cancel after 3 tokens
  };

  session.SetStreamingCallback(callback_fn);

  Response response;
  session.ProcessRequest(request, response);

  // Should have stopped early
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  // we check cancellation at the start of each loop and we don't use std::atomic to make processing cheaper
  // so allow for a couple of extra tokens to come through after the cancellation condition is met
  EXPECT_LE(tokens_received, 6);

  // Cancelled requests should not commit to history (delayed commit)
  EXPECT_EQ(session.MessageCount(), 0u);
  EXPECT_EQ(session.TurnCount(), 0u);
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
