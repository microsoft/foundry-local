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
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "items/tool_result_item.h"

using namespace fl;
using namespace fl::responses;
using namespace fl::ResponseConverter;
using json = nlohmann::json;

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

TEST(ResponseConverterTest, ToInputItems_InstructionsAreNotStoredAsAnItem) {
  // /input_items reports what the caller put in `input`. Instructions are request-scoped state, not an input item,
  // and synthesizing one made the endpoint report something the caller never sent.
  nlohmann::json req = {{"instructions", "Be concise"}, {"input", "Hi"}};
  auto items = ToInputItems(req);

  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0]["role"], "user");
  EXPECT_EQ(items[0]["content"], "Hi");
}

TEST(ResponseConverterTest, ToInputItems_InstructionsOnlyRequestStoresNothing) {
  nlohmann::json req = {{"instructions", "Be concise"}};
  EXPECT_TRUE(ToInputItems(req).empty());
}

TEST(ResponseConverterTest, ToInputItems_CallerSystemMessageIsStoredVerbatim) {
  // A caller system message is ordinary conversation content and is stored exactly as sent, even when its text
  // happens to match the request's instructions.
  nlohmann::json req = {
      {"instructions", "Be concise"},
      {"input", nlohmann::json::array({
                    {{"type", "message"}, {"role", "system"}, {"content", "Be concise"}},
                    {{"type", "message"}, {"role", "user"}, {"content", "Hi"}},
                })},
  };

  auto items = ToInputItems(req);

  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0]["role"], "system");
  EXPECT_EQ(items[0]["content"], "Be concise");
  EXPECT_EQ(items[1]["content"], "Hi");
}

TEST(ResponseConverterTest, ToInputItems_ReasoningItemIsStoredVerbatim) {
  // /input_items must keep reporting a reasoning item the caller echoed back, unchanged.
  nlohmann::json req = {
      {"input", nlohmann::json::array({
                    {{"type", "reasoning"},
                     {"id", "rs_1"},
                     {"summary", nlohmann::json::array({{{"type", "summary_text"}, {"text", "private"}}})}},
                })},
  };

  auto items = ToInputItems(req);

  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0]["type"], "reasoning");
  EXPECT_EQ(items[0]["id"], "rs_1");
  EXPECT_EQ(items[0]["summary"][0]["text"], "private");
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
  expect_opt("seed", "42");
  expect_opt("guidance_type", "json_schema");
  expect_opt("guidance_data", R"({"type":"object"})");
  expect_opt("tool_choice", "required");

  EXPECT_FALSE(tools_json.empty());
}

TEST(ResponseConverterTest, ToSessionRequest_RejectsNonzeroPenalties) {
  for (const auto& [frequency, presence] :
       {std::pair{0.75f, 0.0f}, std::pair{0.0f, 0.25f}, std::pair{-0.75f, 0.0f}, std::pair{0.0f, -0.25f}}) {
    ResponseCreateParams params;
    params.model = "test-model";
    params.input = std::string("hello");
    params.frequency_penalty = frequency;
    params.presence_penalty = presence;

    EXPECT_THROW(ToSessionRequest(params), fl::Exception);
  }
}

TEST(ResponseConverterTest, ToSessionRequest_ZeroPenaltiesAreNoOps) {
  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("hello");
  params.presence_penalty = 0.0f;
  params.frequency_penalty = 0.0f;

  Request req = ToSessionRequest(params);

  EXPECT_EQ(req.options.Find("presence_penalty"), nullptr);
  EXPECT_EQ(req.options.Find("frequency_penalty"), nullptr);
}

// ========================================================================
// ToSessionRequest — tool call replay
//
// A caller continues a tool-calling conversation either by chaining to a
// stored response (previous_output carries `function_call` entries) or by
// sending the call back in the request `input`. Both forms must survive as
// ToolCallItems so the session can correlate the results that follow.
// ========================================================================

TEST(ResponseConverterTest, ToSessionRequest_StoredReplayPreservesFunctionCalls) {
  ResponseCreateParams params;
  params.model = "test-model";

  std::vector<InputItem> input;
  FunctionCallResultInputItem result;
  result.call_id = "call_1";
  result.output = "sunny";
  input.push_back(result);
  params.input = std::move(input);

  ResponseChainContext previous_context{
      ResponseChainHop{nlohmann::json::array(), nlohmann::json::parse(R"([
    {"type": "message", "role": "assistant", "content": [{"type": "output_text", "text": "Let me check."}]},
    {"type": "function_call", "call_id": "call_1", "name": "get_weather", "arguments": "{\"city\":\"Seattle\"}"}
  ])")}};

  auto request = ToSessionRequest(params, &previous_context);

  ASSERT_EQ(request.items.size(), 3u);
  EXPECT_EQ(request.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);

  ASSERT_EQ(request.items[1]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  auto* call = static_cast<ToolCallItem*>(request.items[1]);
  EXPECT_EQ(call->call_id, "call_1");
  EXPECT_EQ(call->name, "get_weather");
  EXPECT_EQ(call->arguments, R"({"city":"Seattle"})");

  ASSERT_EQ(request.items[2]->type, FOUNDRY_LOCAL_ITEM_TOOL_RESULT);
  auto* tool_result = static_cast<ToolResultItem*>(request.items[2]);
  EXPECT_EQ(tool_result->call_id, "call_1");
  EXPECT_EQ(tool_result->result, "sunny");
}

TEST(ResponseConverterTest, ToSessionRequest_FunctionCallInputItemBecomesToolCallItem) {
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"type": "function_call", "call_id": "call_1", "name": "get_weather", "arguments": "{\"city\":\"Seattle\"}"},
      {"type": "function_call_output", "call_id": "call_1", "output": ""}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ToSessionRequest(params);

  ASSERT_EQ(request.items.size(), 2u);

  ASSERT_EQ(request.items[0]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  auto* call = static_cast<ToolCallItem*>(request.items[0]);
  EXPECT_EQ(call->call_id, "call_1");
  EXPECT_EQ(call->name, "get_weather");
  EXPECT_EQ(call->arguments, R"({"city":"Seattle"})");

  ASSERT_EQ(request.items[1]->type, FOUNDRY_LOCAL_ITEM_TOOL_RESULT);
  auto* tool_result = static_cast<ToolResultItem*>(request.items[1]);
  EXPECT_EQ(tool_result->call_id, "call_1");
  EXPECT_EQ(tool_result->result, "");
}

TEST(ResponseConverterTest, ToSessionRequest_ReconstructedChainCorrelatesCallAndResultAfterCacheMiss) {
  // The session cache dropped the conversation, so the whole chain is rebuilt from the store and replayed. The
  // assistant tool call must reach the request ahead of the result that answers it, and the new user turn last.
  ResponseStore store;

  json first_response;
  first_response["id"] = "resp_1";
  first_response["previous_response_id"] = nullptr;
  first_response["output"] = json::array({{{"type", "function_call"},
                                           {"call_id", "call_1"},
                                           {"name", "get_weather"},
                                           {"arguments", R"({"city":"Seattle"})"}}});
  store.Store("resp_1", first_response,
              json::array({{{"type", "message"}, {"role", "user"}, {"content", "weather?"}}}));

  json second_response;
  second_response["id"] = "resp_2";
  second_response["previous_response_id"] = "resp_1";
  second_response["output"] =
      json::array({{{"type", "message"}, {"role", "assistant"}, {"content", "It is sunny."}}});
  store.Store("resp_2", second_response,
              json::array({{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "sunny"}}}));

  auto context = store.BuildChainContext("resp_2");
  ASSERT_TRUE(context.has_value());

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("And tomorrow?");

  auto request = ToSessionRequest(params, &(*context));

  ASSERT_EQ(request.items.size(), 5u);
  EXPECT_EQ(request.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);

  ASSERT_EQ(request.items[1]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  EXPECT_EQ(static_cast<ToolCallItem*>(request.items[1])->call_id, "call_1");
  EXPECT_EQ(static_cast<ToolCallItem*>(request.items[1])->arguments, R"({"city":"Seattle"})");

  ASSERT_EQ(request.items[2]->type, FOUNDRY_LOCAL_ITEM_TOOL_RESULT);
  EXPECT_EQ(static_cast<ToolResultItem*>(request.items[2])->call_id, "call_1");

  EXPECT_EQ(request.items[3]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(static_cast<MessageItem*>(request.items[4])->GetSimpleText(), "And tomorrow?");

  // The replayed context is coherent: the transcript accepts it, with the call already answered.
  auto messages = BuildTranscriptMessages(request.items);
  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateInputs(messages));
}

TEST(ResponseConverterTest, ToSessionRequest_ReplayedCallWithUnusableArgumentsIsNormalizedNotRejected) {
  // The service replaying its own earlier output must not fail because the model once emitted argument bytes that
  // are not a JSON object — the model was already shown that call as having no arguments.
  ResponseChainContext previous_context{
      ResponseChainHop{nlohmann::json::parse(R"([{"type": "message", "role": "user", "content": "weather?"}])"),
                       nlohmann::json::parse(R"([
    {"type": "function_call", "call_id": "call_1", "name": "get_weather", "arguments": "[1,2]"}
  ])")},
      ResponseChainHop{
          nlohmann::json::parse(R"([{"type": "function_call_output", "call_id": "call_1", "output": "sunny"}])"),
          nlohmann::json::parse(R"([{"type": "message", "role": "assistant", "content": "It is sunny."}])")}};

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("And tomorrow?");

  Request request;
  ASSERT_NO_THROW(request = ToSessionRequest(params, &previous_context));

  ASSERT_EQ(request.items.size(), 5u);
  ASSERT_EQ(request.items[1]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  EXPECT_EQ(static_cast<ToolCallItem*>(request.items[1])->arguments, "");

  // The replayed conversation is coherent and the strict transcript path accepts it.
  auto messages = BuildTranscriptMessages(request.items);
  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateInputs(messages));
}

TEST(ResponseConverterTest, ToSessionRequest_CallerSuppliedUnusableArgumentsAreStillRejected) {
  // The same bytes arriving in the request `input` are a client error, not a replay of our own output.
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"type": "function_call", "call_id": "call_1", "name": "get_weather", "arguments": "[1,2]"}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ToSessionRequest(params);

  ASSERT_EQ(request.items.size(), 1u);
  EXPECT_EQ(static_cast<ToolCallItem*>(request.items[0])->arguments, "[1,2]");
  EXPECT_THROW(BuildTranscriptMessages(request.items), fl::Exception);
}

// ========================================================================
// Typed stateless replay — a caller resending the whole conversation in
// `input` instead of chaining via previous_response_id must reach the same
// prompt as chain reconstruction does.
// ========================================================================

TEST(ResponseConverterTest, ToSessionRequest_TypedReplayPreservesAssistantOutputTextAndToolExchange) {
  // Exactly what the Responses API emitted on earlier turns, echoed back by the caller: an assistant message with
  // `output_text` parts, the function_call it issued, and the function_call_output answering it.
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
      {"role": "assistant", "content": [{"type": "output_text", "text": "Hello there."}]},
      {"role": "user", "content": [{"type": "input_text", "text": "Weather in Seattle?"}]},
      {"role": "assistant", "content": [{"type": "output_text", "text": "Let me check."}]},
      {"type": "function_call", "call_id": "call_1", "name": "get_weather",
       "arguments": "{\"city\":\"Seattle\"}"},
      {"type": "function_call_output", "call_id": "call_1", "output": "sunny"},
      {"role": "user", "content": [{"type": "input_text", "text": "And tomorrow?"}]}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ToSessionRequest(params);

  ASSERT_EQ(request.items.size(), 7u);
  EXPECT_EQ(static_cast<MessageItem*>(request.items[1])->GetSimpleText(), "Hello there.");
  EXPECT_EQ(static_cast<MessageItem*>(request.items[3])->GetSimpleText(), "Let me check.");
  ASSERT_EQ(request.items[4]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  ASSERT_EQ(request.items[5]->type, FOUNDRY_LOCAL_ITEM_TOOL_RESULT);

  // The transcript folds the call into the adjacent assistant message and correlates the result with it.
  auto messages = BuildTranscriptMessages(request.items);
  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateInputs(messages));

  // Exact template input: the prior text-only assistant turn survives, and so does the tool exchange.
  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"user","content":"Hi"},)"
            R"({"role":"assistant","content":"Hello there."},)"
            R"({"role":"user","content":"Weather in Seattle?"},)"
            R"({"role":"assistant","content":"Let me check.","tool_calls":[{"id":"call_1","type":"function",)"
            R"("function":{"name":"get_weather","arguments":{"city":"Seattle"}}}]},)"
            R"({"role":"tool","content":"sunny","tool_call_id":"call_1"},)"
            R"({"role":"user","content":"And tomorrow?"}])");
}

TEST(ResponseConverterTest, ToSessionRequest_TypedReplayMatchesChainReconstruction) {
  // The same conversation replayed two ways must produce the same prompt input.
  ResponseStore store;

  json first;
  first["id"] = "resp_1";
  first["previous_response_id"] = nullptr;
  first["output"] = json::array({{{"type", "message"},
                                  {"role", "assistant"},
                                  {"content", json::array({{{"type", "output_text"}, {"text", "Let me check."}}})}},
                                 {{"type", "function_call"},
                                  {"call_id", "call_1"},
                                  {"name", "get_weather"},
                                  {"arguments", R"({"city":"Seattle"})"}}});
  store.Store("resp_1", first,
              json::array({{{"type", "message"}, {"role", "user"}, {"content", "Weather in Seattle?"}}}));

  auto context = store.BuildChainContext("resp_1");
  ASSERT_TRUE(context.has_value());

  ResponseCreateParams chained;
  chained.model = "test-model";
  {
    std::vector<InputItem> items;
    FunctionCallResultInputItem result;
    result.call_id = "call_1";
    result.output = "sunny";
    items.push_back(result);
    chained.input = std::move(items);
  }

  auto chained_request = ToSessionRequest(chained, &(*context));

  auto stateless_body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "user", "content": [{"type": "input_text", "text": "Weather in Seattle?"}]},
      {"role": "assistant", "content": [{"type": "output_text", "text": "Let me check."}]},
      {"type": "function_call", "call_id": "call_1", "name": "get_weather",
       "arguments": "{\"city\":\"Seattle\"}"},
      {"type": "function_call_output", "call_id": "call_1", "output": "sunny"}
    ]
  })");
  auto stateless_params = stateless_body.get<ResponseCreateParams>();
  auto stateless_request = ToSessionRequest(stateless_params);

  EXPECT_EQ(BuildChatMessagesJson(BuildTranscriptMessages(stateless_request.items)),
            BuildChatMessagesJson(BuildTranscriptMessages(chained_request.items)));
}

TEST(ResponseConverterTest, StoredFunctionCallArgumentsAreCanonicalizedForChainReplay) {
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"type":"function_call","call_id":"call_1","name":"get_weather","arguments":{"city":"Seattle"}}
    ]
  })");

  auto stored_items = ToInputItems(body);
  ASSERT_EQ(stored_items.size(), 1u);
  ASSERT_TRUE(stored_items.front()["arguments"].is_string());
  EXPECT_EQ(stored_items.front()["arguments"], R"({"city":"Seattle"})");

  ResponseStore store;
  nlohmann::json response = {
      {"id", "resp_1"},
      {"previous_response_id", nullptr},
      {"output", nlohmann::json::array()},
  };
  store.Store("resp_1", response, stored_items);

  auto context = store.BuildChainContext("resp_1");
  ASSERT_TRUE(context.has_value());

  ResponseCreateParams next;
  next.model = "test-model";
  next.input = std::vector<InputItem>{};
  auto request = ToSessionRequest(next, &*context);

  // The replayed call, then the hop's assistant boundary: the stored response produced no output, and the live
  // session still committed an assistant message for that turn.
  ASSERT_EQ(request.items.size(), 2u);
  auto* call = static_cast<ToolCallItem*>(request.items.front());
  EXPECT_EQ(call->call_id, "call_1");
  EXPECT_EQ(call->name, "get_weather");
  EXPECT_EQ(call->arguments, R"({"city":"Seattle"})");
}

TEST(ResponseConverterTest, ToSessionRequest_TypedReplayAcceptsStoredTextPartShape) {
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [{"role": "assistant", "content": [{"type": "text", "text": "Stored shape."}]}]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ToSessionRequest(params);

  ASSERT_EQ(request.items.size(), 1u);
  EXPECT_EQ(static_cast<MessageItem*>(request.items[0])->GetSimpleText(), "Stored shape.");
}

TEST(ResponseConverterTest, ToSessionRequest_TypedReplayStillSkipsUnknownContentTypes) {
  // Policy is unchanged for content types we do not model: they are skipped, not coerced into text.
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [{"role": "user", "content": [
      {"type": "some_future_part", "text": "ignored"},
      {"type": "input_text", "text": "kept"}
    ]}]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ToSessionRequest(params);

  ASSERT_EQ(request.items.size(), 1u);
  EXPECT_EQ(static_cast<MessageItem*>(request.items[0])->GetSimpleText(), "kept");
}

// ========================================================================
// Chain replay — per-hop assistant-turn grouping
//
// A hop's output is exactly one assistant turn. These pin what the converter
// emits for each output shape, including the shapes that produce nothing
// replayable.
// ========================================================================

TEST(ResponseConverterTest, HopOutputTextCallTextEmitsOneOrderedAssistantTurn) {
  ResponseChainContext context{ResponseChainHop{
      nlohmann::json::parse(R"([{"type":"message","role":"user","content":"Weather in Seattle?"}])"),
      nlohmann::json::parse(R"([
        {"type":"message","role":"assistant","content":[{"type":"output_text","text":"Let me check."}]},
        {"type":"function_call","call_id":"call_1","name":"get_weather","arguments":"{\"city\":\"Seattle\"}"},
        {"type":"message","role":"assistant","content":[{"type":"output_text","text":" One moment."}]}
      ])")}};

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  auto request = ToSessionRequest(params, &context);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  ASSERT_EQ(messages[1].entries.size(), 3u);
  EXPECT_EQ(messages[1].entries[0].text, "Let me check.");
  EXPECT_EQ(messages[1].entries[1].kind, TranscriptEntry::Kind::kToolCall);
  EXPECT_EQ(messages[1].entries[2].text, " One moment.");
}

TEST(ResponseConverterTest, HopOutputWithOnlyReasoningEmitsAnEmptyAssistantBoundary) {
  ResponseChainContext context{ResponseChainHop{
      nlohmann::json::parse(R"([{"type":"message","role":"user","content":"Think about it."}])"),
      nlohmann::json::parse(
          R"([{"type":"reasoning","id":"rs_1","summary":[{"type":"summary_text","text":"private"}]}])")}};

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Well?");

  auto request = ToSessionRequest(params, &context);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(messages[1].entries.empty());
  EXPECT_EQ(messages[1].ReasoningText(), "");
  EXPECT_EQ(messages[2].role, FOUNDRY_LOCAL_ROLE_USER);
}

TEST(ResponseConverterTest, HopOutputWithReasoningAndTextReplaysOnlyTheText) {
  ResponseChainContext context{ResponseChainHop{
      nlohmann::json::parse(R"([{"type":"message","role":"user","content":"Think about it."}])"),
      nlohmann::json::parse(R"([
        {"type":"reasoning","id":"rs_1","summary":[{"type":"summary_text","text":"private"}]},
        {"type":"message","role":"assistant","content":[{"type":"output_text","text":"Answer."}]}
      ])")}};

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  auto request = ToSessionRequest(params, &context);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[1].VisibleText(), "Answer.");
  EXPECT_EQ(messages[1].ReasoningText(), "");
}

TEST(ResponseConverterTest, EveryHopContributesExactlyOneAssistantTurn) {
  ResponseChainContext context{
      ResponseChainHop{nlohmann::json::parse(R"([{"type":"message","role":"user","content":"one"}])"),
                       nlohmann::json::array()},
      ResponseChainHop{nlohmann::json::parse(R"([{"type":"message","role":"user","content":"two"}])"),
                       nlohmann::json::parse(
                           R"([{"type":"reasoning","id":"rs_1","summary":[]}])")},
      ResponseChainHop{nlohmann::json::parse(R"([{"type":"message","role":"user","content":"three"}])"),
                       nlohmann::json::parse(
                           R"([{"type":"message","role":"assistant","content":"done"}])")}};

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("four");

  auto request = ToSessionRequest(params, &context);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 7u);
  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"user","content":"one"},)"
            R"({"role":"assistant","content":""},)"
            R"({"role":"user","content":"two"},)"
            R"({"role":"assistant","content":""},)"
            R"({"role":"user","content":"three"},)"
            R"({"role":"assistant","content":"done"},)"
            R"({"role":"user","content":"four"}])");
}

TEST(ResponseConverterTest, StoredReasoningInputItemReplaysAsAnAssistantBoundary) {
  // ToInputItems stores whatever the caller sent, including a `reasoning` item echoed back from an earlier turn.
  ResponseChainContext context{ResponseChainHop{
      nlohmann::json::parse(R"([
        {"type":"message","role":"user","content":"Think about it."},
        {"type":"reasoning","id":"rs_1","summary":[{"type":"summary_text","text":"private"}]}
      ])"),
      nlohmann::json::parse(R"([{"type":"message","role":"assistant","content":"Answer."}])")}};

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  auto request = ToSessionRequest(params, &context);
  auto messages = BuildTranscriptMessages(request.items);

  // The stored reasoning item and the hop's own output are one contiguous assistant run, so they merge.
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(messages[1].VisibleText(), "Answer.");
  EXPECT_EQ(messages[1].ReasoningText(), "");
}

TEST(ResponseConverterTest, StoredAssistantInputMessageWithNoTextIsAnAssistantBoundary) {
  ResponseChainContext context{ResponseChainHop{
      nlohmann::json::parse(R"([
        {"type":"message","role":"user","content":"Say nothing."},
        {"type":"message","role":"assistant","content":""},
        {"type":"message","role":"user","content":"Still there?"}
      ])"),
      nlohmann::json::array()}};

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Hello?");

  auto request = ToSessionRequest(params, &context);
  auto messages = BuildTranscriptMessages(request.items);

  // user / assistant boundary / user from the stored input, then the hop's own empty output boundary, then the new
  // user turn.
  ASSERT_EQ(messages.size(), 5u);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(messages[1].entries.empty());
  EXPECT_EQ(messages[2].VisibleText(), "Still there?");
  EXPECT_EQ(messages[3].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(messages[3].entries.empty());
  EXPECT_EQ(messages[4].VisibleText(), "Hello?");
}

TEST(ResponseConverterTest, StoredNonAssistantMessageWithNoTextIsStillSkipped) {
  ResponseChainContext context{ResponseChainHop{
      nlohmann::json::parse(R"([
        {"type":"message","role":"user","content":""},
        {"type":"message","role":"user","content":"Hello"}
      ])"),
      nlohmann::json::parse(R"([{"type":"message","role":"assistant","content":"Hi"}])")}};

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::vector<InputItem>{};

  auto request = ToSessionRequest(params, &context);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].VisibleText(), "Hello");
  EXPECT_EQ(messages[1].VisibleText(), "Hi");
}

TEST(ResponseConverterTest, TypedReasoningInputItemBecomesAnAssistantBoundary) {
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "user", "content": [{"type": "input_text", "text": "Think about it."}]},
      {"type": "reasoning", "id": "rs_1", "summary": [{"type": "summary_text", "text": "private"}]},
      {"role": "user", "content": [{"type": "input_text", "text": "Well?"}]}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ToSessionRequest(params);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(messages[1].entries.empty());
  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"user","content":"Think about it."},)"
            R"({"role":"assistant","content":""},)"
            R"({"role":"user","content":"Well?"}])");
}

TEST(ResponseConverterTest, TypedReasoningItemNextToVisibleOutputAddsNothing) {
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "user", "content": [{"type": "input_text", "text": "Think about it."}]},
      {"type": "reasoning", "id": "rs_1", "summary": [{"type": "summary_text", "text": "private"}]},
      {"role": "assistant", "content": [{"type": "output_text", "text": "Answer."}]},
      {"role": "user", "content": [{"type": "input_text", "text": "Thanks."}]}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ToSessionRequest(params);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[1].VisibleText(), "Answer.");
  EXPECT_EQ(messages[1].ReasoningText(), "");
}

TEST(ResponseConverterTest, TypedEmptyUserMessageIsStillSkipped) {
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "user", "content": []},
      {"role": "user", "content": [{"type": "input_text", "text": "Hello"}]}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ToSessionRequest(params);

  ASSERT_EQ(request.items.size(), 1u);
  EXPECT_EQ(static_cast<MessageItem*>(request.items[0])->GetSimpleText(), "Hello");
}

TEST(ResponseConverterTest, TypedConsecutiveReasoningItemsCollapseToOneBoundary) {
  // A reasoning-only turn can surface as several reasoning items. They are one assistant turn, not several.
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "user", "content": [{"type": "input_text", "text": "Think about it."}]},
      {"type": "reasoning", "id": "rs_1", "summary": [{"type": "summary_text", "text": "first"}]},
      {"type": "reasoning", "id": "rs_2", "summary": [{"type": "summary_text", "text": "second"}]},
      {"role": "user", "content": [{"type": "input_text", "text": "Well?"}]}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ToSessionRequest(params);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(messages[1].entries.empty());
}

TEST(ResponseConverterTest, HopOutputWithSeveralReasoningItemsStillEmitsOneBoundary) {
  ResponseChainContext context{ResponseChainHop{
      nlohmann::json::parse(R"([{"type":"message","role":"user","content":"Think about it."}])"),
      nlohmann::json::parse(R"([
        {"type":"reasoning","id":"rs_1","summary":[{"type":"summary_text","text":"first"}]},
        {"type":"reasoning","id":"rs_2","summary":[{"type":"summary_text","text":"second"}]}
      ])")}};

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Well?");

  auto request = ToSessionRequest(params, &context);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(messages[1].entries.empty());
}
