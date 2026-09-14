// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for chat_completions_converter.cc — pure conversion functions that
// map between OpenAI ChatCompletions contract types and internal session types.
//
#include "contracts/chat_completions_converter.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/chat/stop_strings.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>

using namespace fl;
using namespace fl::chat_completions;
using json = nlohmann::json;

namespace {

template <typename Fn>
void ExpectInvalidArgument(Fn&& fn, const std::string& message_fragment) {
  try {
    fn();
    FAIL() << "Expected fl::Exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(e.what()).find(message_fragment), std::string::npos) << e.what();
  }
}

}  // namespace

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

  EXPECT_FALSE(req.presence_penalty.has_value());
  EXPECT_FALSE(req.frequency_penalty.has_value());
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
  req.messages.push_back({"assistant", std::string(""), {}, {}, {}});  // empty assistant without reasoning
  req.messages.push_back({"user", std::string("Real message"), {}, {}, {}});

  Request session_request;
  BuildRequestItems(req, session_request);

  // Only the non-empty message should be added
  ASSERT_EQ(session_request.items.size(), 1u);
  auto* msg = static_cast<MessageItem*>(session_request.items[0]);
  EXPECT_EQ(msg->GetSimpleText(), "Real message");
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_ReasoningOnlyAssistantPreservesAnEmptyRoleBoundary) {
  ChatCompletionRequest req;
  ChatCompletionMessage assistant;
  assistant.role = "assistant";
  assistant.content = std::string("");
  assistant.reasoning_content = "private scratchpad";
  req.messages.push_back(std::move(assistant));

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 1u);
  ASSERT_EQ(session_request.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  const auto* boundary = static_cast<const MessageItem*>(session_request.items[0]);
  EXPECT_EQ(boundary->role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(boundary->GetSimpleText(), "");

  const auto messages = BuildTranscriptMessages(session_request.items);
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(messages[0].VisibleText().empty());
  EXPECT_TRUE(messages[0].ReasoningText().empty());
  EXPECT_EQ(BuildChatMessagesJson(messages), R"([{"role":"assistant","content":""}])");
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

TEST(ChatCompletionsConverterTest, BuildRequestItems_ToolRoleKeepsEmptyResult) {
  ChatCompletionRequest req;
  ChatCompletionMessage tool_msg;
  tool_msg.role = "tool";
  tool_msg.content = std::string("");
  tool_msg.tool_call_id = "call_1";
  req.messages.push_back(tool_msg);

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 1u);
  auto* tr = static_cast<ToolResultItem*>(session_request.items[0]);
  EXPECT_EQ(tr->call_id, "call_1");
  EXPECT_EQ(tr->result, "");
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_AssistantToolCallsWithNullContent) {
  ChatCompletionRequest req;
  ChatCompletionMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls.push_back({"call_1", "function", {"get_weather", R"({"city":"Seattle"})"}, std::nullopt});
  assistant.tool_calls.push_back({"call_2", "function", {"get_time", "{}"}, std::nullopt});
  req.messages.push_back(assistant);

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 2u);
  ASSERT_EQ(session_request.items[0]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);

  auto* first = static_cast<ToolCallItem*>(session_request.items[0]);
  EXPECT_EQ(first->call_id, "call_1");
  EXPECT_EQ(first->name, "get_weather");
  EXPECT_EQ(first->arguments, R"({"city":"Seattle"})");

  auto* second = static_cast<ToolCallItem*>(session_request.items[1]);
  EXPECT_EQ(second->call_id, "call_2");
  EXPECT_EQ(second->name, "get_time");
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_AssistantContentPrecedesItsToolCalls) {
  ChatCompletionRequest req;
  ChatCompletionMessage assistant;
  assistant.role = "assistant";
  assistant.content = "Let me check.";
  assistant.tool_calls.push_back({"call_1", "function", {"get_weather", "{}"}, std::nullopt});
  req.messages.push_back(assistant);

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 2u);
  ASSERT_EQ(session_request.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(static_cast<MessageItem*>(session_request.items[0])->GetSimpleText(), "Let me check.");
  EXPECT_EQ(session_request.items[1]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_PropagatesParticipantName) {
  ChatCompletionRequest req;
  ChatCompletionMessage user;
  user.role = "user";
  user.content = "Hello";
  user.name = "alice";
  req.messages.push_back(user);

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 1u);
  auto* msg = static_cast<MessageItem*>(session_request.items[0]);
  EXPECT_EQ(msg->name, "alice");

  // And it survives into the transcript projection the chat template consumes.
  auto messages = BuildTranscriptMessages(session_request.items);
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].name, "alice");
  EXPECT_EQ(BuildChatMessagesJson(messages), R"([{"role":"user","content":"Hello","name":"alice"}])");
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_PreservesNameOnToolCallOnlyAssistantMessage) {
  // An assistant message that only issues tool calls has null content, so its name has nowhere to live on a
  // ToolCallItem. A content-free MessageItem carries it without fabricating text the caller never sent.
  ChatCompletionRequest req;
  ChatCompletionMessage assistant;
  assistant.role = "assistant";
  assistant.name = "weather_bot";
  assistant.tool_calls.push_back({"call_1", "function", {"get_weather", R"({"city":"Seattle"})"}, std::nullopt});
  req.messages.push_back(assistant);

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 2u);
  ASSERT_EQ(session_request.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);

  auto* named = static_cast<MessageItem*>(session_request.items[0]);
  EXPECT_EQ(named->name, "weather_bot");
  EXPECT_TRUE(named->content.empty());
  EXPECT_EQ(session_request.items[1]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);

  // The transcript folds the call into the named message, so the name reaches the template alongside tool_calls.
  auto messages = BuildTranscriptMessages(session_request.items);
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].name, "weather_bot");
  ASSERT_EQ(messages[0].ToolCalls().size(), 1u);
  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"assistant","content":"","name":"weather_bot","tool_calls":)"
            R"([{"id":"call_1","type":"function","function":{"name":"get_weather",)"
            R"("arguments":{"city":"Seattle"}}}]}])");
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_UnnamedToolCallOnlyAssistantMessageAddsNoMessageItem) {
  ChatCompletionRequest req;
  ChatCompletionMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls.push_back({"call_1", "function", {"get_weather", "{}"}, std::nullopt});
  req.messages.push_back(assistant);

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 1u);
  EXPECT_EQ(session_request.items[0]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);
}

TEST(ChatCompletionsConverterTest, BuildRequestItems_ToolCallsOnNonAssistantRoleAreIgnored) {
  ChatCompletionRequest req;
  ChatCompletionMessage user;
  user.role = "user";
  user.content = "Hello";
  user.tool_calls.push_back({"call_1", "function", {"get_weather", "{}"}, std::nullopt});
  req.messages.push_back(user);

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 1u);
  EXPECT_EQ(session_request.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
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

TEST(ChatCompletionsConverterTest, MapRequestParameters_TemperatureAndTopP) {
  ChatCompletionRequest req;
  req.temperature = 0.7f;
  req.top_p = 0.9f;

  Request session_request;
  MapRequestParameters(req, session_request);

  EXPECT_NE(session_request.options.Find("temperature"), nullptr);
  EXPECT_NE(session_request.options.Find("top_p"), nullptr);
}

TEST(ChatCompletionsConverterTest, MapRequestParameters_RejectsNonzeroPenalties) {
  for (const auto& [frequency, presence] :
       {std::pair{0.5f, 0.0f}, std::pair{0.0f, 0.3f}, std::pair{-0.5f, 0.0f}, std::pair{0.0f, -0.3f}}) {
    ChatCompletionRequest req;
    req.frequency_penalty = frequency;
    req.presence_penalty = presence;

    Request session_request;
    EXPECT_THROW(MapRequestParameters(req, session_request), fl::Exception);
  }
}

TEST(ChatCompletionsConverterTest, MapRequestParameters_ZeroPenaltiesAreNoOps) {
  ChatCompletionRequest req;
  req.frequency_penalty = 0.0f;
  req.presence_penalty = 0.0f;

  Request session_request;
  MapRequestParameters(req, session_request);

  EXPECT_EQ(session_request.options.Find("frequency_penalty"), nullptr);
  EXPECT_EQ(session_request.options.Find("presence_penalty"), nullptr);
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
  EXPECT_EQ(session_request.options.Find(kInternalStopStringsOptionKey), nullptr);
}

TEST(ChatCompletionsConverterTest, MapStopSequences_StringStopStoresNormalizedPayload) {
  ChatCompletionRequest req;
  req.stop = json("END");

  Request session_request;
  MapStopSequences(req, session_request);

  EXPECT_EQ(session_request.options.Find("early_stopping"), nullptr);
  EXPECT_EQ(LoadStopStringsOption(session_request.options), (std::vector<std::string>{"END"}));
}

TEST(ChatCompletionsConverterTest, MapStopSequences_ArrayStopStoresNormalizedPayload) {
  ChatCompletionRequest req;
  req.stop = json::parse(R"(["END", "STOP"])");

  Request session_request;
  MapStopSequences(req, session_request);

  EXPECT_EQ(session_request.options.Find("early_stopping"), nullptr);
  EXPECT_EQ(LoadStopStringsOption(session_request.options), (std::vector<std::string>{"END", "STOP"}));
}

TEST(ChatCompletionsConverterTest, MapStopSequences_TypeMustBeStringOrArray) {
  ExpectInvalidArgument(
      []() {
        ChatCompletionRequest req;
        req.stop = json(123);
        Request session_request;
        MapStopSequences(req, session_request);
      },
      "must be a string or array of strings");
}

TEST(ChatCompletionsConverterTest, MapStopSequences_ArrayMembersMustBeStrings) {
  ExpectInvalidArgument(
      []() {
        ChatCompletionRequest req;
        req.stop = json::array({"END", 42});
        Request session_request;
        MapStopSequences(req, session_request);
      },
      "stop[1] must be a string");
}

TEST(ChatCompletionsConverterTest, MapStopSequences_RejectsEmptyString) {
  ExpectInvalidArgument(
      []() {
        ChatCompletionRequest req;
        req.stop = json("");
        Request session_request;
        MapStopSequences(req, session_request);
      },
      "must not be empty");
}

TEST(ChatCompletionsConverterTest, MapStopSequences_EmptyArrayIsNoOp) {
  ChatCompletionRequest req;
  req.stop = json::array();
  Request session_request;

  MapStopSequences(req, session_request);

  EXPECT_EQ(session_request.options.Find(kInternalStopStringsOptionKey), nullptr);
}

TEST(ChatCompletionsConverterTest, MapStopSequences_RejectsEmbeddedNul) {
  ExpectInvalidArgument(
      []() {
        ChatCompletionRequest req;
        req.stop = json(std::string("A\0B", 3));
        Request session_request;
        MapStopSequences(req, session_request);
      },
      "embedded NUL");
}

TEST(ChatCompletionsConverterTest, MapStopSequences_RejectsMoreThanFourStops) {
  ExpectInvalidArgument(
      []() {
        ChatCompletionRequest req;
        req.stop = json::array({"a", "b", "c", "d", "e"});
        Request session_request;
        MapStopSequences(req, session_request);
      },
      "at most 4 strings");
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

TEST(ChatCompletionsConverterTest, ReasoningOnlyResponseReplayKeepsAdjacentMessageRolesAndHidesReasoning) {
  Response response;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("private scratchpad", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_LENGTH;

  const auto completion = BuildResponse(response, "id", 0, "m");
  ASSERT_EQ(completion.choices.size(), 1u);

  const json replay_json = {
      {"model", "m"},
      {"messages",
       json::array({{{"role", "user"}, {"content", "before"}},
                    json(completion.choices[0].message),
                    {{"role", "user"}, {"content", "after"}}})}};
  const auto replay = replay_json.get<ChatCompletionRequest>();

  Request session_request;
  BuildRequestItems(replay, session_request);

  const auto messages = BuildTranscriptMessages(session_request.items);
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[0].role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(messages[2].role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_TRUE(messages[1].VisibleText().empty());
  EXPECT_TRUE(messages[1].ReasoningText().empty());
  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"user","content":"before"},)"
            R"({"role":"assistant","content":""},)"
            R"({"role":"user","content":"after"}])");
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

// ========================================================================
// Assistant-turn ordering invariant on a Chat Completions payload.
//
// A chat completion carries the assistant reply as `content` plus a `tool_calls` array — the same schema the
// transcript projection uses, with the same limitation. These pin that the limitation is handled explicitly on this
// endpoint too, rather than by quietly moving text in front of the call it followed.
// ========================================================================

TEST(ChatCompletionsConverterTest, AssistantContentBeforeItsToolCallsIsAccepted) {
  ChatCompletionRequest req;
  req.messages.push_back({"user", std::string("Weather in Seattle?"), {}, {}, {}});

  ChatCompletionMessage assistant;
  assistant.role = "assistant";
  assistant.content = std::string("Let me check.");
  assistant.tool_calls.push_back({"call_1", "function", {"get_weather", R"({"city":"Seattle"})"}, std::nullopt});
  req.messages.push_back(assistant);

  req.messages.push_back({"tool", std::string("sunny"), {}, std::string("call_1"), {}});

  Request session_request;
  BuildRequestItems(req, session_request);

  auto messages = BuildTranscriptMessages(session_request.items);
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_FALSE(messages[1].HasVisibleTextAfterToolCall());

  const ChatTranscript payload_transcript;
  EXPECT_NO_THROW(payload_transcript.ValidateInputs(messages));
}

TEST(ChatCompletionsConverterTest, AnAssistantMessageContinuingAnUnansweredCallIsRejected) {
  // Two assistant messages in a row, the first carrying an unanswered call. They are one assistant turn to any chat
  // template, and that turn would have to report the second message's text before the call — so it is refused.
  ChatCompletionRequest req;
  req.messages.push_back({"user", std::string("Weather in Seattle?"), {}, {}, {}});

  ChatCompletionMessage assistant;
  assistant.role = "assistant";
  assistant.content = std::string("Let me check.");
  assistant.tool_calls.push_back({"call_1", "function", {"get_weather", R"({"city":"Seattle"})"}, std::nullopt});
  req.messages.push_back(assistant);

  req.messages.push_back({"assistant", std::string("One moment."), {}, {}, {}});

  Request session_request;
  BuildRequestItems(req, session_request);

  auto messages = BuildTranscriptMessages(session_request.items);
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_TRUE(messages[1].HasVisibleTextAfterToolCall());

  const ChatTranscript payload_transcript;
  try {
    payload_transcript.ValidateInputs(messages);
    FAIL() << "expected the ordering invariant to reject the payload";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("visible text after a tool call"), std::string::npos) << ex.what();
  }
}

TEST(ChatCompletionsConverterTest, AnAssistantReplyAfterAToolResultStaysItsOwnTurn) {
  // The ordinary multi-turn tool exchange: the tool result separates the two assistant turns, so nothing merges and
  // nothing is rejected.
  ChatCompletionRequest req;
  req.messages.push_back({"user", std::string("Weather in Seattle?"), {}, {}, {}});

  ChatCompletionMessage assistant;
  assistant.role = "assistant";
  assistant.content = std::string("Let me check.");
  assistant.tool_calls.push_back({"call_1", "function", {"get_weather", "{}"}, std::nullopt});
  req.messages.push_back(assistant);

  req.messages.push_back({"tool", std::string("sunny"), {}, std::string("call_1"), {}});
  req.messages.push_back({"assistant", std::string("It is sunny."), {}, {}, {}});
  req.messages.push_back({"user", std::string("And tomorrow?"), {}, {}, {}});

  Request session_request;
  BuildRequestItems(req, session_request);

  auto messages = BuildTranscriptMessages(session_request.items);
  ASSERT_EQ(messages.size(), 5u);
  EXPECT_EQ(messages[3].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(messages[3].VisibleText(), "It is sunny.");

  const ChatTranscript payload_transcript;
  EXPECT_NO_THROW(payload_transcript.ValidateInputs(messages));
}
