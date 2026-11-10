// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for media collection, conversation history, and the media policy.

#include "inferencing/generative/chat/media_input.h"

#include "contracts/responses.h"
#include "exception.h"
#include "inferencing/generative/openresponses/response_converter.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace fl;
using namespace fl::responses;
using json = nlohmann::json;

namespace {

/// A message with one text part and one image part, as a vision request carries it.
std::unique_ptr<MessageItem> ImageMessage(const std::string& text) {
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>(text));
  parts.push_back(std::make_unique<ImageItem>(std::vector<std::uint8_t>{1, 2, 3, 4}, "image/png"));
  return std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(parts));
}

std::unique_ptr<MessageItem> AudioMessage(const std::string& text) {
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>(text));
  parts.push_back(std::make_unique<AudioItem>(std::vector<std::uint8_t>{9, 8, 7}, "wav"));
  return std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(parts));
}

/// Run the policy over a request and report the error code it raised, or nullopt when it accepted the turn.
std::optional<flErrorCode> RejectionFor(const Request& request, const MediaTurnContext& context) {
  auto media = CollectMediaInput(request);
  auto inputs = BuildTranscriptMessages(request.items);

  try {
    ValidateMediaTurn(media, inputs, context);
  } catch (const fl::Exception& ex) {
    return ex.code();
  }

  return std::nullopt;
}

}  // namespace

TEST(MediaInputTest, NoMediaProducesAnEmptyCollection) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Hello"));

  auto media = CollectMediaInput(request);

  EXPECT_TRUE(media.Empty());
  EXPECT_TRUE(media.messages.empty());
}

TEST(MediaInputTest, ImageAndAudioPartsAreCollectedWithTheirMessages) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_SYSTEM, "Be terse."));
  request.AddOwnedItem(ImageMessage("What is this?"));
  request.AddOwnedItem(AudioMessage("And this?"));

  auto media = CollectMediaInput(request);

  EXPECT_FALSE(media.Empty());
  EXPECT_EQ(media.images.size(), 1u);
  EXPECT_EQ(media.audios.size(), 1u);
  EXPECT_EQ(media.messages.size(), 3u);
}

TEST(MediaInputTest, FirstTurnMediaWithNoHistoryAndNoToolsIsAccepted) {
  Request request;
  request.AddOwnedItem(ImageMessage("What is this?"));

  EXPECT_FALSE(RejectionFor(request, {}).has_value());
}

TEST(MediaInputTest, ToolActivityWithoutMediaIsAccepted) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Let me check."));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({"city":"Seattle"})"));
  request.AddOwnedItem(std::make_unique<ToolResultItem>("call_1", "sunny"));

  EXPECT_FALSE(RejectionFor(request, {.tools_declared = true}).has_value());
}

// ---------------------------------------------------------------------------
// Image history is accepted identically whether it is live or replayed.
// ---------------------------------------------------------------------------

TEST(MediaInputTest, ImageOnAWarmContinuationIsAccepted) {
  Request request;
  request.AddOwnedItem(ImageMessage("Does this photo match?"));

  std::vector<TranscriptMessage> conversation{
      {FOUNDRY_LOCAL_ROLE_USER, "Weather in Seattle?"},
      {FOUNDRY_LOCAL_ROLE_ASSISTANT, "It is sunny."},
  };
  auto current = BuildTranscriptMessages(request.items);
  conversation.insert(conversation.end(), current.begin(), current.end());
  auto media = CollectMediaInput(conversation);

  ASSERT_EQ(media.messages.size(), 3u);
  EXPECT_NO_THROW(ValidateMediaTurn(media, conversation, {}));
}

TEST(MediaInputTest, ImageOnAColdContinuationIsAccepted) {
  // Same conversation, but the session cache dropped it, so the history arrives replayed in the request items.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather in Seattle?"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "It is sunny."));
  request.AddOwnedItem(ImageMessage("Does this photo match?"));

  EXPECT_FALSE(RejectionFor(request, {}).has_value());
}

TEST(MediaInputTest, WarmAndColdImageContinuationsAgree) {
  Request warm;
  warm.AddOwnedItem(ImageMessage("Does this photo match?"));

  Request cold;
  cold.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather in Seattle?"));
  cold.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "It is sunny."));
  cold.AddOwnedItem(ImageMessage("Does this photo match?"));

  EXPECT_EQ(RejectionFor(warm, {}), RejectionFor(cold, {}));
}

TEST(MediaInputTest, ReplayedToolExchangeWithANewImageIsRejectedNotDropped) {
  // The exact cache-miss shape: the reconstructed conversation carries an assistant call and its result, and the new
  // turn is an image. The media prompt would render only the messages, so the call and result would be committed
  // without ever reaching the model.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather in Seattle?"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Let me check."));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({"city":"Seattle"})"));
  request.AddOwnedItem(std::make_unique<ToolResultItem>("call_1", "sunny"));
  request.AddOwnedItem(ImageMessage("Does this photo match?"));

  auto media = CollectMediaInput(request);
  auto inputs = BuildTranscriptMessages(request.items);

  // The omission this guards against: what the media path would render carries neither the call nor its result.
  ASSERT_FALSE(media.Empty());
  EXPECT_EQ(media.messages.size(), 3u);

  try {
    ValidateMediaTurn(media, inputs, {});
    FAIL() << "expected the media + replayed tool exchange to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("tool"), std::string::npos) << ex.what();
  }
}

TEST(MediaInputTest, ToolResultOnlyWithANewAudioClipIsRejected) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Let me check."));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({})"));
  request.AddOwnedItem(std::make_unique<ToolResultItem>("call_1", "sunny"));
  request.AddOwnedItem(AudioMessage("Transcribe this."));

  EXPECT_EQ(RejectionFor(request, {}), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(MediaInputTest, ImageCanFollowAReplayedAssistantBoundary) {
  std::vector<std::unique_ptr<Item>> boundary_parts;
  boundary_parts.push_back(std::make_unique<TextItem>(std::string{}));

  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Think about it."));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(boundary_parts)));
  request.AddOwnedItem(ImageMessage("Now look at this."));

  EXPECT_FALSE(RejectionFor(request, {}).has_value());
}

// ---------------------------------------------------------------------------
// The multimodal template cannot represent tool calls or results.
// ---------------------------------------------------------------------------

TEST(MediaInputTest, FirstTurnMediaWithDeclaredToolsIsRejected) {
  Request request;
  request.AddOwnedItem(ImageMessage("What is this?"));

  try {
    auto media = CollectMediaInput(request);
    auto inputs = BuildTranscriptMessages(request.items);
    ValidateMediaTurn(media, inputs, {.tools_declared = true});
    FAIL() << "expected first-turn media with declared tools to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("cannot be combined with tool definitions"), std::string::npos) << ex.what();
  }
}

TEST(MediaInputTest, AudioWithDeclaredToolsIsRejected) {
  Request request;
  request.AddOwnedItem(AudioMessage("Transcribe this."));

  EXPECT_EQ(RejectionFor(request, {.tools_declared = true}), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(MediaInputTest, TextTurnWithDeclaredToolsIsUnaffected) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather in Seattle?"));

  EXPECT_FALSE(RejectionFor(request, {.tools_declared = true}).has_value());
}

// ---------------------------------------------------------------------------
// Through the Responses converter, as a real request arrives.
// ---------------------------------------------------------------------------

TEST(MediaInputTest, ResponsesRequestMixingAnImageWithAToolExchangeIsRejected) {
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"type": "function_call", "call_id": "call_1", "name": "get_weather", "arguments": "{\"city\":\"Seattle\"}"},
      {"type": "function_call_output", "call_id": "call_1", "output": "sunny"},
      {"role": "user", "content": [
        {"type": "input_text", "text": "Does this photo match?"},
        {"type": "input_image", "image_url": "data:image/png;base64,iVBORw0KGgo="}
      ]}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ResponseConverter::ToSessionRequest(params);

  auto media = CollectMediaInput(request);
  ASSERT_EQ(media.images.size(), 1u);
  ASSERT_EQ(media.messages.size(), 1u);
  EXPECT_EQ(RejectionFor(request, {}), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(MediaInputTest, ResponsesChainContinuationWithAnImageIsAccepted) {
  // The full cold path: a stored conversation is reconstructed and the new turn carries an image.
  ResponseStore store;

  json response;
  response["id"] = "resp_1";
  response["previous_response_id"] = nullptr;
  response["output"] = json::array({{{"type", "message"}, {"role", "assistant"}, {"content", "It is sunny."}}});
  store.Store("resp_1", response,
              json::array({{{"type", "message"}, {"role", "user"}, {"content", "Weather in Seattle?"}}}));

  auto context = store.BuildChainContext("resp_1");
  ASSERT_TRUE(context.has_value());

  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "user", "content": [
        {"type": "input_text", "text": "Does this photo match?"},
        {"type": "input_image", "image_url": "data:image/png;base64,iVBORw0KGgo="}
      ]}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ResponseConverter::ToSessionRequest(params, &(*context));

  EXPECT_FALSE(RejectionFor(request, {}).has_value());
}

TEST(MediaInputTest, StoredImageOnlyUserTurnReplaysTheOriginalImage) {
  ResponseStore store;

  json response;
  response["id"] = "resp_1";
  response["previous_response_id"] = nullptr;
  response["output"] = json::array({{{"type", "message"}, {"role", "assistant"}, {"content", "A cat."}}});
  store.Store("resp_1", response,
              json::array({{{"type", "message"},
                            {"role", "user"},
                            {"content", json::array({{{"type", "input_image"},
                                                      {"image_url", "data:image/png;base64,iVBORw0KGgo="}}})}}}));

  auto context = store.BuildChainContext("resp_1");
  ASSERT_TRUE(context.has_value());

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("What colour was it?");

  auto request = ResponseConverter::ToSessionRequest(params, &(*context));
  auto media = CollectMediaInput(request);
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(media.images.size(), 1u);
  EXPECT_EQ(media.images[0]->ReadBytes(), (std::vector<std::uint8_t>{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a}));
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[0].role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(messages[0].VisibleText(), " ") << "the media-only user turn must survive replay";
  EXPECT_EQ(messages[1].VisibleText(), "A cat.");
  EXPECT_EQ(messages[2].VisibleText(), "What colour was it?");

  EXPECT_FALSE(RejectionFor(request, {}).has_value());

  // An image-only live message and the replayed message have the same visible text and media-bearing shape.
  std::vector<std::unique_ptr<Item>> live_parts;
  live_parts.push_back(std::make_unique<ImageItem>(std::vector<std::uint8_t>{1, 2, 3}, "image/png"));
  live_parts.push_back(std::make_unique<TextItem>(" "));

  Request live;
  live.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(live_parts)));

  auto live_messages = BuildTranscriptMessages(live.items);
  ASSERT_EQ(live_messages.size(), 1u);
  EXPECT_EQ(live_messages[0].role, messages[0].role);
  EXPECT_EQ(live_messages[0].VisibleText(), messages[0].VisibleText());
  ASSERT_EQ(live_messages[0].entries.size(), messages[0].entries.size());
  for (size_t i = 0; i < live_messages[0].entries.size(); ++i) {
    EXPECT_EQ(live_messages[0].entries[i].kind, messages[0].entries[i].kind);
  }
}

TEST(MediaInputTest, StoredMixedMediaReplaysInTheSameOrderAsTypedInput) {
  const json body = {
      {"model", "test-model"},
      {"input", json::array({{{"role", "user"},
                              {"content", json::array({{{"type", "input_text"}, {"text", "before"}},
                                                       {{"type", "input_image"},
                                                        {"image_url", "data:image/png;base64,AQI="}},
                                                       {{"type", "input_audio"}, {"format", "wav"}, {"data", "AwQ="}},
                                                       {{"type", "input_text"}, {"text", "after"}}})}}})}};
  auto warm = ResponseConverter::ToSessionRequest(body.get<ResponseCreateParams>());
  ResponseChainContext context;
  context.emplace_back(ResponseConverter::ToInputItems(body), json::array());

  ResponseCreateParams next;
  next.model = "test-model";
  next.input = std::string("continue");
  auto cold = ResponseConverter::ToSessionRequest(next, &context);
  const auto warm_messages = BuildTranscriptMessages(warm.items);
  const auto cold_messages = BuildTranscriptMessages(cold.items);

  ASSERT_FALSE(warm_messages.empty());
  ASSERT_FALSE(cold_messages.empty());
  ASSERT_EQ(warm_messages[0].entries.size(), 4u);
  ASSERT_EQ(cold_messages[0].entries.size(), 4u);
  for (size_t i = 0; i < warm_messages[0].entries.size(); ++i) {
    EXPECT_EQ(warm_messages[0].entries[i].kind, cold_messages[0].entries[i].kind);
    EXPECT_EQ(warm_messages[0].entries[i].text, cold_messages[0].entries[i].text);
  }

  auto media = CollectMediaInput(cold);
  ASSERT_EQ(media.images.size(), 1u);
  ASSERT_EQ(media.audios.size(), 1u);
  EXPECT_EQ(media.images[0]->ReadBytes(), (std::vector<std::uint8_t>{1, 2}));
  const auto* audio = static_cast<const std::uint8_t*>(media.audios[0]->data);
  EXPECT_EQ(std::vector<std::uint8_t>(audio, audio + media.audios[0]->data_size),
            (std::vector<std::uint8_t>{3, 4}));
}

TEST(MediaInputTest, LocalImageReplaySnapshotsCurrentSegmentWithoutChangingPublicInput) {
  const auto dir = fl::test::TempPath::CreateTempDir("fl_media_replay_");
  const auto image_path = dir.path() / "image.png";
  {
    std::ofstream file(image_path, std::ios::binary);
    file.write("\x01\x02\x03", 3);
    ASSERT_TRUE(file.good());
  }

  ResponseChainContext prior;
  prior.emplace_back(
      json::array({{{"role", "user"},
                    {"content", json::array({{{"type", "input_image"},
                                              {"image_url", "data:image/png;base64,CA=="}}})}}}),
      json::array());
  const json body = {
      {"model", "test-model"},
      {"input", json::array({{{"role", "user"},
                              {"content", json::array({{{"type", "input_image"},
                                                        {"image_url", "data:image/png;base64,CQ=="}},
                                                       {{"type", "input_text"}, {"text", "compare"}},
                                                       {{"type", "input_image"},
                                                        {"image_url", image_path.string()}}})}}})}};
  const auto public_items = ResponseConverter::ToInputItems(body);
  auto request = ResponseConverter::ToSessionRequest(body.get<ResponseCreateParams>(), &prior);
  ASSERT_TRUE(std::filesystem::remove(image_path));

  const auto snapshot = ResponseConverter::SnapshotReplayInputItems(public_items, request);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(public_items[0]["content"][2]["image_url"], image_path.string());
  EXPECT_EQ((*snapshot)[0]["content"][0]["image_url"], "data:image/png;base64,CQ==");
  EXPECT_EQ((*snapshot)[0]["content"][2]["image_url"], "data:image/png;base64,AQID");

  ResponseChainContext chain;
  chain.emplace_back(public_items, json::array());
  chain[0].replay_input_items = snapshot;
  ResponseCreateParams next;
  next.model = "test-model";
  next.input = std::string("continue");
  auto replay = ResponseConverter::ToSessionRequest(next, &chain);
  auto media = CollectMediaInput(replay);
  ASSERT_EQ(media.images.size(), 2u);
  EXPECT_EQ(media.images[0]->ReadBytes(), (std::vector<std::uint8_t>{9}));
  EXPECT_EQ(media.images[1]->ReadBytes(), (std::vector<std::uint8_t>{1, 2, 3}));
}

TEST(MediaInputTest, InlineImagesNeedNoPrivateReplayCopy) {
  const json body = {{"model", "test-model"},
                     {"input", json::array({{{"role", "user"},
                                             {"content", json::array({{{"type", "input_image"},
                                                                       {"image_url", "data:image/png;base64,AQI="}}})}}})}};
  auto request = ResponseConverter::ToSessionRequest(body.get<ResponseCreateParams>());

  EXPECT_FALSE(ResponseConverter::SnapshotReplayInputItems(ResponseConverter::ToInputItems(body), request));
}

TEST(MediaInputTest, LocalImageReplayRejectsMissingResolvedImage) {
  const json input = json::array({{{"role", "user"},
                                   {"content", json::array({{{"type", "input_image"},
                                                             {"image_url", "/missing-image.png"}}})}}});
  Request request;

  EXPECT_THROW(ResponseConverter::SnapshotReplayInputItems(input, request), fl::Exception);
}

TEST(MediaInputTest, LocalImageReplayHandlesUnsafeFormatHints) {
  const auto dir = fl::test::TempPath::CreateTempDir("fl_media_mime_");
  const auto image_path = dir.path() / "image.png";
  {
    std::ofstream file(image_path, std::ios::binary);
    file.write("\x01\x02\x03", 3);
    ASSERT_TRUE(file.good());
  }
  const json body = {
      {"model", "test-model"},
      {"input", json::array({{{"role", "user"},
                              {"content", json::array({{{"type", "input_image"},
                                                        {"image_url", image_path.string()},
                                                        {"media_type", "image/png;extra,invalid"}}})}}})}};
  auto request = ResponseConverter::ToSessionRequest(body.get<ResponseCreateParams>());
  ASSERT_TRUE(std::filesystem::remove(image_path));

  const auto snapshot = ResponseConverter::SnapshotReplayInputItems(ResponseConverter::ToInputItems(body), request);
  ASSERT_TRUE(snapshot);
  ResponseChainContext chain;
  chain.emplace_back(json::array(), json::array());
  chain[0].replay_input_items = snapshot;
  ResponseCreateParams next;
  next.model = "test-model";
  next.input = std::string("continue");
  auto replay = ResponseConverter::ToSessionRequest(next, &chain);
  auto media = CollectMediaInput(replay);
  ASSERT_EQ(media.images.size(), 1u);
  EXPECT_EQ(media.images[0]->ReadBytes(), (std::vector<std::uint8_t>{1, 2, 3}));
}
