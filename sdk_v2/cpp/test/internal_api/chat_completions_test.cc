// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/chat_completions.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace fl;
using json = nlohmann::json;

// ========================================================================
// from_json — ChatCompletionMessage
// ========================================================================

TEST(ChatCompletionMessageTest, BasicUserMessage) {
  auto j = json::parse(R"({"role": "user", "content": "hello"})");
  auto msg = j.get<ChatCompletionMessage>();

  EXPECT_EQ(msg.role, "user");
  ASSERT_TRUE(msg.content.has_value());
  EXPECT_EQ(*msg.content, "hello");
  EXPECT_FALSE(msg.name.has_value());
  EXPECT_FALSE(msg.tool_call_id.has_value());
  EXPECT_FALSE(msg.tool_calls.has_value());
}

TEST(ChatCompletionMessageTest, NullContent) {
  auto j = json::parse(R"({"role": "assistant", "content": null})");
  auto msg = j.get<ChatCompletionMessage>();

  EXPECT_EQ(msg.role, "assistant");
  EXPECT_FALSE(msg.content.has_value());
}

TEST(ChatCompletionMessageTest, ArrayContentExtractsTextParts) {
  auto j = json::parse(R"({
    "role": "user",
    "content": [
      {"type": "text", "text": "Look at this: "},
      {"type": "image_url", "image_url": {"url": "http://img.png"}},
      {"type": "input_text", "text": "and this"}
    ]
  })");
  auto msg = j.get<ChatCompletionMessage>();

  ASSERT_TRUE(msg.content.has_value());
  EXPECT_EQ(*msg.content, "Look at this: and this");
}

TEST(ChatCompletionMessageTest, OptionalFieldsPopulated) {
  auto j = json::parse(R"({
    "role": "tool",
    "content": "result",
    "name": "weather_bot",
    "tool_call_id": "call_abc123"
  })");
  auto msg = j.get<ChatCompletionMessage>();

  EXPECT_EQ(msg.role, "tool");
  ASSERT_TRUE(msg.name.has_value());
  EXPECT_EQ(*msg.name, "weather_bot");
  ASSERT_TRUE(msg.tool_call_id.has_value());
  EXPECT_EQ(*msg.tool_call_id, "call_abc123");
}

TEST(ChatCompletionMessageTest, ToolCallsPreservedAsJson) {
  auto j = json::parse(R"({
    "role": "assistant",
    "content": null,
    "tool_calls": [
      {"id": "call_1", "type": "function", "function": {"name": "get_weather", "arguments": "{}"}}
    ]
  })");
  auto msg = j.get<ChatCompletionMessage>();

  ASSERT_TRUE(msg.tool_calls.has_value());
  EXPECT_TRUE(msg.tool_calls->is_array());
  EXPECT_EQ(msg.tool_calls->size(), 1);
}

// ========================================================================
// from_json — ChatCompletionFunctionDef (+ round-trip to_json)
// ========================================================================

TEST(ChatCompletionFunctionDefTest, MinimalFunction) {
  auto j = json::parse(R"({"name": "get_time"})");
  auto f = j.get<ChatCompletionFunctionDef>();

  EXPECT_EQ(f.name, "get_time");
  EXPECT_FALSE(f.description.has_value());
  EXPECT_FALSE(f.parameters.has_value());
  EXPECT_FALSE(f.strict.has_value());
}

TEST(ChatCompletionFunctionDefTest, FullFunction) {
  auto j = json::parse(R"({
    "name": "search",
    "description": "Search the web",
    "parameters": {"type": "object", "properties": {"q": {"type": "string"}}},
    "strict": true
  })");
  auto f = j.get<ChatCompletionFunctionDef>();

  EXPECT_EQ(f.name, "search");
  ASSERT_TRUE(f.description.has_value());
  EXPECT_EQ(*f.description, "Search the web");
  ASSERT_TRUE(f.parameters.has_value());
  EXPECT_EQ((*f.parameters)["type"], "object");
  ASSERT_TRUE(f.strict.has_value());
  EXPECT_TRUE(*f.strict);
}

TEST(ChatCompletionFunctionDefTest, RoundTripMinimal) {
  ChatCompletionFunctionDef f;
  f.name = "noop";

  json j = f;
  EXPECT_EQ(j["name"], "noop");
  EXPECT_FALSE(j.contains("description"));
  EXPECT_FALSE(j.contains("parameters"));
  EXPECT_FALSE(j.contains("strict"));
}

TEST(ChatCompletionFunctionDefTest, RoundTripFull) {
  auto input = json::parse(R"({
    "name": "calc",
    "description": "Calculate",
    "parameters": {"type": "object"},
    "strict": false
  })");

  auto f = input.get<ChatCompletionFunctionDef>();
  json output = f;

  EXPECT_EQ(output["name"], "calc");
  EXPECT_EQ(output["description"], "Calculate");
  EXPECT_EQ(output["parameters"]["type"], "object");
  EXPECT_EQ(output["strict"], false);
}

// ========================================================================
// from_json — ChatCompletionTool (+ round-trip)
// ========================================================================

TEST(ChatCompletionToolTest, RoundTrip) {
  auto input = json::parse(R"({
    "type": "function",
    "function": {"name": "get_weather", "description": "Get weather"}
  })");

  auto tool = input.get<ChatCompletionTool>();
  EXPECT_EQ(tool.type, "function");
  EXPECT_EQ(tool.function.name, "get_weather");

  json output = tool;
  EXPECT_EQ(output["type"], "function");
  EXPECT_EQ(output["function"]["name"], "get_weather");
  EXPECT_EQ(output["function"]["description"], "Get weather");
}

TEST(ChatCompletionToolTest, DefaultType) {
  auto j = json::parse(R"({"function": {"name": "f"}})");
  auto tool = j.get<ChatCompletionTool>();
  EXPECT_EQ(tool.type, "function");
}

// ========================================================================
// from_json — ChatStreamOptions
// ========================================================================

TEST(ChatStreamOptionsTest, IncludeUsage) {
  auto j = json::parse(R"({"include_usage": true})");
  auto opts = j.get<ChatStreamOptions>();
  EXPECT_TRUE(opts.include_usage);
}

TEST(ChatStreamOptionsTest, DefaultsFalse) {
  auto j = json::parse(R"({})");
  auto opts = j.get<ChatStreamOptions>();
  EXPECT_FALSE(opts.include_usage);
}

// ========================================================================
// from_json — ChatCompletionRequest
// ========================================================================

TEST(ChatCompletionRequestTest, MinimalRequest) {
  auto j = json::parse(R"({
    "model": "phi-4-mini",
    "messages": [{"role": "user", "content": "Hi"}]
  })");
  auto req = j.get<ChatCompletionRequest>();

  EXPECT_EQ(req.model, "phi-4-mini");
  ASSERT_EQ(req.messages.size(), 1);
  EXPECT_EQ(req.messages[0].role, "user");
  EXPECT_FALSE(req.temperature.has_value());
  EXPECT_FALSE(req.top_p.has_value());
  EXPECT_FALSE(req.stream.has_value());
  EXPECT_FALSE(req.tools.has_value());
  EXPECT_FALSE(req.metadata.has_value());
}

TEST(ChatCompletionRequestTest, AllOptionalScalars) {
  auto j = json::parse(R"({
    "model": "m",
    "messages": [{"role": "user", "content": "x"}],
    "temperature": 0.5,
    "top_p": 0.9,
    "n": 2,
    "stream": true,
    "max_tokens": 100,
    "max_completion_tokens": 200,
    "presence_penalty": 0.1,
    "frequency_penalty": 0.2,
    "seed": 42,
    "logprobs": true,
    "top_logprobs": 5,
    "parallel_tool_calls": false,
    "user": "test-user"
  })");
  auto req = j.get<ChatCompletionRequest>();

  ASSERT_TRUE(req.temperature.has_value());
  EXPECT_FLOAT_EQ(*req.temperature, 0.5f);
  ASSERT_TRUE(req.top_p.has_value());
  EXPECT_FLOAT_EQ(*req.top_p, 0.9f);
  ASSERT_TRUE(req.n.has_value());
  EXPECT_EQ(*req.n, 2);
  ASSERT_TRUE(req.stream.has_value());
  EXPECT_TRUE(*req.stream);
  ASSERT_TRUE(req.max_tokens.has_value());
  EXPECT_EQ(*req.max_tokens, 100);
  ASSERT_TRUE(req.max_completion_tokens.has_value());
  EXPECT_EQ(*req.max_completion_tokens, 200);
  ASSERT_TRUE(req.presence_penalty.has_value());
  EXPECT_FLOAT_EQ(*req.presence_penalty, 0.1f);
  ASSERT_TRUE(req.frequency_penalty.has_value());
  EXPECT_FLOAT_EQ(*req.frequency_penalty, 0.2f);
  ASSERT_TRUE(req.seed.has_value());
  EXPECT_EQ(*req.seed, 42);
  ASSERT_TRUE(req.logprobs.has_value());
  EXPECT_TRUE(*req.logprobs);
  ASSERT_TRUE(req.top_logprobs.has_value());
  EXPECT_EQ(*req.top_logprobs, 5);
  ASSERT_TRUE(req.parallel_tool_calls.has_value());
  EXPECT_FALSE(*req.parallel_tool_calls);
  ASSERT_TRUE(req.user.has_value());
  EXPECT_EQ(*req.user, "test-user");
}

TEST(ChatCompletionRequestTest, StreamOptions) {
  auto j = json::parse(R"({
    "model": "m",
    "messages": [{"role": "user", "content": "x"}],
    "stream_options": {"include_usage": true}
  })");
  auto req = j.get<ChatCompletionRequest>();

  ASSERT_TRUE(req.stream_options.has_value());
  EXPECT_TRUE(req.stream_options->include_usage);
}

TEST(ChatCompletionRequestTest, ToolsArray) {
  auto j = json::parse(R"({
    "model": "m",
    "messages": [{"role": "user", "content": "x"}],
    "tools": [
      {"type": "function", "function": {"name": "f1"}},
      {"type": "function", "function": {"name": "f2", "description": "desc"}}
    ]
  })");
  auto req = j.get<ChatCompletionRequest>();

  ASSERT_TRUE(req.tools.has_value());
  EXPECT_EQ(req.tools->size(), 2);
  EXPECT_EQ((*req.tools)[0].function.name, "f1");
  EXPECT_EQ((*req.tools)[1].function.name, "f2");
}

TEST(ChatCompletionRequestTest, PolymorphicFieldsPreservedAsJson) {
  auto j = json::parse(R"({
    "model": "m",
    "messages": [{"role": "user", "content": "x"}],
    "stop": ["END", "STOP"],
    "tool_choice": "auto",
    "response_format": {"type": "json_object"}
  })");
  auto req = j.get<ChatCompletionRequest>();

  ASSERT_TRUE(req.stop.has_value());
  EXPECT_TRUE(req.stop->is_array());
  EXPECT_EQ(req.stop->size(), 2);

  ASSERT_TRUE(req.tool_choice.has_value());
  EXPECT_EQ(*req.tool_choice, "auto");

  ASSERT_TRUE(req.response_format.has_value());
  EXPECT_EQ((*req.response_format)["type"], "json_object");
}

TEST(ChatCompletionRequestTest, MetadataMap) {
  auto j = json::parse(R"({
    "model": "m",
    "messages": [{"role": "user", "content": "x"}],
    "metadata": {"session": "abc", "source": "test"}
  })");
  auto req = j.get<ChatCompletionRequest>();

  ASSERT_TRUE(req.metadata.has_value());
  EXPECT_EQ(req.metadata->size(), 2);
  EXPECT_EQ((*req.metadata)["session"], "abc");
  EXPECT_EQ((*req.metadata)["source"], "test");
}

// ========================================================================
// to_json — Response types
// ========================================================================

TEST(ChatCompletionResponseTest, FunctionCallSerialization) {
  ChatCompletionFunctionCall fc;
  fc.name = "get_weather";
  fc.arguments = R"({"city": "Seattle"})";

  json j = fc;
  EXPECT_EQ(j["name"], "get_weather");
  EXPECT_EQ(j["arguments"], R"({"city": "Seattle"})");
}

TEST(ChatCompletionResponseTest, ToolCallSerialization) {
  ChatCompletionToolCall tc;
  tc.id = "call_abc";
  tc.type = "function";
  tc.function.name = "get_weather";
  tc.function.arguments = "{}";

  json j = tc;
  EXPECT_EQ(j["id"], "call_abc");
  EXPECT_EQ(j["type"], "function");
  EXPECT_EQ(j["function"]["name"], "get_weather");
}

TEST(ChatCompletionResponseTest, ResponseMessageWithContent) {
  ChatCompletionResponseMessage msg;
  msg.role = "assistant";
  msg.content = "Hello!";

  json j = msg;
  EXPECT_EQ(j["role"], "assistant");
  EXPECT_EQ(j["content"], "Hello!");
  EXPECT_TRUE(j["refusal"].is_null());
  EXPECT_FALSE(j.contains("tool_calls"));
}

TEST(ChatCompletionResponseTest, ResponseMessageWithToolCalls) {
  ChatCompletionResponseMessage msg;
  msg.role = "assistant";
  // content is nullopt — should serialize as null

  ChatCompletionToolCall tc;
  tc.id = "call_1";
  tc.function.name = "f";
  tc.function.arguments = "{}";
  msg.tool_calls = std::vector<ChatCompletionToolCall>{tc};

  json j = msg;
  EXPECT_TRUE(j["content"].is_null());
  ASSERT_TRUE(j.contains("tool_calls"));
  EXPECT_EQ(j["tool_calls"].size(), 1);
}

TEST(ChatCompletionResponseTest, ChoiceSerialization) {
  ChatCompletionChoice choice;
  choice.index = 0;
  choice.message.role = "assistant";
  choice.message.content = "test";
  choice.finish_reason = "stop";

  json j = choice;
  EXPECT_EQ(j["index"], 0);
  EXPECT_EQ(j["message"]["content"], "test");
  EXPECT_TRUE(j["logprobs"].is_null());
  EXPECT_EQ(j["finish_reason"], "stop");
}

TEST(ChatCompletionResponseTest, UsageSerialization) {
  ChatCompletionUsage usage;
  usage.prompt_tokens = 10;
  usage.completion_tokens = 20;
  usage.total_tokens = 30;
  usage.prompt_tokens_details.cached_tokens = 5;
  usage.completion_tokens_details.reasoning_tokens = 0;

  json j = usage;
  EXPECT_EQ(j["prompt_tokens"], 10);
  EXPECT_EQ(j["completion_tokens"], 20);
  EXPECT_EQ(j["total_tokens"], 30);
  EXPECT_EQ(j["prompt_tokens_details"]["cached_tokens"], 5);
  EXPECT_EQ(j["completion_tokens_details"]["reasoning_tokens"], 0);
}

TEST(ChatCompletionResponseTest, FullResponseSerialization) {
  ChatCompletionResponse resp;
  resp.id = "chatcmpl-abc";
  resp.object = "chat.completion";
  resp.created = 1700000000;
  resp.model = "phi-4-mini";

  ChatCompletionChoice choice;
  choice.index = 0;
  choice.message.role = "assistant";
  choice.message.content = "Hello!";
  choice.finish_reason = "stop";
  resp.choices.push_back(choice);

  resp.usage.prompt_tokens = 5;
  resp.usage.completion_tokens = 3;
  resp.usage.total_tokens = 8;

  json j = resp;
  EXPECT_EQ(j["id"], "chatcmpl-abc");
  EXPECT_EQ(j["object"], "chat.completion");
  EXPECT_EQ(j["created"], 1700000000);
  EXPECT_EQ(j["model"], "phi-4-mini");
  EXPECT_TRUE(j["system_fingerprint"].is_null());
  ASSERT_EQ(j["choices"].size(), 1);
  EXPECT_EQ(j["choices"][0]["message"]["content"], "Hello!");
  EXPECT_EQ(j["usage"]["total_tokens"], 8);
}

TEST(ChatCompletionResponseTest, ResponseWithSystemFingerprint) {
  ChatCompletionResponse resp;
  resp.id = "chatcmpl-x";
  resp.system_fingerprint = "fp_abc123";
  resp.created = 0;
  resp.model = "m";

  json j = resp;
  EXPECT_EQ(j["system_fingerprint"], "fp_abc123");
}

// ========================================================================
// to_json — Streaming types
// ========================================================================

TEST(ChatCompletionStreamingTest, DeltaWithRole) {
  ChatCompletionDelta delta;
  delta.role = "assistant";

  json j = delta;
  EXPECT_EQ(j["role"], "assistant");
  EXPECT_FALSE(j.contains("content"));
  EXPECT_FALSE(j.contains("tool_calls"));
}

TEST(ChatCompletionStreamingTest, DeltaWithContent) {
  ChatCompletionDelta delta;
  delta.content = "partial";

  json j = delta;
  EXPECT_FALSE(j.contains("role"));
  EXPECT_EQ(j["content"], "partial");
}

TEST(ChatCompletionStreamingTest, ChunkChoiceSerialization) {
  ChatCompletionChunkChoice choice;
  choice.index = 0;
  choice.delta.content = "token";
  // finish_reason is nullopt

  json j = choice;
  EXPECT_EQ(j["index"], 0);
  EXPECT_EQ(j["delta"]["content"], "token");
  EXPECT_TRUE(j["logprobs"].is_null());
  EXPECT_TRUE(j["finish_reason"].is_null());
}

TEST(ChatCompletionStreamingTest, ChunkChoiceWithFinishReason) {
  ChatCompletionChunkChoice choice;
  choice.index = 0;
  choice.finish_reason = "stop";

  json j = choice;
  EXPECT_EQ(j["finish_reason"], "stop");
}

TEST(ChatCompletionStreamingTest, FullChunkSerialization) {
  ChatCompletionChunk chunk;
  chunk.id = "chatcmpl-stream-1";
  chunk.created = 1700000000;
  chunk.model = "phi-4-mini";

  ChatCompletionChunkChoice choice;
  choice.index = 0;
  choice.delta.content = "Hi";
  chunk.choices.push_back(choice);

  json j = chunk;
  EXPECT_EQ(j["id"], "chatcmpl-stream-1");
  EXPECT_EQ(j["object"], "chat.completion.chunk");
  EXPECT_EQ(j["created"], 1700000000);
  EXPECT_EQ(j["model"], "phi-4-mini");
  EXPECT_TRUE(j["system_fingerprint"].is_null());
  ASSERT_EQ(j["choices"].size(), 1);
  EXPECT_EQ(j["choices"][0]["delta"]["content"], "Hi");
  EXPECT_FALSE(j.contains("usage"));
}

TEST(ChatCompletionStreamingTest, ChunkWithUsage) {
  ChatCompletionChunk chunk;
  chunk.id = "c";
  chunk.created = 0;
  chunk.model = "m";

  ChatCompletionUsage usage;
  usage.prompt_tokens = 5;
  usage.completion_tokens = 10;
  usage.total_tokens = 15;
  usage.completion_tokens_details.reasoning_tokens = 2;
  chunk.usage = usage;

  json j = chunk;
  ASSERT_TRUE(j.contains("usage"));
  EXPECT_EQ(j["usage"]["total_tokens"], 15);
  EXPECT_EQ(j["usage"]["completion_tokens_details"]["reasoning_tokens"], 2);
}

// ========================================================================
// reasoning_content serialization
// ========================================================================

TEST(ChatCompletionResponseTest, ResponseMessageWithReasoningContent) {
  ChatCompletionResponseMessage msg;
  msg.content = "The answer is 42.";
  msg.reasoning_content = "Let me think step by step...";

  json j = msg;
  EXPECT_EQ(j["content"], "The answer is 42.");
  EXPECT_EQ(j["reasoning_content"], "Let me think step by step...");
  EXPECT_TRUE(j["refusal"].is_null());
}

TEST(ChatCompletionResponseTest, ResponseMessageOmitsReasoningContentWhenAbsent) {
  ChatCompletionResponseMessage msg;
  msg.content = "Hello!";
  // reasoning_content left as nullopt

  json j = msg;
  EXPECT_EQ(j["content"], "Hello!");
  EXPECT_FALSE(j.contains("reasoning_content"));
}

TEST(ChatCompletionStreamingTest, DeltaWithReasoningContent) {
  ChatCompletionDelta delta;
  delta.reasoning_content = "thinking...";

  json j = delta;
  EXPECT_EQ(j["reasoning_content"], "thinking...");
  EXPECT_FALSE(j.contains("content"));
  EXPECT_FALSE(j.contains("role"));
}

TEST(ChatCompletionStreamingTest, DeltaOmitsReasoningContentWhenAbsent) {
  ChatCompletionDelta delta;
  delta.content = "visible text";

  json j = delta;
  EXPECT_EQ(j["content"], "visible text");
  EXPECT_FALSE(j.contains("reasoning_content"));
}

TEST(ChatCompletionStreamingTest, DeltaDoesNotMixReasoningAndContent) {
  // Verify that setting only reasoning_content does not produce a content field
  ChatCompletionDelta delta;
  delta.reasoning_content = "step 1";

  json j = delta;
  EXPECT_TRUE(j.contains("reasoning_content"));
  EXPECT_FALSE(j.contains("content"));

  // And vice versa
  ChatCompletionDelta delta2;
  delta2.content = "result";

  json j2 = delta2;
  EXPECT_TRUE(j2.contains("content"));
  EXPECT_FALSE(j2.contains("reasoning_content"));
}
