// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/responses.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace fl::responses;
using json = nlohmann::json;

// ========================================================================
// Enum string helpers
// ========================================================================

TEST(ResponseStatusTest, AllValuesRoundTrip) {
  EXPECT_EQ(ResponseStatusToString(ResponseStatus::kInProgress), "in_progress");
  EXPECT_EQ(ResponseStatusToString(ResponseStatus::kCompleted), "completed");
  EXPECT_EQ(ResponseStatusToString(ResponseStatus::kFailed), "failed");
  EXPECT_EQ(ResponseStatusToString(ResponseStatus::kCancelled), "cancelled");
  EXPECT_EQ(ResponseStatusToString(ResponseStatus::kIncomplete), "incomplete");

  EXPECT_EQ(ResponseStatusFromString("in_progress"), ResponseStatus::kInProgress);
  EXPECT_EQ(ResponseStatusFromString("completed"), ResponseStatus::kCompleted);
  EXPECT_EQ(ResponseStatusFromString("failed"), ResponseStatus::kFailed);
  EXPECT_EQ(ResponseStatusFromString("cancelled"), ResponseStatus::kCancelled);
  EXPECT_EQ(ResponseStatusFromString("incomplete"), ResponseStatus::kIncomplete);
}

TEST(ResponseStatusTest, UnknownDefaultsToInProgress) {
  EXPECT_EQ(ResponseStatusFromString("bogus"), ResponseStatus::kInProgress);
  EXPECT_EQ(ResponseStatusFromString(""), ResponseStatus::kInProgress);
}

TEST(StreamEventTypeTest, SelectedValues) {
  EXPECT_EQ(StreamEventTypeToString(StreamEventType::kResponseCreated), "response.created");
  EXPECT_EQ(StreamEventTypeToString(StreamEventType::kTextDelta), "response.output_text.delta");
  EXPECT_EQ(StreamEventTypeToString(StreamEventType::kFunctionCallArgumentsDelta), "response.function_call_arguments.delta");
  EXPECT_EQ(StreamEventTypeToString(StreamEventType::kError), "error");
}

// ========================================================================
// from_json — Input content types
// ========================================================================

TEST(InputContentTest, TextContent) {
  auto j = json::parse(R"({"text": "hello world"})");
  auto c = j.get<InputTextContent>();
  EXPECT_EQ(c.text, "hello world");
}

TEST(InputContentTest, ImageContentWithUrl) {
  auto j = json::parse(R"({"detail": "high", "image_url": "http://img.png"})");
  auto c = j.get<InputImageContent>();

  EXPECT_EQ(c.detail, "high");
  ASSERT_TRUE(c.image_url.has_value());
  EXPECT_EQ(*c.image_url, "http://img.png");
  EXPECT_FALSE(c.file_id.has_value());
}

TEST(InputContentTest, ImageContentWithDataAndMediaType) {
  auto j = json::parse(R"({"image_data": "aW1hZ2U=", "media_type": "image/jpeg"})");
  auto c = j.get<InputImageContent>();

  ASSERT_TRUE(c.image_data.has_value());
  EXPECT_EQ(*c.image_data, "aW1hZ2U=");
  ASSERT_TRUE(c.media_type.has_value());
  EXPECT_EQ(*c.media_type, "image/jpeg");
  EXPECT_FALSE(c.image_url.has_value());
}

TEST(InputContentTest, ImageContentDefaultDetail) {
  auto j = json::parse(R"({})");
  auto c = j.get<InputImageContent>();
  EXPECT_EQ(c.detail, "auto");
}

TEST(InputContentTest, FileContent) {
  auto j = json::parse(R"({"file_id": "f1", "filename": "data.csv"})");
  auto c = j.get<InputFileContent>();

  ASSERT_TRUE(c.file_id.has_value());
  EXPECT_EQ(*c.file_id, "f1");
  ASSERT_TRUE(c.filename.has_value());
  EXPECT_EQ(*c.filename, "data.csv");
  EXPECT_FALSE(c.file_data.has_value());
}

TEST(InputContentTest, AudioContent) {
  auto j = json::parse(R"({"data": "base64audio", "format": "wav"})");
  auto c = j.get<InputAudioContent>();
  EXPECT_EQ(c.data, "base64audio");
  EXPECT_EQ(c.format, "wav");
}

// ========================================================================
// from_json — InputMessage
// ========================================================================

TEST(InputMessageTest, StringContent) {
  auto j = json::parse(R"({"role": "user", "content": "hello"})");
  auto msg = j.get<InputMessage>();

  EXPECT_EQ(msg.role, "user");
  ASSERT_EQ(msg.content.size(), 1);
  auto* text = std::get_if<InputTextContent>(&msg.content[0]);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(text->text, "hello");
}

TEST(InputMessageTest, ArrayContentMultipleTypes) {
  auto j = json::parse(R"({
    "role": "user",
    "content": [
      {"type": "input_text", "text": "Look"},
      {"type": "input_image", "detail": "low"},
      {"type": "input_audio", "data": "abc", "format": "mp3"}
    ]
  })");
  auto msg = j.get<InputMessage>();

  ASSERT_EQ(msg.content.size(), 3);
  EXPECT_NE(std::get_if<InputTextContent>(&msg.content[0]), nullptr);
  EXPECT_NE(std::get_if<InputImageContent>(&msg.content[1]), nullptr);
  EXPECT_NE(std::get_if<InputAudioContent>(&msg.content[2]), nullptr);
}

TEST(InputMessageTest, MissingContentIsEmpty) {
  auto j = json::parse(R"({"role": "system"})");
  auto msg = j.get<InputMessage>();
  EXPECT_EQ(msg.role, "system");
  EXPECT_TRUE(msg.content.empty());
}

// ========================================================================
// from_json — FunctionCallResultInputItem
// ========================================================================

TEST(FunctionCallResultTest, Deserialize) {
  auto j = json::parse(R"({
    "type": "function_call_output",
    "call_id": "call_xyz",
    "output": "42"
  })");
  auto f = j.get<FunctionCallResultInputItem>();

  EXPECT_EQ(f.type, "function_call_output");
  EXPECT_EQ(f.call_id, "call_xyz");
  EXPECT_EQ(f.output, "42");
}

// ========================================================================
// from_json — Tool types (Responses API format)
// ========================================================================

TEST(ResponsesToolTest, FunctionDefinitionRoundTrip) {
  auto input = json::parse(R"({
    "name": "get_weather",
    "description": "Get current weather",
    "parameters": {"type": "object", "properties": {"city": {"type": "string"}}},
    "strict": true
  })");

  auto f = input.get<FunctionDefinition>();
  EXPECT_EQ(f.name, "get_weather");
  ASSERT_TRUE(f.description.has_value());
  EXPECT_EQ(*f.description, "Get current weather");
  ASSERT_TRUE(f.parameters_json.has_value());
  ASSERT_TRUE(f.strict.has_value());
  EXPECT_TRUE(*f.strict);

  // Round-trip to_json
  json output = f;
  EXPECT_EQ(output["name"], "get_weather");
  EXPECT_EQ(output["description"], "Get current weather");
  EXPECT_EQ(output["parameters"]["type"], "object");
  EXPECT_EQ(output["strict"], true);
}

TEST(ResponsesToolTest, ToolDefinitionFlatFormat) {
  auto j = json::parse(R"({
    "type": "function",
    "name": "search",
    "description": "Search docs",
    "parameters": {"type": "object"}
  })");

  auto t = j.get<ToolDefinition>();
  EXPECT_EQ(t.type, "function");
  EXPECT_EQ(t.function.name, "search");
  ASSERT_TRUE(t.function.description.has_value());
  EXPECT_EQ(*t.function.description, "Search docs");

  // to_json produces flat format
  json output = t;
  EXPECT_EQ(output["type"], "function");
  EXPECT_EQ(output["name"], "search");
  EXPECT_EQ(output["description"], "Search docs");
  EXPECT_FALSE(output.contains("function"));
}

TEST(ResponsesToolTest, ToolDefinitionNestedFormat) {
  auto j = json::parse(R"({
    "type": "function",
    "function": {"name": "nested_fn", "description": "via nested"}
  })");

  auto t = j.get<ToolDefinition>();
  EXPECT_EQ(t.function.name, "nested_fn");
}

TEST(ResponsesToolTest, ForcedFunctionRoundTrip) {
  auto j = json::parse(R"({"name": "must_call"})");
  auto f = j.get<ForcedFunction>();
  EXPECT_EQ(f.name, "must_call");

  json output = f;
  EXPECT_EQ(output["type"], "function");
  EXPECT_EQ(output["name"], "must_call");
}

// ========================================================================
// from_json — ResponseTextConfig, ReasoningConfig
// ========================================================================

TEST(ResponseConfigTest, TextConfigWithJsonSchema) {
  auto j = json::parse(R"({
    "format": {"type": "json_schema", "schema": {"type": "object"}}
  })");
  auto c = j.get<ResponseTextConfig>();

  EXPECT_EQ(c.format, "json_schema");
  ASSERT_TRUE(c.json_schema.has_value());
  EXPECT_FALSE(c.lark_grammar.has_value());

  // Round-trip
  json output = c;
  EXPECT_EQ(output["format"]["type"], "json_schema");
  EXPECT_EQ(output["format"]["schema"]["type"], "object");
}

TEST(ResponseConfigTest, TextConfigWithGrammar) {
  auto j = json::parse(R"({
    "format": {"type": "grammar", "grammar": "start: WORD+"}
  })");
  auto c = j.get<ResponseTextConfig>();

  EXPECT_EQ(c.format, "grammar");
  ASSERT_TRUE(c.lark_grammar.has_value());
  EXPECT_EQ(*c.lark_grammar, "start: WORD+");
}

TEST(ResponseConfigTest, TextConfigPlainText) {
  auto j = json::parse(R"({})");
  auto c = j.get<ResponseTextConfig>();
  EXPECT_EQ(c.format, "text");
}

TEST(ResponseConfigTest, ReasoningConfigRoundTrip) {
  auto j = json::parse(R"({"effort": "high", "generate_summary": true})");
  auto c = j.get<ReasoningConfig>();

  ASSERT_TRUE(c.effort.has_value());
  EXPECT_EQ(*c.effort, "high");
  ASSERT_TRUE(c.generate_summary.has_value());
  EXPECT_TRUE(*c.generate_summary);

  json output = c;
  EXPECT_EQ(output["effort"], "high");
  EXPECT_EQ(output["generate_summary"], true);
}

TEST(ResponseConfigTest, ReasoningConfigEmpty) {
  auto j = json::parse(R"({})");
  auto c = j.get<ReasoningConfig>();
  EXPECT_FALSE(c.effort.has_value());

  json output = c;
  EXPECT_FALSE(output.contains("effort"));
}

// ========================================================================
// from_json — ResponseCreateParams
// ========================================================================

TEST(ResponseCreateParamsTest, ArrayInputWithMessages) {
  auto j = json::parse(R"({
    "model": "m",
    "input": [
      {"role": "user", "content": "Hi"},
      {"type": "function_call_output", "call_id": "c1", "output": "42"}
    ]
  })");
  auto p = j.get<ResponseCreateParams>();

  auto* items = std::get_if<std::vector<InputItem>>(&p.input);
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), 2);
  EXPECT_NE(std::get_if<InputMessage>(&(*items)[0]), nullptr);
  EXPECT_NE(std::get_if<FunctionCallResultInputItem>(&(*items)[1]), nullptr);
}

TEST(ResponseCreateParamsTest, ToolsAndToolChoice) {
  auto j = json::parse(R"({
    "model": "m",
    "input": "x",
    "tools": [
      {"type": "function", "name": "fn1"}
    ],
    "tool_choice": {"name": "fn1"}
  })");
  auto p = j.get<ResponseCreateParams>();

  ASSERT_TRUE(p.tools.has_value());
  EXPECT_EQ(p.tools->size(), 1);

  ASSERT_TRUE(p.tool_choice.has_value());
  auto* forced = std::get_if<ForcedFunction>(&*p.tool_choice);
  ASSERT_NE(forced, nullptr);
  EXPECT_EQ(forced->name, "fn1");
}

TEST(ResponseCreateParamsTest, ToolChoiceString) {
  auto j = json::parse(R"({
    "model": "m",
    "input": "x",
    "tool_choice": "required"
  })");
  auto p = j.get<ResponseCreateParams>();

  ASSERT_TRUE(p.tool_choice.has_value());
  auto* s = std::get_if<std::string>(&*p.tool_choice);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(*s, "required");
}

// ========================================================================
// to_json — Output content types
// ========================================================================

TEST(OutputContentTest, TextContent) {
  OutputTextContent c;
  c.text = "Hello world";

  json j;
  to_json(j, c);
  EXPECT_EQ(j["type"], "output_text");
  EXPECT_EQ(j["text"], "Hello world");
}

TEST(OutputContentTest, RefusalContent) {
  OutputRefusalContent c;
  c.refusal = "I cannot do that";

  json j;
  to_json(j, c);
  EXPECT_EQ(j["type"], "refusal");
  EXPECT_EQ(j["refusal"], "I cannot do that");
}

TEST(OutputContentTest, AudioContent) {
  OutputAudioContent c;
  c.data = "base64data";
  c.transcript = "spoken text";

  json j;
  to_json(j, c);
  EXPECT_EQ(j["type"], "output_audio");
  EXPECT_EQ(j["data"], "base64data");
  EXPECT_EQ(j["transcript"], "spoken text");
}

// ========================================================================
// to_json — ResponseOutputMessage
// ========================================================================

TEST(ResponseOutputMessageTest, WithTextContent) {
  ResponseOutputMessage msg;
  msg.id = "msg_abc";
  msg.role = "assistant";
  msg.status = ResponseStatus::kCompleted;

  OutputTextContent tc;
  tc.text = "Answer";
  msg.content.push_back(tc);

  json j;
  to_json(j, msg);
  EXPECT_EQ(j["type"], "message");
  EXPECT_EQ(j["id"], "msg_abc");
  EXPECT_EQ(j["role"], "assistant");
  EXPECT_EQ(j["status"], "completed");
  ASSERT_EQ(j["content"].size(), 1);
  EXPECT_EQ(j["content"][0]["type"], "output_text");
  EXPECT_EQ(j["content"][0]["text"], "Answer");
}

// ========================================================================
// to_json — FunctionCallOutputItem
// ========================================================================

TEST(FunctionCallOutputItemTest, Serialization) {
  FunctionCallOutputItem f;
  f.id = "fc_1";
  f.type = "function_call";
  f.call_id = "call_abc";
  f.name = "get_weather";
  f.arguments = R"({"city":"Seattle"})";
  f.status = ResponseStatus::kCompleted;

  json j;
  to_json(j, f);
  EXPECT_EQ(j["type"], "function_call");
  EXPECT_EQ(j["id"], "fc_1");
  EXPECT_EQ(j["call_id"], "call_abc");
  EXPECT_EQ(j["name"], "get_weather");
  EXPECT_EQ(j["arguments"], R"({"city":"Seattle"})");
  EXPECT_EQ(j["status"], "completed");
}

// ========================================================================
// to_json — Usage types
// ========================================================================

TEST(ResponseUsageTest, Serialization) {
  ResponseUsage usage;
  usage.input_tokens = 10;
  usage.output_tokens = 20;
  usage.total_tokens = 30;
  usage.input_tokens_details.cached_tokens = 5;
  usage.output_tokens_details.reasoning_tokens = 2;

  json j;
  to_json(j, usage);
  EXPECT_EQ(j["input_tokens"], 10);
  EXPECT_EQ(j["output_tokens"], 20);
  EXPECT_EQ(j["total_tokens"], 30);
  EXPECT_EQ(j["input_tokens_details"]["cached_tokens"], 5);
  EXPECT_EQ(j["output_tokens_details"]["reasoning_tokens"], 2);
}

TEST(ResponseErrorTest, Serialization) {
  ResponseError e;
  e.code = "server_error";
  e.message = "Internal failure";

  json j;
  to_json(j, e);
  EXPECT_EQ(j["code"], "server_error");
  EXPECT_EQ(j["message"], "Internal failure");
}

// ========================================================================
// to_json — ResponseObject (full response)
// ========================================================================

TEST(ResponseObjectTest, EchoesRequestParameters) {
  ResponseObject r;
  r.id = "resp_1";
  r.created_at = 0;
  r.model = "m";
  r.temperature = 0.5f;
  r.top_p = 0.9f;
  r.max_output_tokens = 1024;
  r.store = true;
  r.parallel_tool_calls = false;
  r.metadata["key"] = "val";

  json j = r;

  EXPECT_FLOAT_EQ(j["temperature"].get<float>(), 0.5f);
  EXPECT_FLOAT_EQ(j["top_p"].get<float>(), 0.9f);
  EXPECT_EQ(j["max_output_tokens"], 1024);
  EXPECT_EQ(j["store"], true);
  EXPECT_EQ(j["parallel_tool_calls"], false);
  EXPECT_EQ(j["metadata"]["key"], "val");
}

TEST(ResponseObjectTest, DefaultParameterValues) {
  ResponseObject r;
  r.id = "resp_1";
  r.created_at = 0;
  r.model = "m";

  json j = r;

  // temperature/top_p default to 1.0 when not set
  EXPECT_FLOAT_EQ(j["temperature"].get<float>(), 1.0f);
  EXPECT_FLOAT_EQ(j["top_p"].get<float>(), 1.0f);
  EXPECT_FLOAT_EQ(j["presence_penalty"].get<float>(), 0.0f);
  EXPECT_FLOAT_EQ(j["frequency_penalty"].get<float>(), 0.0f);
  EXPECT_TRUE(j["max_output_tokens"].is_null());
  EXPECT_EQ(j["tool_choice"], "auto");
  EXPECT_TRUE(j["metadata"].is_object());
  EXPECT_TRUE(j["metadata"].empty());
}

TEST(ResponseObjectTest, ToolChoiceForcedFunction) {
  ResponseObject r;
  r.id = "resp_1";
  r.created_at = 0;
  r.model = "m";
  r.tool_choice = ForcedFunction{"my_fn"};

  json j = r;
  EXPECT_EQ(j["tool_choice"]["type"], "function");
  EXPECT_EQ(j["tool_choice"]["name"], "my_fn");
}

TEST(ResponseObjectTest, ToolChoiceStringValue) {
  ResponseObject r;
  r.id = "resp_1";
  r.created_at = 0;
  r.model = "m";
  r.tool_choice = std::string("required");

  json j = r;
  EXPECT_EQ(j["tool_choice"], "required");
}

TEST(ResponseObjectTest, WithError) {
  ResponseObject r;
  r.id = "resp_1";
  r.created_at = 0;
  r.model = "m";
  r.status = ResponseStatus::kFailed;
  r.error = ResponseError{"server_error", "Crash"};
  r.failed_at = 1700000005;

  json j = r;
  EXPECT_EQ(j["status"], "failed");
  EXPECT_EQ(j["error"]["code"], "server_error");
  EXPECT_EQ(j["error"]["message"], "Crash");
  EXPECT_EQ(j["failed_at"], 1700000005);
}

// ========================================================================
// to_json — StreamEvent
// ========================================================================

TEST(StreamEventTest, ResponseCreatedEvent) {
  ResponseObject resp;
  resp.id = "resp_1";
  resp.created_at = 0;
  resp.model = "m";

  StreamEvent e;
  e.type = StreamEventType::kResponseCreated;
  e.sequence_number = 0;
  e.response = resp;

  json j;
  to_json(j, e);
  EXPECT_EQ(j["type"], "response.created");
  EXPECT_EQ(j["sequence_number"], 0);
  ASSERT_TRUE(j.contains("response"));
  EXPECT_EQ(j["response"]["id"], "resp_1");
}

TEST(StreamEventTest, TextDeltaEvent) {
  StreamEvent e;
  e.type = StreamEventType::kTextDelta;
  e.sequence_number = 5;
  e.delta = "partial";
  e.output_index = 0;
  e.content_index = 0;
  e.item_id = "msg_1";

  json j;
  to_json(j, e);
  EXPECT_EQ(j["type"], "response.output_text.delta");
  EXPECT_EQ(j["delta"], "partial");
  EXPECT_EQ(j["output_index"], 0);
  EXPECT_EQ(j["content_index"], 0);
  EXPECT_EQ(j["item_id"], "msg_1");
}

TEST(StreamEventTest, TextDoneEvent) {
  StreamEvent e;
  e.type = StreamEventType::kTextDone;
  e.sequence_number = 10;
  e.text = "full text";
  e.output_index = 0;
  e.content_index = 0;
  e.item_id = "msg_1";

  json j;
  to_json(j, e);
  EXPECT_EQ(j["type"], "response.output_text.done");
  EXPECT_EQ(j["text"], "full text");
}

TEST(StreamEventTest, FunctionCallArgumentsDeltaEvent) {
  StreamEvent e;
  e.type = StreamEventType::kFunctionCallArgumentsDelta;
  e.sequence_number = 3;
  e.delta = "{\"ci";
  e.output_index = 1;
  e.item_id = "fc_1";
  e.function_call_id = "call_abc";

  json j;
  to_json(j, e);
  EXPECT_EQ(j["type"], "response.function_call_arguments.delta");
  EXPECT_EQ(j["delta"], "{\"ci");
  EXPECT_EQ(j["call_id"], "call_abc");
  EXPECT_EQ(j["item_id"], "fc_1");
}

TEST(StreamEventTest, FunctionCallArgumentsDoneEvent) {
  StreamEvent e;
  e.type = StreamEventType::kFunctionCallArgumentsDone;
  e.sequence_number = 4;
  e.output_index = 1;
  e.item_id = "fc_1";
  e.function_name = "get_weather";
  e.function_call_id = "call_abc";
  e.function_arguments = R"({"city":"Seattle"})";

  json j;
  to_json(j, e);
  EXPECT_EQ(j["type"], "response.function_call_arguments.done");
  EXPECT_EQ(j["name"], "get_weather");
  EXPECT_EQ(j["call_id"], "call_abc");
  EXPECT_EQ(j["arguments"], R"({"city":"Seattle"})");
}

TEST(StreamEventTest, ErrorEvent) {
  StreamEvent e;
  e.type = StreamEventType::kError;
  e.sequence_number = 99;
  e.error_code = "server_error";
  e.error_message = "Something broke";

  json j;
  to_json(j, e);
  EXPECT_EQ(j["type"], "error");
  EXPECT_EQ(j["error"]["code"], "server_error");
  EXPECT_EQ(j["error"]["message"], "Something broke");
}

TEST(StreamEventTest, OutputItemAddedEvent) {
  ResponseOutputMessage msg;
  msg.id = "msg_1";
  msg.role = "assistant";
  msg.status = ResponseStatus::kInProgress;

  StreamEvent e;
  e.type = StreamEventType::kOutputItemAdded;
  e.sequence_number = 2;
  e.item = msg;
  e.output_index = 0;

  json j;
  to_json(j, e);
  EXPECT_EQ(j["type"], "response.output_item.added");
  ASSERT_TRUE(j.contains("item"));
  EXPECT_EQ(j["item"]["type"], "message");
  EXPECT_EQ(j["item"]["id"], "msg_1");
  EXPECT_EQ(j["output_index"], 0);
}

TEST(StreamEventTest, ContentPartAddedEvent) {
  OutputTextContent tc;
  tc.text = "";

  StreamEvent e;
  e.type = StreamEventType::kContentPartAdded;
  e.sequence_number = 3;
  e.content_part = tc;
  e.output_index = 0;
  e.content_index = 0;
  e.item_id = "msg_1";

  json j;
  to_json(j, e);
  EXPECT_EQ(j["type"], "response.content_part.added");
  ASSERT_TRUE(j.contains("part"));
  EXPECT_EQ(j["part"]["type"], "output_text");
  EXPECT_EQ(j["content_index"], 0);
  EXPECT_EQ(j["item_id"], "msg_1");
}
