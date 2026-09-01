// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for chat_completions_converter.cc — pure conversion functions that
// map between OpenAI ChatCompletions contract types and internal session types.
//
#include "contracts/chat_completions_converter.h"

#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

using namespace fl;
using namespace fl::chat_completions;
using json = nlohmann::json;

// ========================================================================
// GenerateCompletionId
// ========================================================================

TEST(ChatCompletionsConverterTest, GenerateCompletionId_HasExpectedPrefix) {
  std::string id = GenerateCompletionId();
  EXPECT_EQ(id.substr(0, 9), "chatcmpl-");
  EXPECT_GT(id.size(), 9u);  // must have random hex after prefix
}

TEST(ChatCompletionsConverterTest, GenerateCompletionId_UniqueAcrossCalls) {
  std::string id1 = GenerateCompletionId();
  std::string id2 = GenerateCompletionId();
  EXPECT_NE(id1, id2);
}

// ========================================================================
// ApplyCatalogDefaults
// ========================================================================

TEST(ChatCompletionsConverterTest, ApplyCatalogDefaults_EmptySettings_NoChange) {
  ChatCompletionRequest req;
  req.temperature = 0.5f;
  KeyValuePairs settings;

  ApplyCatalogDefaults(req, settings);

  ASSERT_TRUE(req.temperature.has_value());
  EXPECT_FLOAT_EQ(*req.temperature, 0.5f);
}

TEST(ChatCompletionsConverterTest, ApplyCatalogDefaults_AppliesFloatsWhenNotSet) {
  ChatCompletionRequest req;
  // Leave all optional fields unset

  KeyValuePairs settings;
  settings.Add("temperature", "0.7");
  settings.Add("top_p", "0.9");
  settings.Add("presence_penalty", "0.1");
  settings.Add("frequency_penalty", "0.2");

  ApplyCatalogDefaults(req, settings);

  ASSERT_TRUE(req.temperature.has_value());
  EXPECT_FLOAT_EQ(*req.temperature, 0.7f);

  ASSERT_TRUE(req.top_p.has_value());
  EXPECT_FLOAT_EQ(*req.top_p, 0.9f);

  ASSERT_TRUE(req.presence_penalty.has_value());
  EXPECT_FLOAT_EQ(*req.presence_penalty, 0.1f);

  ASSERT_TRUE(req.frequency_penalty.has_value());
  EXPECT_FLOAT_EQ(*req.frequency_penalty, 0.2f);
}

TEST(ChatCompletionsConverterTest, ApplyCatalogDefaults_DoesNotOverrideExisting) {
  ChatCompletionRequest req;
  req.temperature = 0.3f;
  req.top_p = 0.5f;

  KeyValuePairs settings;
  settings.Add("temperature", "0.7");
  settings.Add("top_p", "0.9");

  ApplyCatalogDefaults(req, settings);

  // Should keep user-provided values, not override with catalog defaults
  EXPECT_FLOAT_EQ(*req.temperature, 0.3f);
  EXPECT_FLOAT_EQ(*req.top_p, 0.5f);
}

TEST(ChatCompletionsConverterTest, ApplyCatalogDefaults_AppliesMaxTokens) {
  ChatCompletionRequest req;

  KeyValuePairs settings;
  settings.Add("max_tokens", "1024");

  ApplyCatalogDefaults(req, settings);

  ASSERT_TRUE(req.max_tokens.has_value());
  EXPECT_EQ(*req.max_tokens, 1024);
}

TEST(ChatCompletionsConverterTest, ApplyCatalogDefaults_AppliesMetadata) {
  ChatCompletionRequest req;
  // No metadata set yet

  KeyValuePairs settings;
  settings.Add("top_k", "40");
  settings.Add("random_seed", "42");

  ApplyCatalogDefaults(req, settings);

  ASSERT_TRUE(req.metadata.has_value());
  EXPECT_EQ(req.metadata->at("top_k"), "40");
  EXPECT_EQ(req.metadata->at("random_seed"), "42");
}

TEST(ChatCompletionsConverterTest, ApplyCatalogDefaults_MetadataDoesNotOverrideExisting) {
  ChatCompletionRequest req;
  req.metadata = std::map<std::string, std::string>{{"top_k", "10"}};

  KeyValuePairs settings;
  settings.Add("top_k", "40");
  settings.Add("random_seed", "42");

  ApplyCatalogDefaults(req, settings);

  // top_k should keep user value, random_seed should be added
  EXPECT_EQ(req.metadata->at("top_k"), "10");
  EXPECT_EQ(req.metadata->at("random_seed"), "42");
}

// ========================================================================
// MapFinishReason
// ========================================================================

TEST(ChatCompletionsConverterTest, MapFinishReason_Stop) {
  EXPECT_EQ(MapFinishReason(FOUNDRY_LOCAL_FINISH_STOP), "stop");
}

TEST(ChatCompletionsConverterTest, MapFinishReason_Length) {
  EXPECT_EQ(MapFinishReason(FOUNDRY_LOCAL_FINISH_LENGTH), "length");
}

TEST(ChatCompletionsConverterTest, MapFinishReason_ToolCalls) {
  EXPECT_EQ(MapFinishReason(FOUNDRY_LOCAL_FINISH_TOOL_CALLS), "tool_calls");
}

TEST(ChatCompletionsConverterTest, MapFinishReason_UnknownDefaultsToStop) {
  EXPECT_EQ(MapFinishReason(FOUNDRY_LOCAL_FINISH_NONE), "stop");
}

// ========================================================================
// BuildRequestItems
// ========================================================================

TEST(ChatCompletionsConverterTest, BuildRequestItems_UserAndSystemMessages) {
  ChatCompletionRequest req;
  req.messages.push_back({"system", "You are helpful", {}, {}, {}});
  req.messages.push_back({"user", "Hello", {}, {}, {}});

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 2u);

  auto* sys = static_cast<MessageItem*>(session_request.items[0]);
  EXPECT_EQ(sys->role, FOUNDRY_LOCAL_ROLE_SYSTEM);
  EXPECT_EQ(sys->GetSimpleText(), "You are helpful");

  auto* usr = static_cast<MessageItem*>(session_request.items[1]);
  EXPECT_EQ(usr->role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(usr->GetSimpleText(), "Hello");
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_SkipsEmptyContent) {
  ChatCompletionRequest req;
  req.messages.push_back({"user", std::nullopt, {}, {}, {}});     // null content
  req.messages.push_back({"user", std::string(""), {}, {}, {}});  // empty string
  req.messages.push_back({"user", std::string("Real message"), {}, {}, {}});

  Request session_request;
  BuildRequestItems(req, session_request);

  // Only the non-empty message should be added
  ASSERT_EQ(session_request.items.size(), 1u);
  auto* msg = static_cast<MessageItem*>(session_request.items[0]);
  EXPECT_EQ(msg->GetSimpleText(), "Real message");
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_ToolRoleCreatesToolResultItem) {
  ChatCompletionRequest req;
  ChatCompletionMessage tool_msg;
  tool_msg.role = "tool";
  tool_msg.content = "The weather is sunny";
  tool_msg.tool_call_id = "call_abc123";
  req.messages.push_back(tool_msg);

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 1u);
  EXPECT_EQ(session_request.items[0]->type, FOUNDRY_LOCAL_ITEM_TOOL_RESULT);

  auto* tr = static_cast<ToolResultItem*>(session_request.items[0]);
  EXPECT_EQ(tr->call_id, "call_abc123");
  EXPECT_EQ(tr->result, "The weather is sunny");
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_ToolRoleMissingCallId) {
  ChatCompletionRequest req;
  ChatCompletionMessage tool_msg;
  tool_msg.role = "tool";
  tool_msg.content = "result";
  // tool_call_id is not set → value_or("") should produce empty string
  req.messages.push_back(tool_msg);

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 1u);
  auto* tr = static_cast<ToolResultItem*>(session_request.items[0]);
  EXPECT_EQ(tr->call_id, "");
}

// ========================================================================
// ExtractToolDefinitions
// ========================================================================

TEST(ChatCompletionsConverterTest, ExtractToolDefinitions_NoTools_ReturnsEmpty) {
  ChatCompletionRequest req;
  Request session_request;

  std::string tools_json = ExtractToolDefinitions(req, session_request);

  EXPECT_TRUE(tools_json.empty());
}

TEST(ChatCompletionsConverterTest, ExtractToolDefinitions_WithTools_ReturnsSerializedJson) {
  ChatCompletionRequest req;
  ChatCompletionTool tool;
  tool.type = "function";
  tool.function.name = "get_weather";
  tool.function.description = "Get weather for a city";
  req.tools = std::vector<ChatCompletionTool>{tool};

  Request session_request;
  std::string tools_json = ExtractToolDefinitions(req, session_request);

  EXPECT_FALSE(tools_json.empty());
  auto parsed = json::parse(tools_json);
  ASSERT_TRUE(parsed.is_array());
  EXPECT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0]["function"]["name"], "get_weather");
}

TEST(ChatCompletionsConverterTest, ExtractToolDefinitions_ToolChoiceString_SetsOption) {
  ChatCompletionRequest req;
  req.tool_choice = json("auto");

  Request session_request;
  ExtractToolDefinitions(req, session_request);

  auto it = session_request.options.find("tool_choice");
  ASSERT_NE(it, session_request.options.Entries().end());
  EXPECT_EQ(it->second, "auto");
}

TEST(ChatCompletionsConverterTest, ExtractToolDefinitions_ToolChoiceNone_SetsOption) {
  ChatCompletionRequest req;
  req.tool_choice = json("none");

  Request session_request;
  ExtractToolDefinitions(req, session_request);

  auto it = session_request.options.find("tool_choice");
  ASSERT_NE(it, session_request.options.Entries().end());
  EXPECT_EQ(it->second, "none");
}

TEST(ChatCompletionsConverterTest, ExtractToolDefinitions_ToolChoiceObject_FiltersToNamedFunction) {
  // Set up two tools, then use tool_choice to target one
  ChatCompletionRequest req;

  ChatCompletionTool tool1;
  tool1.type = "function";
  tool1.function.name = "get_weather";
  tool1.function.description = "Get weather";

  ChatCompletionTool tool2;
  tool2.type = "function";
  tool2.function.name = "get_time";
  tool2.function.description = "Get time";

  req.tools = std::vector<ChatCompletionTool>{tool1, tool2};
  req.tool_choice = json::parse(R"({"type": "function", "function": {"name": "get_weather"}})");

  Request session_request;
  std::string tools_json = ExtractToolDefinitions(req, session_request);

  // tool_choice should be "required"
  auto it = session_request.options.find("tool_choice");
  ASSERT_NE(it, session_request.options.Entries().end());
  EXPECT_EQ(it->second, "required");

  // tools_json should contain only get_weather, not get_time
  auto parsed = json::parse(tools_json);
  ASSERT_TRUE(parsed.is_array());
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0]["function"]["name"], "get_weather");
}

TEST(ChatCompletionsConverterTest, ExtractToolDefinitions_ToolChoiceObject_NoMatchingTool) {
  ChatCompletionRequest req;

  ChatCompletionTool tool1;
  tool1.type = "function";
  tool1.function.name = "get_weather";
  req.tools = std::vector<ChatCompletionTool>{tool1};

  // Target a function that doesn't exist in the tools list
  req.tool_choice = json::parse(R"({"type": "function", "function": {"name": "nonexistent"}})");

  Request session_request;
  std::string tools_json = ExtractToolDefinitions(req, session_request);

  // tool_choice should still be "required"
  auto it = session_request.options.find("tool_choice");
  ASSERT_NE(it, session_request.options.Entries().end());
  EXPECT_EQ(it->second, "required");

  // tools_json should be the original serialization (filtered was empty, so no override)
  auto parsed = json::parse(tools_json);
  ASSERT_TRUE(parsed.is_array());
  EXPECT_EQ(parsed.size(), 1u);
}

// ========================================================================
// MapRequestParameters
// ========================================================================

TEST(ChatCompletionsConverterTest, MapRequestParameters_AllFloatParams) {
  ChatCompletionRequest req;
  req.temperature = 0.7f;
  req.top_p = 0.9f;
  req.frequency_penalty = 0.5f;
  req.presence_penalty = 0.3f;

  Request session_request;
  MapRequestParameters(req, session_request);

  EXPECT_NE(session_request.options.Find("temperature"), nullptr);
  EXPECT_NE(session_request.options.Find("top_p"), nullptr);
  EXPECT_NE(session_request.options.Find("frequency_penalty"), nullptr);
  EXPECT_NE(session_request.options.Find("presence_penalty"), nullptr);
}

TEST(ChatCompletionsConverterTest, MapRequestParameters_Seed) {
  ChatCompletionRequest req;
  req.seed = 42;

  Request session_request;
  MapRequestParameters(req, session_request);

  EXPECT_STREQ(session_request.options.Find("seed"), "42");
}

TEST(ChatCompletionsConverterTest, MapRequestParameters_MaxCompletionTokensTakesPrecedence) {
  ChatCompletionRequest req;
  req.max_completion_tokens = 500;
  req.max_tokens = 100;  // deprecated, should be ignored when max_completion_tokens is set

  Request session_request;
  MapRequestParameters(req, session_request);

  EXPECT_STREQ(session_request.options.Find("max_output_tokens"), "500");
}

TEST(ChatCompletionsConverterTest, MapRequestParameters_MaxTokensFallback) {
  ChatCompletionRequest req;
  // max_completion_tokens not set
  req.max_tokens = 256;

  Request session_request;
  MapRequestParameters(req, session_request);

  EXPECT_STREQ(session_request.options.Find("max_output_tokens"), "256");
}

TEST(ChatCompletionsConverterTest, MapRequestParameters_MetadataTopKAndRandomSeed) {
  ChatCompletionRequest req;
  req.metadata = std::map<std::string, std::string>{
      {"top_k", "40"},
      {"random_seed", "123"}};

  Request session_request;
  MapRequestParameters(req, session_request);

  EXPECT_STREQ(session_request.options.Find("top_k"), "40");
  EXPECT_STREQ(session_request.options.Find("seed"), "123");
}

TEST(ChatCompletionsConverterTest, MapRequestParameters_EmptyMetadataValuesIgnored) {
  ChatCompletionRequest req;
  req.metadata = std::map<std::string, std::string>{
      {"top_k", ""},
      {"random_seed", ""}};

  Request session_request;
  MapRequestParameters(req, session_request);

  EXPECT_EQ(session_request.options.Find("top_k"), nullptr);
  EXPECT_EQ(session_request.options.Find("seed"), nullptr);
}

// ========================================================================
// MapGuidance
// ========================================================================

TEST(ChatCompletionsConverterTest, MapGuidance_NoResponseFormat_NoOp) {
  ChatCompletionRequest req;

  Request session_request;
  MapGuidance(req, session_request);

  EXPECT_EQ(session_request.options.Find("guidance_type"), nullptr);
}

TEST(ChatCompletionsConverterTest, MapGuidance_LarkGrammar) {
  ChatCompletionRequest req;
  req.response_format = json::parse(R"({"type": "lark_grammar", "lark_grammar": "start: WORD+"})");

  Request session_request;
  MapGuidance(req, session_request);

  EXPECT_STREQ(session_request.options.Find("guidance_type"), "lark_grammar");
  EXPECT_STREQ(session_request.options.Find("guidance_data"), "start: WORD+");
}

TEST(ChatCompletionsConverterTest, MapGuidance_JsonSchema) {
  ChatCompletionRequest req;
  auto schema = json::parse(R"({"type": "object", "properties": {"name": {"type": "string"}}})");
  req.response_format = json{{"type", "json_schema"}, {"json_schema", schema}};

  Request session_request;
  MapGuidance(req, session_request);

  EXPECT_STREQ(session_request.options.Find("guidance_type"), "json_schema");
  // guidance_data should be the dumped json_schema value
  auto guidance_data = json::parse(session_request.options.Find("guidance_data"));
  EXPECT_EQ(guidance_data["type"], "object");
}

TEST(ChatCompletionsConverterTest, MapGuidance_JsonObject) {
  ChatCompletionRequest req;
  req.response_format = json{{"type", "json_object"}};

  Request session_request;
  MapGuidance(req, session_request);

  EXPECT_STREQ(session_request.options.Find("guidance_type"), "json_schema");
  // json_object maps to json_schema type but with no guidance_data
  EXPECT_EQ(session_request.options.Find("guidance_data"), nullptr);
}

TEST(ChatCompletionsConverterTest, MapGuidance_Text_SetsToolChoiceNone) {
  ChatCompletionRequest req;
  req.response_format = json{{"type", "text"}};

  Request session_request;
  MapGuidance(req, session_request);

  EXPECT_EQ(session_request.options.Find("guidance_type"), nullptr);
  auto it = session_request.options.find("tool_choice");
  ASSERT_NE(it, session_request.options.Entries().end());
  EXPECT_EQ(it->second, "none");
}

// ========================================================================
// MapStopSequences
// ========================================================================

TEST(ChatCompletionsConverterTest, MapStopSequences_NoStop_NoOp) {
  ChatCompletionRequest req;

  Request session_request;
  MapStopSequences(req, session_request);

  EXPECT_EQ(session_request.options.Find("early_stopping"), nullptr);
}

TEST(ChatCompletionsConverterTest, MapStopSequences_StringStop) {
  ChatCompletionRequest req;
  req.stop = json("END");

  Request session_request;
  MapStopSequences(req, session_request);

  EXPECT_STREQ(session_request.options.Find("early_stopping"), "true");
}

TEST(ChatCompletionsConverterTest, MapStopSequences_ArrayStop) {
  ChatCompletionRequest req;
  req.stop = json::parse(R"(["END", "STOP"])");

  Request session_request;
  MapStopSequences(req, session_request);

  EXPECT_STREQ(session_request.options.Find("early_stopping"), "true");
}

TEST(ChatCompletionsConverterTest, MapStopSequences_EmptyString_NoEarlyStopping) {
  ChatCompletionRequest req;
  req.stop = json("");

  Request session_request;
  MapStopSequences(req, session_request);

  EXPECT_EQ(session_request.options.Find("early_stopping"), nullptr);
}

TEST(ChatCompletionsConverterTest, MapStopSequences_EmptyArray_NoEarlyStopping) {
  ChatCompletionRequest req;
  req.stop = json::array();

  Request session_request;
  MapStopSequences(req, session_request);

  EXPECT_EQ(session_request.options.Find("early_stopping"), nullptr);
}

// ========================================================================
// BuildResponse
// ========================================================================

TEST(ChatCompletionsConverterTest, BuildResponse_AssistantTextMessage) {
  Response response;
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Hello there!"));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
  response.usage = {10, 5, 15, 3};

  auto result = BuildResponse(response, "chatcmpl-abc", 1000, "test-model");

  EXPECT_EQ(result.id, "chatcmpl-abc");
  EXPECT_EQ(result.created, 1000);
  EXPECT_EQ(result.model, "test-model");
  ASSERT_EQ(result.choices.size(), 1u);
  EXPECT_EQ(result.choices[0].finish_reason, "stop");
  ASSERT_TRUE(result.choices[0].message.content.has_value());
  EXPECT_EQ(*result.choices[0].message.content, "Hello there!");
  EXPECT_FALSE(result.choices[0].message.tool_calls.has_value());

  EXPECT_EQ(result.usage.prompt_tokens, 10);
  EXPECT_EQ(result.usage.completion_tokens, 5);
  EXPECT_EQ(result.usage.total_tokens, 15);
  EXPECT_EQ(result.usage.completion_tokens_details.reasoning_tokens, 3);
}

TEST(ChatCompletionsConverterTest, BuildResponse_ReasoningOnlyMessageHasNoVisibleContent) {
  Response response;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("private scratchpad", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_LENGTH;

  auto result = BuildResponse(response, "chatcmpl-reasoning", 1000, "test-model");

  ASSERT_EQ(result.choices.size(), 1u);
  ASSERT_TRUE(result.choices[0].message.content.has_value());
  EXPECT_TRUE(result.choices[0].message.content->empty());
}

TEST(ChatCompletionsConverterTest, BuildResponse_InterleavedReasoningKeepsVisibleTextInOrder) {
  Response response;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("think one", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  parts.push_back(std::make_unique<TextItem>("answer one", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
  parts.push_back(std::make_unique<TextItem>("think two", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  parts.push_back(std::make_unique<TextItem>(" answer two", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
  response.items.push_back(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;

  auto result = BuildResponse(response, "chatcmpl-reasoning", 1000, "test-model");

  ASSERT_EQ(result.choices.size(), 1u);
  ASSERT_TRUE(result.choices[0].message.content.has_value());
  EXPECT_EQ(*result.choices[0].message.content, "answer one answer two");
}

TEST(ChatCompletionsConverterTest, BuildResponse_ToolCallItems) {
  Response response;

  // Add ToolCallItems
  response.items.push_back(
      std::make_unique<ToolCallItem>("call_1", "get_weather", R"({"city":"Seattle"})"));
  response.items.push_back(
      std::make_unique<ToolCallItem>("call_2", "get_time", R"({"tz":"PST"})"));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_TOOL_CALLS;
  response.usage = {20, 10, 30};

  auto result = BuildResponse(response, "chatcmpl-tools", 2000, "tool-model");

  ASSERT_EQ(result.choices.size(), 1u);
  EXPECT_EQ(result.choices[0].finish_reason, "tool_calls");

  // When finish_reason is tool_calls, tool_calls should be populated, not content
  ASSERT_TRUE(result.choices[0].message.tool_calls.has_value());
  auto& tcs = *result.choices[0].message.tool_calls;
  ASSERT_EQ(tcs.size(), 2u);

  EXPECT_EQ(tcs[0].id, "call_1");
  EXPECT_EQ(tcs[0].type, "function");
  EXPECT_EQ(tcs[0].function.name, "get_weather");
  EXPECT_EQ(tcs[0].function.arguments, R"({"city":"Seattle"})");

  EXPECT_EQ(tcs[1].id, "call_2");
  EXPECT_EQ(tcs[1].function.name, "get_time");
}

TEST(ChatCompletionsConverterTest, BuildResponse_FinishLength) {
  Response response;
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "truncated..."));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_LENGTH;

  auto result = BuildResponse(response, "id", 0, "m");

  ASSERT_EQ(result.choices.size(), 1u);
  EXPECT_EQ(result.choices[0].finish_reason, "length");
}

TEST(ChatCompletionsConverterTest, BuildResponse_EmptyItems) {
  Response response;
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;

  auto result = BuildResponse(response, "id", 0, "m");

  ASSERT_EQ(result.choices.size(), 1u);
  EXPECT_EQ(result.choices[0].finish_reason, "stop");
  // No assistant message → content should be whatever empty string was default
  ASSERT_TRUE(result.choices[0].message.content.has_value());
  EXPECT_EQ(*result.choices[0].message.content, "");
}

TEST(ChatCompletionsConverterTest, BuildResponse_IgnoresNonAssistantMessages) {
  Response response;
  // A user message shouldn't be picked up as assistant response text
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "User said this"));
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Assistant reply"));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;

  auto result = BuildResponse(response, "id", 0, "m");

  ASSERT_TRUE(result.choices[0].message.content.has_value());
  EXPECT_EQ(*result.choices[0].message.content, "Assistant reply");
}

// ========================================================================
// FormatStreamingChunk
// ========================================================================

TEST(ChatCompletionsConverterTest, FormatStreamingChunk_ContainsDeltaContent) {
  std::string chunk_json = FormatStreamingChunk("Hello", "chatcmpl-s1", 1000, "test-model");

  auto parsed = json::parse(chunk_json);
  EXPECT_EQ(parsed["id"], "chatcmpl-s1");
  EXPECT_EQ(parsed["created"], 1000);
  EXPECT_EQ(parsed["model"], "test-model");
  EXPECT_EQ(parsed["object"], "chat.completion.chunk");
  ASSERT_TRUE(parsed["choices"].is_array());
  ASSERT_EQ(parsed["choices"].size(), 1u);
  EXPECT_EQ(parsed["choices"][0]["delta"]["content"], "Hello");
}

// ========================================================================
// FormatInitialStreamingChunk
// ========================================================================

TEST(ChatCompletionsConverterTest, FormatInitialStreamingChunk_HasRoleAndEmptyContent) {
  std::string chunk_json = FormatInitialStreamingChunk("chatcmpl-init", 2000, "model-x");

  auto parsed = json::parse(chunk_json);
  EXPECT_EQ(parsed["id"], "chatcmpl-init");
  EXPECT_EQ(parsed["created"], 2000);
  EXPECT_EQ(parsed["model"], "model-x");
  ASSERT_EQ(parsed["choices"].size(), 1u);
  EXPECT_EQ(parsed["choices"][0]["delta"]["role"], "assistant");
  EXPECT_EQ(parsed["choices"][0]["delta"]["content"], "");
}

// ========================================================================
// FormatFinalStreamingChunk
// ========================================================================

TEST(ChatCompletionsConverterTest, FormatFinalStreamingChunk_HasFinishReason) {
  std::string chunk_json = FormatFinalStreamingChunk(
      FOUNDRY_LOCAL_FINISH_STOP, "chatcmpl-fin", 3000, "model-y");

  auto parsed = json::parse(chunk_json);
  EXPECT_EQ(parsed["id"], "chatcmpl-fin");
  EXPECT_EQ(parsed["created"], 3000);
  EXPECT_EQ(parsed["model"], "model-y");
  ASSERT_EQ(parsed["choices"].size(), 1u);
  EXPECT_EQ(parsed["choices"][0]["finish_reason"], "stop");
}

TEST(ChatCompletionsConverterTest, FormatFinalStreamingChunk_ToolCallsReason) {
  std::string chunk_json = FormatFinalStreamingChunk(
      FOUNDRY_LOCAL_FINISH_TOOL_CALLS, "chatcmpl-tc", 4000, "model-z");

  auto parsed = json::parse(chunk_json);
  EXPECT_EQ(parsed["choices"][0]["finish_reason"], "tool_calls");
}

TEST(ChatCompletionsConverterTest, FormatFinalStreamingChunk_LengthReason) {
  std::string chunk_json = FormatFinalStreamingChunk(
      FOUNDRY_LOCAL_FINISH_LENGTH, "chatcmpl-len", 5000, "model-w");

  auto parsed = json::parse(chunk_json);
  EXPECT_EQ(parsed["choices"][0]["finish_reason"], "length");
}

// ========================================================================
// BuildResponse — reasoning_content
// ========================================================================

TEST(ChatCompletionsConverterTest, BuildResponse_ReasoningOnlyPopulatesReasoningContent) {
  Response response;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("deep thought", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;

  auto result = BuildResponse(response, "id", 0, "m");

  ASSERT_EQ(result.choices.size(), 1u);
  ASSERT_TRUE(result.choices[0].message.reasoning_content.has_value());
  EXPECT_EQ(*result.choices[0].message.reasoning_content, "deep thought");
  // Visible content should be empty
  ASSERT_TRUE(result.choices[0].message.content.has_value());
  EXPECT_TRUE(result.choices[0].message.content->empty());
}

TEST(ChatCompletionsConverterTest, BuildResponse_InterleavedReasoningPopulatesBothFields) {
  Response response;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("think A", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  parts.push_back(std::make_unique<TextItem>("visible A", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
  parts.push_back(std::make_unique<TextItem>("think B", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  parts.push_back(std::make_unique<TextItem>(" visible B", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;

  auto result = BuildResponse(response, "id", 0, "m");

  ASSERT_EQ(result.choices.size(), 1u);
  ASSERT_TRUE(result.choices[0].message.content.has_value());
  EXPECT_EQ(*result.choices[0].message.content, "visible A visible B");
  ASSERT_TRUE(result.choices[0].message.reasoning_content.has_value());
  EXPECT_EQ(*result.choices[0].message.reasoning_content, "think Athink B");
}

TEST(ChatCompletionsConverterTest, BuildResponse_ReasoningAndToolCallPopulateBothFields) {
  Response response;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("choose a tool", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));
  response.items.push_back(std::make_unique<ToolCallItem>("call_1", "search", R"({"query":"term"})"));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_TOOL_CALLS;

  auto result = BuildResponse(response, "id", 0, "m");

  ASSERT_EQ(result.choices.size(), 1u);
  ASSERT_TRUE(result.choices[0].message.reasoning_content.has_value());
  EXPECT_EQ(*result.choices[0].message.reasoning_content, "choose a tool");
  ASSERT_TRUE(result.choices[0].message.tool_calls.has_value());
  ASSERT_EQ(result.choices[0].message.tool_calls->size(), 1u);
  EXPECT_EQ(result.choices[0].message.tool_calls->front().function.name, "search");
  EXPECT_EQ(result.choices[0].finish_reason, "tool_calls");
}

TEST(ChatCompletionsConverterTest, BuildResponse_NoReasoningOmitsReasoningContent) {
  Response response;
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "plain text"));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;

  auto result = BuildResponse(response, "id", 0, "m");

  ASSERT_EQ(result.choices.size(), 1u);
  EXPECT_FALSE(result.choices[0].message.reasoning_content.has_value());
  ASSERT_TRUE(result.choices[0].message.content.has_value());
  EXPECT_EQ(*result.choices[0].message.content, "plain text");

  // Verify JSON output omits reasoning_content
  json j = result;
  EXPECT_FALSE(j["choices"][0]["message"].contains("reasoning_content"));
}

TEST(ChatCompletionsConverterTest, BuildResponse_ReasoningContentSerializesInJson) {
  Response response;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("reasoning here", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  parts.push_back(std::make_unique<TextItem>("answer here", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;

  auto result = BuildResponse(response, "chatcmpl-r1", 1000, "reasoning-model");
  json j = result;

  EXPECT_EQ(j["choices"][0]["message"]["content"], "answer here");
  EXPECT_EQ(j["choices"][0]["message"]["reasoning_content"], "reasoning here");
}

// ========================================================================
// FormatReasoningStreamingChunk
// ========================================================================

TEST(ChatCompletionsConverterTest, FormatReasoningStreamingChunk_ContainsDeltaReasoningContent) {
  std::string chunk_json = FormatReasoningStreamingChunk("step 1", "chatcmpl-r1", 1000, "reasoning-model");

  auto parsed = json::parse(chunk_json);
  EXPECT_EQ(parsed["id"], "chatcmpl-r1");
  EXPECT_EQ(parsed["created"], 1000);
  EXPECT_EQ(parsed["model"], "reasoning-model");
  EXPECT_EQ(parsed["object"], "chat.completion.chunk");
  ASSERT_EQ(parsed["choices"].size(), 1u);
  EXPECT_EQ(parsed["choices"][0]["delta"]["reasoning_content"], "step 1");
  EXPECT_FALSE(parsed["choices"][0]["delta"].contains("content"));
}

TEST(ChatCompletionsConverterTest, FormatReasoningStreamingChunk_DoesNotContainContent) {
  std::string chunk_json = FormatReasoningStreamingChunk("thinking", "id", 0, "m");

  auto parsed = json::parse(chunk_json);
  const auto& delta = parsed["choices"][0]["delta"];
  EXPECT_TRUE(delta.contains("reasoning_content"));
  EXPECT_FALSE(delta.contains("content"));
  EXPECT_FALSE(delta.contains("role"));
}
