// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for response_converter.cc — BuildFailedResponseObject,
// BuildInitialResponseObject, EchoRequestParams (via Build*), and ToInputItems.
//
#include "inferencing/generative/openresponses/response_converter.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"

using namespace fl;
using namespace fl::responses;
using namespace fl::ResponseConverter;

// ========================================================================
// Helper: minimal ResponseCreateParams for echo tests
// ========================================================================

static ResponseCreateParams MakeTestParams() {
  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Hello");
  params.instructions = "Be helpful";
  params.temperature = 0.7f;
  params.top_p = 0.9f;
  params.max_output_tokens = 100;
  params.presence_penalty = 0.1f;
  params.frequency_penalty = 0.2f;
  params.store = true;
  params.metadata["key1"] = "value1";
  params.user = "test-user";
  return params;
}

TEST(ResponseConverterTest, FromSessionResponse_ReasoningOnlyMessageIsNotOutputText) {
  Response response;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("private scratchpad", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  response.items.push_back(
      std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));

  auto [output, output_text] = FromSessionResponse(response, "msg");

  ASSERT_EQ(output.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<ReasoningOutputItem>(output.front()));
  EXPECT_EQ(std::get<ReasoningOutputItem>(output.front()).summary.front().text, "private scratchpad");
  EXPECT_TRUE(output_text.empty());
}

TEST(ResponseConverterTest, FromSessionResponse_InterleavedReasoningPreservesOutputOrder) {
  Response response;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("think one", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  parts.push_back(std::make_unique<TextItem>("answer one", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
  parts.push_back(std::make_unique<TextItem>("think two", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  parts.push_back(std::make_unique<TextItem>("answer two", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
  response.items.push_back(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));

  auto [output, output_text] = FromSessionResponse(response, "msg");

  ASSERT_EQ(output.size(), 4u);
  EXPECT_TRUE(std::holds_alternative<ReasoningOutputItem>(output[0]));
  EXPECT_TRUE(std::holds_alternative<ResponseOutputMessage>(output[1]));
  EXPECT_TRUE(std::holds_alternative<ReasoningOutputItem>(output[2]));
  EXPECT_TRUE(std::holds_alternative<ResponseOutputMessage>(output[3]));
  EXPECT_EQ(output_text, "answer oneanswer two");
}

TEST(ResponseConverterTest, BuildFunctionCallStreamOutputEmitsCompleteLifecycle) {
  ToolCallItem call("call_test", "get_weather", R"({"city":"Seattle"})");
  int sequence_number = 7;

  auto output = BuildFunctionCallStreamOutput(call, 3, sequence_number);

  ASSERT_EQ(output.events.size(), 4u);
  EXPECT_EQ(sequence_number, 11);

  const auto& added = output.events[0];
  EXPECT_EQ(added.type, StreamEventType::kOutputItemAdded);
  EXPECT_EQ(added.sequence_number, 7);
  EXPECT_EQ(added.output_index, 3);
  ASSERT_TRUE(added.item.has_value());
  const auto& added_item = std::get<FunctionCallOutputItem>(*added.item);
  EXPECT_EQ(added_item.id, output.completed_item.id);
  EXPECT_EQ(added_item.call_id, "call_test");
  EXPECT_EQ(added_item.name, "get_weather");
  EXPECT_TRUE(added_item.arguments.empty());
  EXPECT_EQ(added_item.status, ResponseStatus::kInProgress);

  const auto& delta = output.events[1];
  EXPECT_EQ(delta.type, StreamEventType::kFunctionCallArgumentsDelta);
  EXPECT_EQ(delta.sequence_number, 8);
  EXPECT_EQ(delta.output_index, 3);
  EXPECT_EQ(delta.item_id, output.completed_item.id);
  EXPECT_EQ(delta.delta, R"({"city":"Seattle"})");
  EXPECT_EQ(delta.function_call_id, "call_test");

  const auto& arguments_done = output.events[2];
  EXPECT_EQ(arguments_done.type, StreamEventType::kFunctionCallArgumentsDone);
  EXPECT_EQ(arguments_done.sequence_number, 9);
  EXPECT_EQ(arguments_done.output_index, 3);
  EXPECT_EQ(arguments_done.item_id, output.completed_item.id);
  EXPECT_EQ(arguments_done.function_name, "get_weather");
  EXPECT_EQ(arguments_done.function_call_id, "call_test");
  EXPECT_EQ(arguments_done.function_arguments, R"({"city":"Seattle"})");

  const auto& item_done = output.events[3];
  EXPECT_EQ(item_done.type, StreamEventType::kOutputItemDone);
  EXPECT_EQ(item_done.sequence_number, 10);
  EXPECT_EQ(item_done.output_index, 3);
  ASSERT_TRUE(item_done.item.has_value());
  const auto& completed_item = std::get<FunctionCallOutputItem>(*item_done.item);
  EXPECT_EQ(completed_item.arguments, R"({"city":"Seattle"})");
  EXPECT_EQ(completed_item.status, ResponseStatus::kCompleted);
  EXPECT_EQ(output.completed_item.arguments, R"({"city":"Seattle"})");
  EXPECT_EQ(output.completed_item.status, ResponseStatus::kCompleted);
}

// ========================================================================
// BuildFailedResponseObject
// ========================================================================

TEST(ResponseConverterTest, BuildFailedResponse_HasErrorFields) {
  auto params = MakeTestParams();
  auto r = BuildFailedResponseObject("resp_123", 1000, "my-model", params,
                                     "server_error", "Something broke");

  EXPECT_EQ(r.id, "resp_123");
  EXPECT_EQ(r.created_at, 1000);
  EXPECT_EQ(r.model, "my-model");
  EXPECT_EQ(r.status, ResponseStatus::kFailed);
  ASSERT_TRUE(r.error.has_value());
  EXPECT_EQ(r.error->code, "server_error");
  EXPECT_EQ(r.error->message, "Something broke");
}

TEST(ResponseConverterTest, BuildFailedResponse_HasFailedAtTimestamp) {
  auto params = MakeTestParams();
  auto r = BuildFailedResponseObject("resp_1", 500, "m", params, "err", "msg");

  // failed_at should be set to a recent wall-clock time (not created_at)
  ASSERT_TRUE(r.failed_at.has_value());
  EXPECT_GT(*r.failed_at, 0);
}

TEST(ResponseConverterTest, BuildFailedResponse_OutputIsEmpty) {
  auto params = MakeTestParams();
  auto r = BuildFailedResponseObject("resp_1", 500, "m", params, "err", "msg");

  EXPECT_TRUE(r.output.empty());
  EXPECT_TRUE(r.output_text.empty());
}

TEST(ResponseConverterTest, BuildFailedResponse_EchoesRequestParams) {
  auto params = MakeTestParams();
  auto r = BuildFailedResponseObject("resp_1", 100, "m", params, "e", "m");

  EXPECT_EQ(r.instructions, "Be helpful");
  ASSERT_TRUE(r.temperature.has_value());
  EXPECT_FLOAT_EQ(*r.temperature, 0.7f);
  ASSERT_TRUE(r.top_p.has_value());
  EXPECT_FLOAT_EQ(*r.top_p, 0.9f);
  ASSERT_TRUE(r.max_output_tokens.has_value());
  EXPECT_EQ(*r.max_output_tokens, 100);
  ASSERT_TRUE(r.presence_penalty.has_value());
  EXPECT_FLOAT_EQ(*r.presence_penalty, 0.1f);
  ASSERT_TRUE(r.frequency_penalty.has_value());
  EXPECT_FLOAT_EQ(*r.frequency_penalty, 0.2f);
  EXPECT_TRUE(r.store);
  EXPECT_EQ(r.metadata.at("key1"), "value1");
  ASSERT_TRUE(r.user.has_value());
  EXPECT_EQ(*r.user, "test-user");
  EXPECT_EQ(r.truncation, "disabled");
}

// ========================================================================
// BuildInitialResponseObject
// ========================================================================

TEST(ResponseConverterTest, BuildInitialResponse_StatusIsInProgress) {
  auto params = MakeTestParams();
  auto r = BuildInitialResponseObject("resp_42", 2000, "streaming-model", params);

  EXPECT_EQ(r.id, "resp_42");
  EXPECT_EQ(r.created_at, 2000);
  EXPECT_EQ(r.model, "streaming-model");
  EXPECT_EQ(r.status, ResponseStatus::kInProgress);
}

TEST(ResponseConverterTest, BuildInitialResponse_NoCompletedOrFailedTimestamps) {
  auto params = MakeTestParams();
  auto r = BuildInitialResponseObject("resp_1", 100, "m", params);

  EXPECT_FALSE(r.completed_at.has_value());
  EXPECT_FALSE(r.failed_at.has_value());
}

TEST(ResponseConverterTest, BuildInitialResponse_EchoesRequestParams) {
  auto params = MakeTestParams();
  params.parallel_tool_calls = false;

  auto r = BuildInitialResponseObject("resp_1", 100, "m", params);

  EXPECT_EQ(r.instructions, "Be helpful");
  EXPECT_FALSE(r.parallel_tool_calls);
  EXPECT_TRUE(r.store);
}

// ========================================================================
// EchoRequestParams — tested indirectly via BuildResponseObject
// ========================================================================

TEST(ResponseConverterTest, EchoRequestParams_ToolsEchoed) {
  ResponseCreateParams params;
  params.model = "m";
  params.input = std::string("hi");

  responses::FunctionDefinition fn;
  fn.name = "get_weather";
  fn.description = "Get weather info";
  responses::ToolDefinition tool;
  tool.function = fn;
  params.tools = std::vector<responses::ToolDefinition>{tool};
  params.tool_choice = std::string("auto");

  auto r = BuildInitialResponseObject("resp_1", 100, "m", params);

  ASSERT_EQ(r.tools.size(), 1u);
  EXPECT_EQ(r.tools[0].function.name, "get_weather");
  ASSERT_TRUE(r.tool_choice.has_value());
  auto* tc_str = std::get_if<std::string>(&*r.tool_choice);
  ASSERT_NE(tc_str, nullptr);
  EXPECT_EQ(*tc_str, "auto");
}

TEST(ResponseConverterTest, EchoRequestParams_ParallelToolCallsDefaultTrue) {
  ResponseCreateParams params;
  params.model = "m";
  params.input = std::string("hi");
  // parallel_tool_calls not set → should default to true

  auto r = BuildInitialResponseObject("resp_1", 100, "m", params);
  EXPECT_TRUE(r.parallel_tool_calls);
}

TEST(ResponseConverterTest, EchoRequestParams_TextConfigEchoed) {
  ResponseCreateParams params;
  params.model = "m";
  params.input = std::string("hi");
  ResponseTextConfig text_cfg;
  text_cfg.format = "json_schema";
  text_cfg.json_schema = R"({"type":"object"})";
  params.text = text_cfg;

  auto r = BuildInitialResponseObject("resp_1", 100, "m", params);

  ASSERT_TRUE(r.text.has_value());
  EXPECT_EQ(r.text->format, "json_schema");
  ASSERT_TRUE(r.text->json_schema.has_value());
  EXPECT_EQ(*r.text->json_schema, R"({"type":"object"})");
}

TEST(ResponseConverterTest, EchoRequestParams_ReasoningConfigEchoed) {
  ResponseCreateParams params;
  params.model = "m";
  params.input = std::string("hi");
  ReasoningConfig rc;
  rc.effort = "high";
  rc.generate_summary = true;
  params.reasoning = rc;

  auto r = BuildInitialResponseObject("resp_1", 100, "m", params);

  ASSERT_TRUE(r.reasoning.has_value());
  ASSERT_TRUE(r.reasoning->effort.has_value());
  EXPECT_EQ(*r.reasoning->effort, "high");
  ASSERT_TRUE(r.reasoning->generate_summary.has_value());
  EXPECT_TRUE(*r.reasoning->generate_summary);
}

// ========================================================================
// ToInputItems
// ========================================================================

TEST(ResponseConverterTest, ToInputItems_StringInput) {
  nlohmann::json req = {{"input", "Hello world"}};
  auto items = ToInputItems(req);

  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0]["type"], "message");
  EXPECT_EQ(items[0]["role"], "user");
  EXPECT_EQ(items[0]["status"], "completed");
  EXPECT_EQ(items[0]["content"], "Hello world");
  // Should have a generated id
  EXPECT_TRUE(items[0].contains("id"));
  EXPECT_FALSE(items[0]["id"].get<std::string>().empty());
}

TEST(ResponseConverterTest, ToInputItems_WithInstructions) {
  nlohmann::json req = {{"instructions", "Be concise"}, {"input", "Hi"}};
  auto items = ToInputItems(req);

  ASSERT_EQ(items.size(), 2u);
  // First item is the system message from instructions
  EXPECT_EQ(items[0]["role"], "system");
  EXPECT_EQ(items[0]["content"], "Be concise");
  // Second is the user input
  EXPECT_EQ(items[1]["role"], "user");
  EXPECT_EQ(items[1]["content"], "Hi");
}

TEST(ResponseConverterTest, ToInputItems_ArrayInput_PreservesObjects) {
  nlohmann::json req = {
      {"input", nlohmann::json::array({
                    {{"type", "message"}, {"role", "user"}, {"content", "test"}},
                    {{"type", "function_call"}, {"name", "fn1"}, {"arguments", "{}"}},
                })}};

  auto items = ToInputItems(req);
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0]["type"], "message");
  EXPECT_EQ(items[1]["type"], "function_call");
}

TEST(ResponseConverterTest, ToInputItems_ArrayInput_GeneratesIdsWhenMissing) {
  nlohmann::json req = {
      {"input", nlohmann::json::array({
                    {{"type", "message"}, {"role", "user"}, {"content", "test"}},
                })}};

  auto items = ToInputItems(req);
  ASSERT_EQ(items.size(), 1u);
  // ID should be generated with "msg" prefix
  std::string id = items[0]["id"].get<std::string>();
  EXPECT_TRUE(id.find("msg_") == 0);
}

TEST(ResponseConverterTest, ToInputItems_ArrayInput_PreservesExistingIds) {
  nlohmann::json req = {
      {"input", nlohmann::json::array({
                    {{"type", "message"}, {"id", "existing_id"}, {"role", "user"}, {"content", "test"}},
                })}};

  auto items = ToInputItems(req);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0]["id"], "existing_id");
}

TEST(ResponseConverterTest, ToInputItems_NoInput_ReturnsEmptyArray) {
  nlohmann::json req = {{"model", "test"}};
  auto items = ToInputItems(req);
  EXPECT_TRUE(items.is_array());
  EXPECT_TRUE(items.empty());
}

TEST(ResponseConverterTest, ToInputItems_FunctionCallOutput_GetsFcoPrefix) {
  nlohmann::json req = {
      {"input", nlohmann::json::array({
                    {{"type", "function_call_output"}, {"call_id", "c1"}, {"output", "result"}},
                })}};

  auto items = ToInputItems(req);
  ASSERT_EQ(items.size(), 1u);
  std::string id = items[0]["id"].get<std::string>();
  EXPECT_TRUE(id.find("fco_") == 0);
}

// ========================================================================
// ToSessionRequest — vision input (input_image content)
//
// Verifies that input_image content parts are decoded into owning ImageItems
// and combined with adjacent input_text into MessageItem with typed parts.
// ========================================================================

namespace {

// "PNG\0" magic + a few bytes — just to give Base64Decode something
// non-trivial to round-trip and to provide a known byte count.
constexpr const char* kSamplePngBase64 = "iVBORw0KGgoAAAA=";  // 11 bytes decoded
constexpr size_t kSamplePngDecodedSize = 11;

ResponseCreateParams MakeImageRequest(const std::string& image_url, const std::string& text = "What is this?") {
  ResponseCreateParams params;
  params.model = "test-model";

  InputMessage msg;
  msg.role = "user";
  InputTextContent text_part;
  text_part.text = text;
  msg.content.push_back(text_part);
  InputImageContent image_part;
  image_part.detail = "auto";
  image_part.image_url = image_url;
  msg.content.push_back(image_part);

  params.input = std::vector<InputItem>{msg};
  return params;
}

ResponseCreateParams MakeImageDataRequest(const std::string& image_data,
                                          const std::optional<std::string>& media_type = std::nullopt) {
  ResponseCreateParams params;
  params.model = "test-model";

  InputMessage msg;
  msg.role = "user";
  InputImageContent image_part;
  image_part.detail = "auto";
  image_part.image_data = image_data;
  image_part.media_type = media_type;
  msg.content.push_back(image_part);

  params.input = std::vector<InputItem>{msg};
  return params;
}

}  // namespace

TEST(ResponseConverterTest, ToSessionRequest_InputImage_DataUrl_DecodesToImageItem) {
  std::string data_url = std::string("data:image/png;base64,") + kSamplePngBase64;
  auto params = MakeImageRequest(data_url);

  auto request = ToSessionRequest(params);

  // Expect a single MessageItem with [TextItem, ImageItem] parts.
  ASSERT_EQ(request.items.size(), 1u);
  auto* msg = dynamic_cast<MessageItem*>(request.items[0]);
  ASSERT_NE(msg, nullptr);
  ASSERT_EQ(msg->content.size(), 2u);

  // Part 0: text
  ASSERT_NE(msg->content[0].view, nullptr);
  EXPECT_EQ(msg->content[0].view->type, FOUNDRY_LOCAL_ITEM_TEXT);

  // Part 1: image with decoded bytes + correct MIME type.
  ASSERT_NE(msg->content[1].view, nullptr);
  ASSERT_EQ(msg->content[1].view->type, FOUNDRY_LOCAL_ITEM_IMAGE);
  const auto* img = static_cast<const ImageItem*>(msg->content[1].view);
  EXPECT_EQ(img->format, "image/png");
  EXPECT_EQ(img->data_size, kSamplePngDecodedSize);
  EXPECT_NE(img->data, nullptr);
}

TEST(ResponseConverterTest, ToSessionRequest_InputAudio_DecodesToAudioItem) {
  ResponseCreateParams params;
  params.model = "test-model";

  InputMessage msg;
  msg.role = "user";
  InputTextContent text_part;
  text_part.text = "Transcribe this audio.";
  msg.content.push_back(text_part);
  InputAudioContent audio_part;
  audio_part.data = "AQIDBA==";
  audio_part.format = "wav";
  msg.content.push_back(audio_part);
  params.input = std::vector<InputItem>{msg};

  auto request = ToSessionRequest(params);

  ASSERT_EQ(request.items.size(), 1u);
  auto* message = dynamic_cast<MessageItem*>(request.items[0]);
  ASSERT_NE(message, nullptr);
  ASSERT_EQ(message->content.size(), 2u);
  ASSERT_EQ(message->content[1].view->type, FOUNDRY_LOCAL_ITEM_AUDIO);
  const auto* audio = static_cast<const AudioItem*>(message->content[1].view);
  EXPECT_EQ(audio->format, "wav");
  ASSERT_EQ(audio->data_size, 4u);
  const auto* bytes = static_cast<const std::uint8_t*>(audio->data);
  EXPECT_EQ(bytes[0], 1u);
  EXPECT_EQ(bytes[3], 4u);
}

TEST(ResponseConverterTest, ToSessionRequest_InputAudio_RejectsInvalidBase64) {
  ResponseCreateParams params;
  params.model = "test-model";

  InputMessage msg;
  msg.role = "user";
  InputAudioContent audio_part;
  audio_part.data = "not-base64!";
  audio_part.format = "wav";
  msg.content.push_back(audio_part);
  params.input = std::vector<InputItem>{msg};

  try {
    ToSessionRequest(params);
    FAIL() << "Expected invalid base64 audio data to be rejected";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

TEST(ResponseConverterTest, ToSessionRequest_InputAudio_RejectsEmptyData) {
  ResponseCreateParams params;
  params.model = "test-model";

  InputMessage msg;
  msg.role = "user";
  InputAudioContent audio_part;
  audio_part.format = "wav";
  msg.content.push_back(audio_part);
  params.input = std::vector<InputItem>{msg};

  try {
    ToSessionRequest(params);
    FAIL() << "Expected empty audio data to be rejected";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

TEST(ResponseConverterTest, ToSessionRequest_InputAudio_RejectsEmptyFormat) {
  ResponseCreateParams params;
  params.model = "test-model";

  InputMessage msg;
  msg.role = "user";
  InputAudioContent audio_part;
  audio_part.data = "AQIDBA==";
  msg.content.push_back(audio_part);
  params.input = std::vector<InputItem>{msg};

  try {
    ToSessionRequest(params);
    FAIL() << "Expected empty audio format to be rejected";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

TEST(ResponseConverterTest, ToSessionRequest_InputImage_ImageData_DecodesWithMediaType) {
  auto params = MakeImageDataRequest(kSamplePngBase64, "image/jpeg");

  auto request = ToSessionRequest(params);

  auto* msg = dynamic_cast<MessageItem*>(request.items[0]);
  ASSERT_NE(msg, nullptr);
  ASSERT_EQ(msg->content.size(), 2u);
  const auto* img = static_cast<const ImageItem*>(msg->content[0].view);
  EXPECT_EQ(img->format, "image/jpeg");
  EXPECT_EQ(img->data_size, kSamplePngDecodedSize);
}

TEST(ResponseConverterTest, ToSessionRequest_InputImage_ImageData_DefaultsToPng) {
  auto params = MakeImageDataRequest(kSamplePngBase64);

  auto request = ToSessionRequest(params);

  auto* msg = dynamic_cast<MessageItem*>(request.items[0]);
  ASSERT_NE(msg, nullptr);
  const auto* img = static_cast<const ImageItem*>(msg->content[0].view);
  EXPECT_EQ(img->format, "image/png");
}

TEST(ResponseConverterTest, ToSessionRequest_InputImage_InvalidImageData_NamesSourceField) {
  auto params = MakeImageDataRequest("not-valid-base64");

  try {
    ToSessionRequest(params);
    FAIL() << "Expected invalid image_data to throw";
  } catch (const std::exception& e) {
    const std::string message = e.what();
    EXPECT_NE(message.find("image_data"), std::string::npos);
    EXPECT_EQ(message.find("image_url"), std::string::npos);
  }
}

TEST(ResponseConverterTest, ToSessionRequest_InputImage_ImageUrlTakesPrecedenceOverImageData) {
  std::string data_url = std::string("data:image/png;base64,") + kSamplePngBase64;
  auto params = MakeImageRequest(data_url);
  auto& items = std::get<std::vector<InputItem>>(params.input);
  auto& msg = std::get<InputMessage>(items[0]);
  auto& image = std::get<InputImageContent>(msg.content[1]);
  image.image_data = "not-valid-base64";
  image.media_type = "image/jpeg";

  auto request = ToSessionRequest(params);

  auto* converted_msg = dynamic_cast<MessageItem*>(request.items[0]);
  ASSERT_NE(converted_msg, nullptr);
  const auto* img = static_cast<const ImageItem*>(converted_msg->content[1].view);
  EXPECT_EQ(img->format, "image/png");
}

TEST(ResponseConverterTest, ToSessionRequest_InputImage_DataUrl_MissingBase64Marker_Throws) {
  // Missing ";base64," — the converter requires base64 encoding.
  auto params = MakeImageRequest("data:image/png,plaintext");
  EXPECT_THROW(ToSessionRequest(params), std::exception);
}

TEST(ResponseConverterTest, ToSessionRequest_InputImage_HttpUrl_NotImplemented) {
  auto params = MakeImageRequest("https://example.com/image.png");
  EXPECT_THROW(ToSessionRequest(params), std::exception);
}

TEST(ResponseConverterTest, ToSessionRequest_InputImage_FileId_NotImplemented) {
  ResponseCreateParams params;
  params.model = "test-model";

  InputMessage msg;
  msg.role = "user";
  InputImageContent image_part;
  image_part.detail = "auto";
  image_part.file_id = "file_abc123";
  msg.content.push_back(image_part);
  params.input = std::vector<InputItem>{msg};

  EXPECT_THROW(ToSessionRequest(params), std::exception);
}

TEST(ResponseConverterTest, ToSessionRequest_InputImage_ImageOnlyMessage_GetsTextSentinel) {
  // Pure-image message (no text). The converter injects a single-space
  // text part so the chat template can render the message.
  std::string data_url = std::string("data:image/jpeg;base64,") + kSamplePngBase64;

  ResponseCreateParams params;
  params.model = "test-model";

  InputMessage msg;
  msg.role = "user";
  InputImageContent image_part;
  image_part.detail = "auto";
  image_part.image_url = data_url;
  msg.content.push_back(image_part);
  params.input = std::vector<InputItem>{msg};

  auto request = ToSessionRequest(params);

  ASSERT_EQ(request.items.size(), 1u);
  auto* m = dynamic_cast<MessageItem*>(request.items[0]);
  ASSERT_NE(m, nullptr);
  ASSERT_EQ(m->content.size(), 2u);

  // The image part is added first, then the text sentinel.
  EXPECT_EQ(m->content[0].view->type, FOUNDRY_LOCAL_ITEM_IMAGE);
  EXPECT_EQ(m->content[1].view->type, FOUNDRY_LOCAL_ITEM_TEXT);
  const auto* img = static_cast<const ImageItem*>(m->content[0].view);
  EXPECT_EQ(img->format, "image/jpeg");
}

// ========================================================================
// ExtractResponsesToolDefinitions / ToSessionRequest tool_choice
//
// Verifies that the typed tool_choice variant and tools array reach the
// session request the same way the chat-completions path does. Regression
// guard for the bug where ResponseConverter dropped tools + tool_choice,
// causing small models on the Responses path to ignore `required`.
// ========================================================================

namespace {

responses::ToolDefinition MakeTool(const std::string& name, const std::string& desc = "") {
  responses::ToolDefinition td;
  td.type = "function";
  td.function.name = name;
  if (!desc.empty()) {
    td.function.description = desc;
  }
  td.function.parameters_json = R"({"type":"object","properties":{}})";
  return td;
}

ResponseCreateParams MakeToolParams() {
  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("hi");
  return params;
}

}  // namespace

TEST(ResponseConverterTest, ToSessionRequest_ToolChoiceString_PropagatesToOptions) {
  for (const auto& choice : {std::string("auto"), std::string("none"), std::string("required")}) {
    auto params = MakeToolParams();
    params.tools = std::vector<responses::ToolDefinition>{MakeTool("get_weather")};
    params.tool_choice = choice;

    Request req = ToSessionRequest(params);
    (void)ExtractResponsesToolDefinitions(params, req);

    const char* opt = req.options.Find("tool_choice");
    ASSERT_NE(opt, nullptr) << "tool_choice='" << choice << "' should land in options";
    EXPECT_EQ(std::string(opt), choice);
  }
}

TEST(ResponseConverterTest, ToSessionRequest_NoToolChoice_DoesNotSetOption) {
  auto params = MakeToolParams();
  params.tools = std::vector<responses::ToolDefinition>{MakeTool("get_weather")};
  // tool_choice intentionally absent

  Request req = ToSessionRequest(params);
  (void)ExtractResponsesToolDefinitions(params, req);

  EXPECT_EQ(req.options.Find("tool_choice"), nullptr);
}

TEST(ResponseConverterTest, ExtractResponsesToolDefinitions_NoTools_ReturnsEmpty) {
  auto params = MakeToolParams();
  // No tools, no tool_choice.

  Request req;
  std::string tools_json = ExtractResponsesToolDefinitions(params, req);

  EXPECT_TRUE(tools_json.empty());
  EXPECT_EQ(req.options.Find("tool_choice"), nullptr);

  // Empty vector should also produce empty json.
  params.tools = std::vector<responses::ToolDefinition>{};
  Request req2;
  EXPECT_TRUE(ExtractResponsesToolDefinitions(params, req2).empty());
}

TEST(ResponseConverterTest, ExtractResponsesToolDefinitions_ToolChoiceString_SerializesAllTools) {
  auto params = MakeToolParams();
  params.tools = std::vector<responses::ToolDefinition>{MakeTool("tool_a", "first"), MakeTool("tool_b", "second")};
  params.tool_choice = std::string("auto");

  Request req;
  std::string tools_json = ExtractResponsesToolDefinitions(params, req);

  ASSERT_FALSE(tools_json.empty());
  auto j = nlohmann::json::parse(tools_json);
  ASSERT_TRUE(j.is_array());
  ASSERT_EQ(j.size(), 2u);

  // Chat-template (OpenAI nested) format expected by ChatSession::BuildToolCallContext.
  EXPECT_EQ(j[0]["type"], "function");
  EXPECT_EQ(j[0]["function"]["name"], "tool_a");
  EXPECT_EQ(j[0]["function"]["description"], "first");
  EXPECT_TRUE(j[0]["function"].contains("parameters"));
  EXPECT_EQ(j[1]["function"]["name"], "tool_b");

  const char* opt = req.options.Find("tool_choice");
  ASSERT_NE(opt, nullptr);
  EXPECT_EQ(std::string(opt), "auto");
}

TEST(ResponseConverterTest, ExtractResponsesToolDefinitions_ForcedFunction_FiltersToNamedToolAndSetsRequired) {
  auto params = MakeToolParams();
  params.tools = std::vector<responses::ToolDefinition>{MakeTool("tool_a"), MakeTool("tool_b")};
  params.tool_choice = ForcedFunction{"tool_b"};

  Request req;
  std::string tools_json = ExtractResponsesToolDefinitions(params, req);

  ASSERT_FALSE(tools_json.empty());
  auto j = nlohmann::json::parse(tools_json);
  ASSERT_TRUE(j.is_array());
  ASSERT_EQ(j.size(), 1u);
  EXPECT_EQ(j[0]["function"]["name"], "tool_b");

  const char* opt = req.options.Find("tool_choice");
  ASSERT_NE(opt, nullptr);
  EXPECT_EQ(std::string(opt), "required");
}

TEST(ResponseConverterTest, ExtractResponsesToolDefinitions_AllowedTools_FiltersTools) {
  auto params = MakeToolParams();
  params.tools = std::vector<responses::ToolDefinition>{
      MakeTool("tool_a"), MakeTool("tool_b"), MakeTool("tool_c")};
  params.tool_choice = std::string("auto");
  params.allowed_tools = std::vector<std::string>{"tool_b", "tool_c"};

  Request req;
  std::string tools_json = ExtractResponsesToolDefinitions(params, req);

  ASSERT_FALSE(tools_json.empty());
  auto j = nlohmann::json::parse(tools_json);
  ASSERT_TRUE(j.is_array());
  ASSERT_EQ(j.size(), 2u);
  EXPECT_EQ(j[0]["function"]["name"], "tool_b");
  EXPECT_EQ(j[1]["function"]["name"], "tool_c");
}

TEST(ResponseConverterTest, ExtractResponsesToolDefinitions_AllowedToolsAndForcedFunction_BothApplied) {
  // Forced function names tool_c, but allowed_tools only permits tool_a and tool_b.
  // Result: empty tools (strict intersection — matches C# behaviour). tool_choice still
  // gets "required" since the forced-function branch sets it before allowed_tools runs;
  // an empty tools array combined with "required" effectively disables tool calling,
  // which is the intended consequence of an over-restricted allowed_tools list.
  auto params = MakeToolParams();
  params.tools = std::vector<responses::ToolDefinition>{
      MakeTool("tool_a"), MakeTool("tool_b"), MakeTool("tool_c")};
  params.tool_choice = ForcedFunction{"tool_c"};
  params.allowed_tools = std::vector<std::string>{"tool_a", "tool_b"};

  Request req;
  std::string tools_json = ExtractResponsesToolDefinitions(params, req);

  EXPECT_TRUE(tools_json.empty());

  const char* opt = req.options.Find("tool_choice");
  ASSERT_NE(opt, nullptr);
  EXPECT_EQ(std::string(opt), "required");
}

TEST(ResponseConverterTest, ExtractResponsesToolDefinitions_AllowedTools_CaseInsensitive) {
  auto params = MakeToolParams();
  params.tools = std::vector<responses::ToolDefinition>{MakeTool("GetWeather")};
  params.allowed_tools = std::vector<std::string>{"getweather"};

  Request req;
  std::string tools_json = ExtractResponsesToolDefinitions(params, req);

  ASSERT_FALSE(tools_json.empty());
  auto j = nlohmann::json::parse(tools_json);
  ASSERT_TRUE(j.is_array());
  ASSERT_EQ(j.size(), 1u);
  EXPECT_EQ(j[0]["function"]["name"], "GetWeather");
}

// ========================================================================
// Round-trip checklist: every option ToSessionRequest is supposed to map
// must appear in session_request.options. Adding a new option mapping to
// the converter requires extending this test (treat the test as the spec).
// ========================================================================

TEST(ResponseConverterTest, ToSessionRequest_AllRequestOptions_PropagatedToSessionOptions) {
  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("hello");
  params.temperature = 0.5f;
  params.top_p = 0.95f;
  params.max_output_tokens = 256;
  params.presence_penalty = 0.25f;
  params.frequency_penalty = 0.75f;
  params.seed = 42;

  ResponseTextConfig text_cfg;
  text_cfg.format = "json_schema";
  text_cfg.json_schema = R"({"type":"object"})";
  params.text = text_cfg;

  params.tools = std::vector<responses::ToolDefinition>{MakeTool("get_weather")};
  params.tool_choice = std::string("required");

  Request req = ToSessionRequest(params);
  std::string tools_json = ExtractResponsesToolDefinitions(params, req);

  auto expect_opt = [&](const char* key, const std::string& expected) {
    const char* val = req.options.Find(key);
    ASSERT_NE(val, nullptr) << "missing option: " << key;
    EXPECT_EQ(std::string(val), expected) << "option key: " << key;
  };

  expect_opt("temperature", std::to_string(0.5f));
  expect_opt("top_p", std::to_string(0.95f));
  expect_opt("max_output_tokens", "256");
  expect_opt("presence_penalty", std::to_string(0.25f));
  expect_opt("frequency_penalty", std::to_string(0.75f));
  expect_opt("seed", "42");
  expect_opt("guidance_type", "json_schema");
  expect_opt("guidance_data", R"({"type":"object"})");
  expect_opt("tool_choice", "required");

  EXPECT_FALSE(tools_json.empty());
}