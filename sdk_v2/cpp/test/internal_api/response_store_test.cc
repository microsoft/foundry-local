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

#include <optional>
#include <set>
#include <string>
#include <vector>

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
  auto session_req = ResponseConverter::ToSessionRequest(params);

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
  auto session_req = ResponseConverter::ToSessionRequest(params);

  // Instructions travel as the request-scoped system prefix, not as a message: a message would be committed to the
  // transcript and replayed on every later turn.
  const char* prefix = session_req.options.Find(kSystemPromptOption);
  ASSERT_NE(prefix, nullptr);
  EXPECT_STREQ(prefix, "You are a helpful robot.");

  ASSERT_EQ(session_req.items.size(), 1u);
  const MessageItem& user_msg = static_cast<const MessageItem&>(*session_req.items[0]);
  EXPECT_EQ(user_msg.role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(user_msg.GetSimpleText(), "Hello");
}

TEST(ResponseConverterTest, ToSessionRequestWithoutInstructionsSetsNoSystemPrefix) {
  json req = {{"model", "test-model"}, {"input", "Hello"}};

  auto params = req.get<ResponseCreateParams>();
  auto session_req = ResponseConverter::ToSessionRequest(params);

  EXPECT_EQ(session_req.options.Find(kSystemPromptOption), nullptr);
}

TEST(ResponseConverterTest, ToSessionRequestWithTemperature) {
  json req = {
      {"model", "test-model"},
      {"input", "test"},
      {"temperature", 0.7},
  };

  auto params = req.get<ResponseCreateParams>();
  auto session_req = ResponseConverter::ToSessionRequest(params);

  EXPECT_STREQ(session_req.options.Find("temperature"), "0.700000");
}

TEST(ResponseConverterTest, ToSessionRequestWithMaxOutputTokens) {
  json req = {
      {"model", "test-model"},
      {"input", "test"},
      {"max_output_tokens", 512},
  };

  auto params = req.get<ResponseCreateParams>();
  auto session_req = ResponseConverter::ToSessionRequest(params);

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
  auto session_req = ResponseConverter::ToSessionRequest(params);

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
  ResponseChainContext previous_context{ResponseChainHop{prev_input, prev_output}};
  auto session_req = ResponseConverter::ToSessionRequest(params, &previous_context);

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

namespace {

/// Store `count` responses named resp_1 .. resp_count, oldest first. ResponseStore owns a mutex and so is neither
/// copyable nor movable; the caller supplies the store.
void FillStore(ResponseStore& store, int count) {
  for (int i = 1; i <= count; ++i) {
    const std::string id = "resp_" + std::to_string(i);
    store.Store(id, {{"id", id}}, json::array());
  }
}

/// The IDs of a page, in the order the store returned them.
std::vector<std::string> PageIds(const ResponseStore::Page& page) {
  std::vector<std::string> ids;
  for (const auto& item : page.data) {
    ids.push_back(item.value("id", ""));
  }

  return ids;
}

}  // namespace

TEST(ResponseStoreTest, ListReturnsAllEntries) {
  ResponseStore store;
  FillStore(store, 3);

  auto all = store.List(10, "", "desc");
  EXPECT_EQ(all.data.size(), 3u);
}

TEST(ResponseStoreTest, ListRespectsLimit) {
  ResponseStore store;
  FillStore(store, 3);

  auto limited = store.List(2, "", "desc");
  EXPECT_EQ(limited.data.size(), 2u);
}

TEST(ResponseStoreTest, ListDescOrderReturnsNewestInsertionFirst) {
  ResponseStore store;
  FillStore(store, 3);

  auto results = store.List(10, "", "desc");
  EXPECT_EQ(PageIds(results), (std::vector<std::string>{"resp_3", "resp_2", "resp_1"}));
}

TEST(ResponseStoreTest, ListAscOrderReturnsOldestFirst) {
  ResponseStore store;
  FillStore(store, 3);

  auto results = store.List(10, "", "asc");
  EXPECT_EQ(PageIds(results), (std::vector<std::string>{"resp_1", "resp_2", "resp_3"}));
}

TEST(ResponseStoreTest, ListWithCursorPagination) {
  ResponseStore store;
  FillStore(store, 3);

  // In desc order (newest first: 3,2,1), after resp_2 should give resp_1.
  auto results = store.List(10, "resp_2", "desc");
  EXPECT_EQ(PageIds(results), (std::vector<std::string>{"resp_1"}));
}

TEST(ResponseStoreTest, CursorTraversalIsStableAcrossReadsAndContinuationTouches) {
  ResponseStore store;
  FillStore(store, 6);

  const auto first = store.List(2, "", "desc");
  ASSERT_EQ(PageIds(first), (std::vector<std::string>{"resp_6", "resp_5"}));
  ASSERT_TRUE(first.has_more);

  ASSERT_TRUE(store.Get("resp_1").has_value());
  ASSERT_TRUE(store.GetInputItems("resp_4").has_value());
  auto continuation = store.BeginResponse("resp_3", "");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  const auto second = store.List(2, "resp_5", "desc");
  ASSERT_EQ(PageIds(second), (std::vector<std::string>{"resp_4", "resp_3"}));
  ASSERT_TRUE(second.has_more);

  ASSERT_TRUE(store.BuildChainContext("resp_2").has_value());
  continuation.lease.Release();

  const auto third = store.List(2, "resp_3", "desc");
  EXPECT_EQ(PageIds(third), (std::vector<std::string>{"resp_2", "resp_1"}));
  EXPECT_FALSE(third.has_more);
}

TEST(ResponseStoreTest, ReadsChangeEvictionRecencyWithoutChangingListOrder) {
  ResponseStore store(3);
  FillStore(store, 3);

  ASSERT_TRUE(store.Get("resp_1").has_value());
  EXPECT_EQ(PageIds(store.List()), (std::vector<std::string>{"resp_3", "resp_2", "resp_1"}));

  store.Store("resp_4", {{"id", "resp_4"}}, json::array());

  EXPECT_FALSE(store.Get("resp_2").has_value()) << "the untouched LRU entry should be evicted";
  EXPECT_EQ(PageIds(store.List()), (std::vector<std::string>{"resp_4", "resp_3", "resp_1"}));
}

// --- has_more: the page must report whether anything actually follows it -----------------------------------------

TEST(ResponseStoreTest, FewerEntriesThanLimitReportsNoMore) {
  ResponseStore store;
  FillStore(store, 2);

  auto page = store.List(5, "", "desc");
  EXPECT_EQ(page.data.size(), 2u);
  EXPECT_FALSE(page.has_more);
}

TEST(ResponseStoreTest, ExactlyLimitEntriesReportsNoMoreInDescOrder) {
  // The false positive this guards: a full final page used to claim a next page that did not exist.
  ResponseStore store;
  FillStore(store, 3);

  auto page = store.List(3, "", "desc");
  EXPECT_EQ(PageIds(page), (std::vector<std::string>{"resp_3", "resp_2", "resp_1"}));
  EXPECT_FALSE(page.has_more);
}

TEST(ResponseStoreTest, ExactlyLimitEntriesReportsNoMoreInAscOrder) {
  ResponseStore store;
  FillStore(store, 3);

  auto page = store.List(3, "", "asc");
  EXPECT_EQ(PageIds(page), (std::vector<std::string>{"resp_1", "resp_2", "resp_3"}));
  EXPECT_FALSE(page.has_more);
}

TEST(ResponseStoreTest, MoreEntriesThanLimitReportsMore) {
  ResponseStore store;
  FillStore(store, 4);

  auto page = store.List(3, "", "desc");
  EXPECT_EQ(PageIds(page), (std::vector<std::string>{"resp_4", "resp_3", "resp_2"}));
  EXPECT_TRUE(page.has_more);
}

TEST(ResponseStoreTest, ExactlyLimitEntriesAfterACursorReportsNoMore) {
  ResponseStore store;
  FillStore(store, 4);

  // desc order is 4,3,2,1; after resp_3 exactly two remain and the page holds both.
  auto page = store.List(2, "resp_3", "desc");
  EXPECT_EQ(PageIds(page), (std::vector<std::string>{"resp_2", "resp_1"}));
  EXPECT_FALSE(page.has_more);
}

TEST(ResponseStoreTest, MoreEntriesAfterACursorReportsMore) {
  ResponseStore store;
  FillStore(store, 4);

  // asc order is 1,2,3,4; after resp_1 three remain and the page holds two of them.
  auto page = store.List(2, "resp_1", "asc");
  EXPECT_EQ(PageIds(page), (std::vector<std::string>{"resp_2", "resp_3"}));
  EXPECT_TRUE(page.has_more);
}

TEST(ResponseStoreTest, CursorAtTheEndReturnsAnEmptyFinalPage) {
  ResponseStore store;
  FillStore(store, 3);

  auto page = store.List(2, "resp_1", "desc");
  EXPECT_TRUE(page.data.empty());
  EXPECT_FALSE(page.has_more);
}

TEST(ResponseStoreTest, EmptyStoreReportsNoMore) {
  ResponseStore store;

  auto page = store.List(10, "", "desc");
  EXPECT_TRUE(page.data.empty());
  EXPECT_FALSE(page.has_more);
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

// ========================================================================
// BuildChainContext — complete-chain reconstruction
//
// Each entry stores only its own request's input items (that is what the
// /input_items endpoint returns), so continuing a conversation after the
// session cache has dropped it requires walking the whole chain.
// ========================================================================

namespace {

/// Store one hop of a conversation: its own request input items and the response it produced.
void StoreHop(ResponseStore& store, const std::string& id, const std::string& previous_id,
              const json& input_items, const json& output) {
  json response;
  response["id"] = id;
  response["previous_response_id"] = previous_id.empty() ? json(nullptr) : json(previous_id);
  response["output"] = output;
  store.Store(id, std::move(response), input_items);
}

}  // namespace

TEST(ResponseStoreChainTest, ReconstructsToolCallAndResultAcrossMultipleHops) {
  ResponseStore store;

  // resp_1: user asks; the model answers with a tool call.
  StoreHop(store, "resp_1", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "weather?"}}}),
           json::array({{{"type", "function_call"},
                         {"call_id", "call_1"},
                         {"name", "get_weather"},
                         {"arguments", R"({"city":"Seattle"})"}}}));

  // resp_2: caller supplies the tool result; the model answers in text.
  StoreHop(store, "resp_2", "resp_1",
           json::array({{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "sunny"}}}),
           json::array({{{"type", "message"}, {"role", "assistant"}, {"content", "It is sunny."}}}));

  auto context = store.BuildChainContext("resp_2");

  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 2u);

  // Oldest hop first, and each hop keeps its own input and output apart so replay can rebuild one assistant turn
  // per hop — the tool call therefore still precedes the result that answers it.
  ASSERT_EQ((*context)[0].input_items.size(), 1u);
  EXPECT_EQ((*context)[0].input_items[0]["role"], "user");
  ASSERT_EQ((*context)[0].output_items.size(), 1u);
  EXPECT_EQ((*context)[0].output_items[0]["type"], "function_call");
  EXPECT_EQ((*context)[0].output_items[0]["call_id"], "call_1");

  ASSERT_EQ((*context)[1].input_items.size(), 1u);
  EXPECT_EQ((*context)[1].input_items[0]["type"], "function_call_output");
  EXPECT_EQ((*context)[1].input_items[0]["call_id"], "call_1");
  ASSERT_EQ((*context)[1].output_items.size(), 1u);
  EXPECT_EQ((*context)[1].output_items[0]["role"], "assistant");
}

TEST(ResponseStoreChainTest, TouchingAChainChangesEvictionRecencyWithoutChangingListOrder) {
  ResponseStore store(3);
  StoreHop(store, "resp_root", "", json::array(), json::array());
  StoreHop(store, "resp_tip", "resp_root", json::array(), json::array());
  StoreHop(store, "resp_other", "", json::array(), json::array());

  // Opening a continuation refreshes the whole chain without materializing it — what a caller that continues from a
  // live session relies on.
  ASSERT_EQ(store.BeginResponse("resp_tip", "").status, ContinuationStatus::kOk);
  EXPECT_EQ(PageIds(store.List(3, "", "desc")),
            (std::vector<std::string>{"resp_other", "resp_tip", "resp_root"}));

  StoreHop(store, "resp_new", "", json::array(), json::array());

  EXPECT_TRUE(store.Get("resp_tip").has_value());
  EXPECT_TRUE(store.Get("resp_root").has_value());
  EXPECT_FALSE(store.Get("resp_other").has_value());
}

TEST(ResponseStoreChainTest, RebuildingAChainChangesEvictionRecencyWithoutChangingListOrder) {
  ResponseStore store(3);
  StoreHop(store, "resp_root", "", json::array(), json::array());
  StoreHop(store, "resp_tip", "resp_root", json::array(), json::array());
  StoreHop(store, "resp_other", "", json::array(), json::array());

  ASSERT_TRUE(store.BuildChainContext("resp_tip").has_value());
  EXPECT_EQ(PageIds(store.List(3, "", "desc")),
            (std::vector<std::string>{"resp_other", "resp_tip", "resp_root"}));
}

TEST(ResponseStoreChainTest, AConversationLongerThanCapacityStillReconstructsFromItsRetainedTip) {
  ResponseStore store;
  constexpr int kHopCount = ResponseStore::kDefaultCapacity + 5;

  std::string previous_id;
  for (int i = 1; i <= kHopCount; ++i) {
    const std::string id = "resp_" + std::to_string(i);
    StoreHop(store, id, previous_id,
             json::array({{{"type", "message"}, {"role", "user"}, {"content", "input_" + std::to_string(i)}}}),
             json::array({{{"type", "message"},
                           {"role", "assistant"},
                           {"content", "output_" + std::to_string(i)}}}));
    previous_id = id;
  }

  EXPECT_EQ(store.Size(), static_cast<size_t>(ResponseStore::kDefaultCapacity));
  EXPECT_FALSE(store.Get("resp_1").has_value());
  EXPECT_FALSE(store.Get("resp_5").has_value());

  auto context = store.BuildChainContext(previous_id);
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), static_cast<size_t>(kHopCount));
  for (int i = 1; i <= kHopCount; ++i) {
    EXPECT_EQ((*context)[i - 1].input_items[0]["content"], "input_" + std::to_string(i));
    EXPECT_EQ((*context)[i - 1].output_items[0]["content"], "output_" + std::to_string(i));
  }
}

TEST(ResponseStoreChainTest, CompactedPrefixesAreSharedAcrossBranches) {
  ResponseStore store(3);
  StoreHop(store, "resp_root", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "root"}}}), json::array());
  StoreHop(store, "resp_left", "resp_root",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "left"}}}), json::array());
  StoreHop(store, "resp_right", "resp_root",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "right"}}}), json::array());
  StoreHop(store, "resp_other", "", json::array(), json::array());

  EXPECT_FALSE(store.Get("resp_root").has_value());

  auto left = store.BuildChainContext("resp_left");
  auto right = store.BuildChainContext("resp_right");
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());
  ASSERT_EQ(left->size(), 2u);
  ASSERT_EQ(right->size(), 2u);
  EXPECT_EQ((*left)[0].input_items[0]["content"], "root");
  EXPECT_EQ((*left)[1].input_items[0]["content"], "left");
  EXPECT_EQ((*right)[0].input_items[0]["content"], "root");
  EXPECT_EQ((*right)[1].input_items[0]["content"], "right");
}

TEST(ResponseStoreChainTest, RepeatedCompactionExtendsOneBranchWithoutChangingItsSibling) {
  ResponseStore store(3);
  StoreHop(store, "resp_root", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "root"}}}), json::array());
  StoreHop(store, "resp_left", "resp_root",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "left"}}}), json::array());
  StoreHop(store, "resp_right", "resp_root",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "right"}}}), json::array());
  StoreHop(store, "resp_left_child", "resp_left",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "left_child"}}}), json::array());
  StoreHop(store, "resp_other", "", json::array(), json::array());

  auto left = store.BuildChainContext("resp_left_child");
  auto right = store.BuildChainContext("resp_right");
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());
  ASSERT_EQ(left->size(), 3u);
  ASSERT_EQ(right->size(), 2u);
  EXPECT_EQ((*left)[0].input_items[0]["content"], "root");
  EXPECT_EQ((*left)[1].input_items[0]["content"], "left");
  EXPECT_EQ((*left)[2].input_items[0]["content"], "left_child");
  EXPECT_EQ((*right)[0].input_items[0]["content"], "root");
  EXPECT_EQ((*right)[1].input_items[0]["content"], "right");
}

TEST(ResponseStoreChainTest, ReplacingARetainedHopPreservesItsCompactedAncestry) {
  ResponseStore store(2);
  StoreHop(store, "resp_root", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "root"}}}), json::array());
  StoreHop(store, "resp_mid", "resp_root",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "old"}}}), json::array());
  StoreHop(store, "resp_other", "", json::array(), json::array());

  StoreHop(store, "resp_mid", "resp_root",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "new"}}}), json::array());

  auto context = store.BuildChainContext("resp_mid");
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 2u);
  EXPECT_EQ((*context)[0].input_items[0]["content"], "root");
  EXPECT_EQ((*context)[1].input_items[0]["content"], "new");
}

TEST(ResponseStoreChainTest, ReparentingARetainedHopDropsItsOldCompactedAncestry) {
  ResponseStore store(3);
  StoreHop(store, "old_root", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "old_root"}}}), json::array());
  StoreHop(store, "moving", "old_root",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "old_child"}}}), json::array());
  StoreHop(store, "new_root", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "new_root"}}}), json::array());
  StoreHop(store, "other", "", json::array(), json::array());

  StoreHop(store, "moving", "new_root",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "new_child"}}}), json::array());

  auto context = store.BuildChainContext("moving");
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 2u);
  EXPECT_EQ((*context)[0].input_items[0]["content"], "new_root");
  EXPECT_EQ((*context)[1].input_items[0]["content"], "new_child");
}

TEST(ResponseStoreChainTest, DeletingACompactedHopPurgesEveryDependentBranch) {
  ResponseStore store(3);
  StoreHop(store, "resp_root", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "secret"}}}), json::array());
  StoreHop(store, "resp_left", "resp_root", json::array(), json::array());
  StoreHop(store, "resp_right", "resp_root", json::array(), json::array());
  StoreHop(store, "resp_unrelated", "", json::array(), json::array());

  EXPECT_FALSE(store.Get("resp_root").has_value());
  auto deleted = store.DeleteWithDependents("resp_root");
  EXPECT_EQ(deleted, (std::vector<std::string>{"resp_right", "resp_left", "resp_root"}));
  EXPECT_FALSE(store.Get("resp_left").has_value());
  EXPECT_FALSE(store.Get("resp_right").has_value());
  EXPECT_TRUE(store.Get("resp_unrelated").has_value());
}

TEST(ResponseStoreChainTest, DeletingAResidentAncestorReturnsAndPurgesEveryDependentId) {
  ResponseStore store;
  StoreHop(store, "resp_root", "", json::array(), json::array());
  StoreHop(store, "resp_middle", "resp_root", json::array(), json::array());
  StoreHop(store, "resp_tip", "resp_middle", json::array(), json::array());

  auto deleted = store.DeleteWithDependents("resp_middle");

  EXPECT_EQ(deleted, (std::vector<std::string>{"resp_tip", "resp_middle"}));
  EXPECT_TRUE(store.Get("resp_root").has_value());
  EXPECT_FALSE(store.Get("resp_middle").has_value());
  EXPECT_FALSE(store.Get("resp_tip").has_value());
}

TEST(ResponseStoreChainTest, EvictionWalksPastANonRootLruEntry) {
  ResponseStore store(3);
  StoreHop(store, "resp_root",
           "", json::array({{{"type", "message"}, {"role", "user"}, {"content", "root"}}}), json::array());
  StoreHop(store, "resp_child", "resp_root",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "child"}}}), json::array());
  ASSERT_TRUE(store.Get("resp_root").has_value());
  StoreHop(store, "other_1", "", json::array(), json::array());
  StoreHop(store, "other_2", "", json::array(), json::array());

  EXPECT_FALSE(store.Get("resp_root").has_value());
  auto context = store.BuildChainContext("resp_child");
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 2u);
  EXPECT_EQ((*context)[0].input_items[0]["content"], "root");
  EXPECT_EQ((*context)[1].input_items[0]["content"], "child");
}

TEST(ResponseStoreChainTest, CyclicEntriesCannotMonopolizeCapacity) {
  ResponseStore store(2);
  StoreHop(store, "cycle_a", "cycle_b", json::array(), json::array());
  StoreHop(store, "cycle_b", "cycle_a", json::array(), json::array());
  StoreHop(store, "healthy", "", json::array(), json::array());

  EXPECT_EQ(store.Size(), 2u);
  EXPECT_TRUE(store.Get("healthy").has_value());
  EXPECT_FALSE(store.BuildChainContext("cycle_b").has_value());
}

TEST(ResponseStoreChainTest, SingleHopChainReturnsItsOwnInputAndOutput) {
  ResponseStore store;
  StoreHop(store, "resp_1", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "hello"}}}),
           json::array({{{"type", "message"}, {"role", "assistant"}, {"content", "hi"}}}));

  auto context = store.BuildChainContext("resp_1");

  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 1u);
  ASSERT_EQ((*context)[0].input_items.size(), 1u);
  EXPECT_EQ((*context)[0].input_items[0]["role"], "user");
  ASSERT_EQ((*context)[0].output_items.size(), 1u);
  EXPECT_EQ((*context)[0].output_items[0]["role"], "assistant");
}

TEST(ResponseStoreChainTest, EmptyOutputStillReportsTheHopThatProducedIt) {
  // A hop whose output was reasoning-only or truncated before any visible text still happened. Reporting the hop
  // with an empty output array is what lets replay rebuild the assistant boundary the live session committed.
  ResponseStore store;
  StoreHop(store, "resp_1", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "hello"}}}), json::array());

  auto context = store.BuildChainContext("resp_1");

  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 1u);
  EXPECT_EQ((*context)[0].input_items.size(), 1u);
  EXPECT_TRUE((*context)[0].output_items.is_array());
  EXPECT_TRUE((*context)[0].output_items.empty());
}

TEST(ResponseStoreChainTest, MissingLinkCannotBeReconstructed) {
  ResponseStore store;
  // resp_2 points at a root that is no longer stored (evicted, or from a previous process).
  StoreHop(store, "resp_2", "resp_1",
           json::array({{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "sunny"}}}),
           json::array());

  EXPECT_FALSE(store.BuildChainContext("resp_2").has_value());
}

TEST(ResponseStoreChainTest, EvictingABrokenRootDoesNotTurnItsDescendantIntoATruncatedValidChain) {
  ResponseStore store(2);
  StoreHop(store, "resp_2", "missing_resp_1", json::array(), json::array());
  StoreHop(store, "resp_3", "resp_2", json::array(), json::array());
  StoreHop(store, "resp_other", "", json::array(), json::array());

  EXPECT_FALSE(store.Get("resp_2").has_value());
  EXPECT_FALSE(store.BuildChainContext("resp_3").has_value());
}

TEST(ResponseStoreChainTest, UnknownResponseCannotBeReconstructed) {
  ResponseStore store;

  EXPECT_FALSE(store.BuildChainContext("resp_missing").has_value());
}

TEST(ResponseStoreChainTest, CyclicChainCannotBeReconstructed) {
  ResponseStore store;
  StoreHop(store, "resp_1", "resp_2", json::array(), json::array());
  StoreHop(store, "resp_2", "resp_1", json::array(), json::array());

  EXPECT_FALSE(store.BuildChainContext("resp_2").has_value());
}

TEST(ResponseStoreChainTest, ReconstructionDoesNotChangeInputItemsEndpointSemantics) {
  ResponseStore store;
  StoreHop(store, "resp_1", "",
           json::array({{{"type", "message"}, {"role", "user"}, {"content", "weather?"}}}),
           json::array({{{"type", "function_call"}, {"call_id", "call_1"}, {"name", "get_weather"}}}));
  StoreHop(store, "resp_2", "resp_1",
           json::array({{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "sunny"}}}),
           json::array());

  ASSERT_TRUE(store.BuildChainContext("resp_2").has_value());

  // /input_items must still report only the request's own items, not the reconstructed chain.
  auto items = store.GetInputItems("resp_2");
  ASSERT_TRUE(items.has_value());
  ASSERT_EQ(items->size(), 1u);
  EXPECT_EQ((*items)[0]["type"], "function_call_output");
}

TEST(ResponseStoreChainTest, EveryStoredSystemMessageIsReplayedVerbatim) {
  // Instructions are no longer stored as items, so the store never has to guess which system message was ours.
  // Every system message the caller sent survives reconstruction, including duplicates and content-part forms.
  ResponseStore store;

  json response;
  response["id"] = "resp_1";
  response["previous_response_id"] = nullptr;
  response["instructions"] = "Be terse.";
  response["output"] = json::array();

  store.Store("resp_1", response,
              json::array({{{"type", "message"}, {"role", "system"}, {"content", "Be terse."}},
                           {{"type", "message"}, {"role", "user"}, {"content", "hello"}},
                           {{"type", "message"}, {"role", "system"}, {"content", "Be terse."}},
                           {{"type", "message"},
                            {"role", "system"},
                            {"content", json::array({{{"type", "input_text"}, {"text", "Be terse."}}})}}}));

  auto context = store.BuildChainContext("resp_1");
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 1u);

  const auto& replayed = (*context)[0].input_items;
  ASSERT_EQ(replayed.size(), 4u);
  EXPECT_EQ(replayed[0]["content"], "Be terse.");
  EXPECT_EQ(replayed[1]["content"], "hello");
  EXPECT_EQ(replayed[2]["content"], "Be terse.");
  EXPECT_TRUE(replayed[3]["content"].is_array());
  EXPECT_EQ(replayed[3]["content"][0]["text"], "Be terse.");
}

TEST(ResponseStoreChainTest, StoredInputItemsAreReturnedUnchangedAcrossHops) {
  ResponseStore store;

  StoreHop(store, "resp_1", "", json::array({{{"type", "message"}, {"role", "user"}, {"content", "hello"}}}),
           json::array({{{"type", "message"}, {"role", "assistant"}, {"content", "ok"}}}));
  StoreHop(store, "resp_2", "resp_1", json::array({{{"type", "message"}, {"role", "user"}, {"content", "again"}}}),
           json::array({{{"type", "message"}, {"role", "assistant"}, {"content", "sure"}}}));

  auto context = store.BuildChainContext("resp_2");
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 2u);

  EXPECT_EQ((*context)[0].input_items, *store.GetInputItems("resp_1"));
  EXPECT_EQ((*context)[1].input_items, *store.GetInputItems("resp_2"));
  EXPECT_EQ((*context)[0].output_items[0]["content"], "ok");
  EXPECT_EQ((*context)[1].output_items[0]["content"], "sure");
}

TEST(ResponseStoreChainTest, NonArrayStoredInputItemsBecomeAnEmptyHopInput) {
  // Stored entries are arbitrary JSON. A malformed input-items field must not throw or leak a non-array into replay.
  ResponseStore store;

  json response;
  response["id"] = "resp_1";
  response["previous_response_id"] = nullptr;
  response["output"] = json("not-an-array");
  store.Store("resp_1", response, json("not-an-array"));

  std::optional<ResponseChainContext> context;
  ASSERT_NO_THROW(context = store.BuildChainContext("resp_1"));
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 1u);
  EXPECT_TRUE((*context)[0].input_items.is_array());
  EXPECT_TRUE((*context)[0].input_items.empty());
  EXPECT_TRUE((*context)[0].output_items.is_array());
  EXPECT_TRUE((*context)[0].output_items.empty());
}

TEST(ResponseStoreChainTest, OpeningAContinuationKeepsAnActiveConversationResident) {
  ResponseStore store(3);

  StoreHop(store, "resp_1", "", json::array({{{"type", "message"}, {"role", "user"}, {"content", "one"}}}),
           json::array());
  StoreHop(store, "resp_2", "resp_1", json::array({{{"type", "message"}, {"role", "user"}, {"content", "two"}}}),
           json::array());

  // Unrelated traffic would otherwise evict the conversation's root before it is ever replayed.
  EXPECT_EQ(store.BeginResponse("resp_2", "").status, ContinuationStatus::kOk);
  StoreHop(store, "other_1", "", json::array(), json::array());

  auto context = store.BuildChainContext("resp_2");
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 2u);
  EXPECT_EQ((*context)[0].input_items[0]["content"], "one");
}

TEST(ResponseStoreChainTest, OpeningAContinuationReportsABrokenChain) {
  ResponseStore store;
  StoreHop(store, "resp_2", "resp_1", json::array(), json::array());

  EXPECT_EQ(store.BeginResponse("resp_2", "").status, ContinuationStatus::kChainUnavailable);
  EXPECT_EQ(store.BeginResponse("resp_missing", "").status, ContinuationStatus::kChainUnavailable);
}

TEST(ResponseStoreChainTest, NonStringRolesAndContentDoNotBreakReconstruction) {
  ResponseStore store;

  json response;
  response["id"] = "resp_1";
  response["previous_response_id"] = nullptr;
  response["instructions"] = "Be terse.";
  response["output"] = json::array();

  // Stored items are arbitrary caller input. Reconstruction copies them through untouched — it never inspects a
  // role or a content shape, so no stored shape can make it throw or drop an item.
  store.Store("resp_1", response,
              json::array({json::array({1, 2}),
                           {{"role", 7}, {"content", "Be terse."}},
                           {{"role", "system"}, {"content", 42}},
                           {{"role", "system"}, {"content", "Be terse."}}}));

  std::optional<ResponseChainContext> context;
  ASSERT_NO_THROW(context = store.BuildChainContext("resp_1"));
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 1u);

  EXPECT_EQ((*context)[0].input_items.size(), 4u);
}
