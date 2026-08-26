// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for ResponseStore and ResponseConverter.

#include "inferencing/generative/openresponses/response_converter.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "contracts/responses.h"
#include "inferencing/session/session.h"
#include "items/message_item.h"
#include "items/tool_call_item.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <set>
#include <string>

using namespace fl::responses;
using namespace fl;
using json = nlohmann::json;

// ========================================================================
// ResponseConverter::GenerateId
// ========================================================================

TEST(ResponseConverterTest, GenerateIdHasCorrectPrefix) {
  auto id = ResponseConverter::GenerateId("resp");
  EXPECT_EQ(id.substr(0, 5), "resp_") << "ID: " << id;
}

TEST(ResponseConverterTest, GenerateIdProducesUniqueValues) {
  std::set<std::string> ids;
  for (int i = 0; i < 100; ++i) {
    ids.insert(ResponseConverter::GenerateId("resp"));
  }

  EXPECT_EQ(ids.size(), 100u) << "Expected 100 unique IDs";
}

TEST(ResponseConverterTest, GenerateIdDifferentPrefixes) {
  auto resp_id = ResponseConverter::GenerateId("resp");
  auto msg_id = ResponseConverter::GenerateId("msg");

  EXPECT_EQ(resp_id.substr(0, 5), "resp_");
  EXPECT_EQ(msg_id.substr(0, 4), "msg_");
}

// ========================================================================
// ResponseConverter::ToSessionRequest
// ========================================================================

TEST(ResponseConverterTest, ToSessionRequestStringInput) {
  json req = {
      {"model", "test-model"},
      {"input", "Hello, world!"},
  };

  auto params = req.get<ResponseCreateParams>();
  auto session_req = ResponseConverter::ToSessionRequest(params, nullptr, nullptr);

  // Should have one user message
  ASSERT_EQ(session_req.items.size(), 1u);
  ASSERT_EQ(session_req.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  const MessageItem& msg = static_cast<const MessageItem&>(*session_req.items[0]);
  EXPECT_EQ(msg.role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(msg.GetSimpleText(), "Hello, world!");
}

TEST(ResponseConverterTest, ToSessionRequestWithInstructions) {
  json req = {
      {"model", "test-model"},
      {"input", "Hello"},
      {"instructions", "You are a helpful robot."},
  };

  auto params = req.get<ResponseCreateParams>();
  auto session_req = ResponseConverter::ToSessionRequest(params, nullptr, nullptr);

  ASSERT_GE(session_req.items.size(), 2u);
  // First item should be the system message
  const MessageItem& system_msg = static_cast<const MessageItem&>(*session_req.items[0]);
  EXPECT_EQ(system_msg.role, FOUNDRY_LOCAL_ROLE_SYSTEM);
  EXPECT_EQ(system_msg.GetSimpleText(), "You are a helpful robot.");
  // Second should be the user message
  const MessageItem& user_msg = static_cast<const MessageItem&>(*session_req.items[1]);
  EXPECT_EQ(user_msg.role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(user_msg.GetSimpleText(), "Hello");
}

TEST(ResponseConverterTest, ToSessionRequestWithTemperature) {
  json req = {
      {"model", "test-model"},
      {"input", "test"},
      {"temperature", 0.7},
  };

  auto params = req.get<ResponseCreateParams>();
  auto session_req = ResponseConverter::ToSessionRequest(params, nullptr, nullptr);

  EXPECT_STREQ(session_req.options.Find("temperature"), "0.700000");
}

TEST(ResponseConverterTest, ToSessionRequestWithMaxOutputTokens) {
  json req = {
      {"model", "test-model"},
      {"input", "test"},
      {"max_output_tokens", 512},
  };

  auto params = req.get<ResponseCreateParams>();
  auto session_req = ResponseConverter::ToSessionRequest(params, nullptr, nullptr);

  EXPECT_STREQ(session_req.options.Find("max_output_tokens"), "512");
}

TEST(ResponseConverterTest, ToSessionRequestArrayInput) {
  json req = {
      {"model", "test-model"},
      {"input", json::array({
                    {{"role", "user"}, {"content", "First message"}},
                    {{"role", "assistant"}, {"content", "Response"}},
                    {{"role", "user"}, {"content", "Follow-up"}},
                })},
  };

  auto params = req.get<ResponseCreateParams>();
  auto session_req = ResponseConverter::ToSessionRequest(params, nullptr, nullptr);

  ASSERT_EQ(session_req.items.size(), 3u);
  const MessageItem& msg1 = static_cast<const MessageItem&>(*session_req.items[0]);
  EXPECT_EQ(msg1.role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(msg1.GetSimpleText(), "First message");
  const MessageItem& msg2 = static_cast<const MessageItem&>(*session_req.items[1]);
  EXPECT_EQ(msg2.role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(msg2.GetSimpleText(), "Response");
  const MessageItem& msg3 = static_cast<const MessageItem&>(*session_req.items[2]);
  EXPECT_EQ(msg3.role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(msg3.GetSimpleText(), "Follow-up");
}

TEST(ResponseConverterTest, ToSessionRequestWithPreviousContext) {
  json req = {
      {"model", "test-model"},
      {"input", "Follow-up question"},
      {"previous_response_id", "resp_abc123"},
  };

  json prev_input = json::array({
      {{"type", "message"}, {"role", "user"}, {"content", "Original question"}},
  });

  json prev_output = json::array({
      {{"type", "message"}, {"role", "assistant"}, {"content", json::array({{{"type", "output_text"}, {"text", "Original answer"}}})}},
  });

  auto params = req.get<ResponseCreateParams>();
  auto session_req =
      ResponseConverter::ToSessionRequest(params, &prev_input, &prev_output);

  // Should have: previous user msg + previous assistant msg + new user msg
  ASSERT_GE(session_req.items.size(), 3u);
  const MessageItem& msg1 = static_cast<const MessageItem&>(*session_req.items[0]);
  EXPECT_EQ(msg1.role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(msg1.GetSimpleText(), "Original question");
  const MessageItem& msg2 = static_cast<const MessageItem&>(*session_req.items[1]);
  EXPECT_EQ(msg2.role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(msg2.GetSimpleText(), "Original answer");
  const MessageItem& msg3 = static_cast<const MessageItem&>(*session_req.items[2]);
  EXPECT_EQ(msg3.role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(msg3.GetSimpleText(), "Follow-up question");
}

// ========================================================================
// ResponseConverter::BuildResponseObject
// ========================================================================

TEST(ResponseConverterTest, BuildResponseObjectHasRequiredFields) {
  json req = {
      {"model", "test-model"},
      {"input", "test"},
  };

  auto params = req.get<ResponseCreateParams>();

  // Build a typed output matching what FromSessionResponse would produce
  std::vector<ResponseOutputItem> output;
  ResponseOutputMessage msg;
  msg.id = "msg_1";
  msg.role = "assistant";
  msg.status = ResponseStatus::kCompleted;
  msg.content.push_back(OutputTextContent{"hello"});
  output.push_back(msg);

  TokenUsage usage{10, 5, 15, 2};

  auto typed = ResponseConverter::BuildResponseObject(
      "resp_123", 1700000000, "test-model", params,
      std::move(output), "hello", usage);
  json response = typed;

  EXPECT_EQ(response["id"], "resp_123");
  EXPECT_EQ(response["object"], "response");
  EXPECT_EQ(response["created_at"], 1700000000);
  EXPECT_EQ(response["model"], "test-model");
  EXPECT_EQ(response["status"], "completed");
  EXPECT_EQ(response["output_text"], "hello");

  // Output should have a message item
  ASSERT_EQ(response["output"].size(), 1u);
  EXPECT_EQ(response["output"][0]["type"], "message");
  EXPECT_EQ(response["output"][0]["id"], "msg_1");

  // Usage
  EXPECT_EQ(response["usage"]["input_tokens"], 10);
  EXPECT_EQ(response["usage"]["output_tokens"], 5);
  EXPECT_EQ(response["usage"]["output_tokens_details"]["reasoning_tokens"], 2);
  EXPECT_EQ(response["usage"]["total_tokens"], 15);
}

TEST(ResponseConverterTest, BuildResponseObjectEchoesParameters) {
  json req = {
      {"model", "test-model"},
      {"input", "test"},
      {"temperature", 0.8},
      {"top_p", 0.95},
      {"max_output_tokens", 256},
      {"instructions", "Be helpful"},
  };

  auto params = req.get<ResponseCreateParams>();

  std::vector<ResponseOutputItem> output;
  TokenUsage usage{0, 0, 0};

  auto typed = ResponseConverter::BuildResponseObject(
      "resp_123", 1700000000, "test-model", params,
      std::move(output), "", usage);
  json response = typed;

  EXPECT_NEAR(response["temperature"].get<double>(), 0.8, 1e-6);
  EXPECT_NEAR(response["top_p"].get<double>(), 0.95, 1e-6);
  EXPECT_EQ(response["max_output_tokens"], 256);
  EXPECT_EQ(response["instructions"], "Be helpful");
}

// ========================================================================
// ResponseConverter::BuildFailedResponseObject
// ========================================================================

TEST(ResponseConverterTest, BuildFailedResponseObjectHasErrorFields) {
  json req = {
      {"model", "test-model"},
      {"input", "test"},
  };

  auto params = req.get<ResponseCreateParams>();

  auto typed = ResponseConverter::BuildFailedResponseObject(
      "resp_err", 1700000000, "test-model", params, "server_error", "Something broke");
  json response = typed;

  EXPECT_EQ(response["id"], "resp_err");
  EXPECT_EQ(response["status"], "failed");
  EXPECT_EQ(response["error"]["code"], "server_error");
  EXPECT_EQ(response["error"]["message"], "Something broke");
  EXPECT_TRUE(response.contains("failed_at"));
}

// ========================================================================
// ResponseConverter::ToInputItems
// ========================================================================

TEST(ResponseConverterTest, ToInputItemsFromStringInput) {
  json req = {
      {"model", "test-model"},
      {"input", "Hello"},
  };

  auto items = ResponseConverter::ToInputItems(req);

  ASSERT_TRUE(items.is_array());
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0]["type"], "message");
  EXPECT_EQ(items[0]["role"], "user");
  EXPECT_TRUE(items[0].contains("id"));
}

TEST(ResponseConverterTest, ToInputItemsFromArrayInput) {
  json req = {
      {"model", "test-model"},
      {"input", json::array({
                    {{"role", "user"}, {"content", "Hello"}},
                    {{"role", "assistant"}, {"content", "Hi"}},
                })},
  };

  auto items = ResponseConverter::ToInputItems(req);

  ASSERT_TRUE(items.is_array());
  ASSERT_EQ(items.size(), 2u);
  // Each should have a generated ID
  EXPECT_TRUE(items[0].contains("id"));
  EXPECT_TRUE(items[1].contains("id"));
  EXPECT_NE(items[0]["id"], items[1]["id"]);
}

// ========================================================================
// ResponseStore
// ========================================================================

TEST(ResponseStoreTest, StoreAndRetrieve) {
  ResponseStore store;

  json response = {{"id", "resp_1"}, {"status", "completed"}};
  json input_items = json::array({{{"type", "message"}, {"role", "user"}}});

  store.Store("resp_1", response, input_items);

  auto retrieved = store.Get("resp_1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ((*retrieved)["id"], "resp_1");
}

TEST(ResponseStoreTest, GetReturnsNulloptForMissing) {
  ResponseStore store;

  auto result = store.Get("nonexistent");
  EXPECT_FALSE(result.has_value());
}

TEST(ResponseStoreTest, GetInputItems) {
  ResponseStore store;

  json response = {{"id", "resp_1"}};
  json input_items = json::array({
      {{"type", "message"}, {"role", "user"}, {"content", "hello"}},
  });

  store.Store("resp_1", response, input_items);

  auto items = store.GetInputItems("resp_1");
  ASSERT_TRUE(items.has_value());
  EXPECT_EQ(items->size(), 1u);
  EXPECT_EQ((*items)[0]["content"], "hello");
}

TEST(ResponseStoreTest, DeleteRemovesEntry) {
  ResponseStore store;

  json response = {{"id", "resp_1"}};
  store.Store("resp_1", response, json::array());

  EXPECT_TRUE(store.Delete("resp_1"));
  EXPECT_FALSE(store.Get("resp_1").has_value());
}

TEST(ResponseStoreTest, DeleteReturnsFalseForMissing) {
  ResponseStore store;

  EXPECT_FALSE(store.Delete("nonexistent"));
}

TEST(ResponseStoreTest, ListReturnsAllEntries) {
  ResponseStore store;

  store.Store("resp_1", {{"id", "resp_1"}}, json::array());
  store.Store("resp_2", {{"id", "resp_2"}}, json::array());
  store.Store("resp_3", {{"id", "resp_3"}}, json::array());

  auto all = store.List(10, "", "desc");
  EXPECT_EQ(all.size(), 3u);
}

TEST(ResponseStoreTest, ListRespectsLimit) {
  ResponseStore store;

  store.Store("resp_1", {{"id", "resp_1"}}, json::array());
  store.Store("resp_2", {{"id", "resp_2"}}, json::array());
  store.Store("resp_3", {{"id", "resp_3"}}, json::array());

  auto limited = store.List(2, "", "desc");
  EXPECT_EQ(limited.size(), 2u);
}

TEST(ResponseStoreTest, ListDescOrderReturnsMostRecentFirst) {
  ResponseStore store;

  store.Store("resp_1", {{"id", "resp_1"}}, json::array());
  store.Store("resp_2", {{"id", "resp_2"}}, json::array());
  store.Store("resp_3", {{"id", "resp_3"}}, json::array());

  auto results = store.List(10, "", "desc");
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0]["id"], "resp_3");
  EXPECT_EQ(results[1]["id"], "resp_2");
  EXPECT_EQ(results[2]["id"], "resp_1");
}

TEST(ResponseStoreTest, ListAscOrderReturnsOldestFirst) {
  ResponseStore store;

  store.Store("resp_1", {{"id", "resp_1"}}, json::array());
  store.Store("resp_2", {{"id", "resp_2"}}, json::array());
  store.Store("resp_3", {{"id", "resp_3"}}, json::array());

  auto results = store.List(10, "", "asc");
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0]["id"], "resp_1");
  EXPECT_EQ(results[1]["id"], "resp_2");
  EXPECT_EQ(results[2]["id"], "resp_3");
}

TEST(ResponseStoreTest, ListWithCursorPagination) {
  ResponseStore store;

  store.Store("resp_1", {{"id", "resp_1"}}, json::array());
  store.Store("resp_2", {{"id", "resp_2"}}, json::array());
  store.Store("resp_3", {{"id", "resp_3"}}, json::array());

  // Get items after resp_2 in desc order
  auto results = store.List(10, "resp_2", "desc");
  // In desc order (newest first: 3,2,1), after resp_2 should give resp_1
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0]["id"], "resp_1");
}

TEST(ResponseStoreTest, EvictsOldestWhenCapacityExceeded) {
  ResponseStore store(3);  // Small capacity for testing

  store.Store("resp_1", {{"id", "resp_1"}}, json::array());
  store.Store("resp_2", {{"id", "resp_2"}}, json::array());
  store.Store("resp_3", {{"id", "resp_3"}}, json::array());
  store.Store("resp_4", {{"id", "resp_4"}}, json::array());  // Evicts resp_1

  EXPECT_FALSE(store.Get("resp_1").has_value());
  EXPECT_TRUE(store.Get("resp_2").has_value());
  EXPECT_TRUE(store.Get("resp_3").has_value());
  EXPECT_TRUE(store.Get("resp_4").has_value());
  EXPECT_EQ(store.Size(), 3u);
}

TEST(ResponseStoreTest, SizeTracksEntryCount) {
  ResponseStore store;

  EXPECT_EQ(store.Size(), 0u);

  store.Store("resp_1", {{"id", "resp_1"}}, json::array());
  EXPECT_EQ(store.Size(), 1u);

  store.Store("resp_2", {{"id", "resp_2"}}, json::array());
  EXPECT_EQ(store.Size(), 2u);

  store.Delete("resp_1");
  EXPECT_EQ(store.Size(), 1u);
}
