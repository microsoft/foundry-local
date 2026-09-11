// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for media collection and the conversation-scoped media policy.
//
// The media prompt path (OnnxChatGenerator::CreateWithMedia) renders MediaInput::messages and nothing else, and the
// image / audio bytes never enter the transcript. These tests pin the one rule that follows: media is allowed only
// while a conversation has no history and declares no tools, and the rule does not depend on whether the session
// happened to still be cached.

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

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
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

  EXPECT_FALSE(RejectionFor(request, {.session_has_history = true, .tools_declared = true}).has_value());
}

// ---------------------------------------------------------------------------
// Prior history — rejected identically whether it is live or replayed.
// ---------------------------------------------------------------------------

TEST(MediaInputTest, MediaOnAWarmContinuationIsRejected) {
  // The session still holds the conversation, so the request carries only the new media turn.
  Request request;
  request.AddOwnedItem(ImageMessage("Does this photo match?"));

  EXPECT_EQ(RejectionFor(request, {.session_has_history = true, .tools_declared = false}),
            FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(MediaInputTest, MediaOnAColdContinuationIsRejectedTheSameWay) {
  // Same conversation, but the session cache dropped it, so the history arrives replayed in the request items.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather in Seattle?"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "It is sunny."));
  request.AddOwnedItem(ImageMessage("Does this photo match?"));

  EXPECT_EQ(RejectionFor(request, {.session_has_history = false, .tools_declared = false}),
            FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(MediaInputTest, WarmAndColdMediaContinuationsRejectIdentically) {
  Request warm;
  warm.AddOwnedItem(ImageMessage("Does this photo match?"));

  Request cold;
  cold.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather in Seattle?"));
  cold.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "It is sunny."));
  cold.AddOwnedItem(ImageMessage("Does this photo match?"));

  EXPECT_EQ(RejectionFor(warm, {.session_has_history = true, .tools_declared = false}),
            RejectionFor(cold, {.session_has_history = false, .tools_declared = false}));
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
    EXPECT_NE(std::string(ex.what()).find("only allowed on the first turn"), std::string::npos) << ex.what();
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

TEST(MediaInputTest, ReplayedAssistantBoundaryAloneStillCountsAsHistory) {
  // A hop whose output was reasoning-only replays as an assistant message with no entries. It is still a prior turn,
  // so a media continuation behind it must be rejected exactly as any other continuation is.
  std::vector<std::unique_ptr<Item>> boundary_parts;
  boundary_parts.push_back(std::make_unique<TextItem>(std::string{}));

  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Think about it."));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(boundary_parts)));
  request.AddOwnedItem(ImageMessage("Now look at this."));

  EXPECT_EQ(RejectionFor(request, {}), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Declared tools — a generated call on a media turn could never be answered.
// ---------------------------------------------------------------------------

TEST(MediaInputTest, FirstTurnMediaWithDeclaredToolsIsRejected) {
  Request request;
  request.AddOwnedItem(ImageMessage("What is this?"));

  try {
    auto media = CollectMediaInput(request);
    auto inputs = BuildTranscriptMessages(request.items);
    ValidateMediaTurn(media, inputs, {.session_has_history = false, .tools_declared = true});
    FAIL() << "expected first-turn media with declared tools to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("cannot be combined with tool definitions"), std::string::npos) << ex.what();
  }
}

TEST(MediaInputTest, AudioWithDeclaredToolsIsRejected) {
  Request request;
  request.AddOwnedItem(AudioMessage("Transcribe this."));

  EXPECT_EQ(RejectionFor(request, {.session_has_history = false, .tools_declared = true}),
            FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(MediaInputTest, TextTurnWithDeclaredToolsIsUnaffected) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather in Seattle?"));

  EXPECT_FALSE(RejectionFor(request, {.session_has_history = false, .tools_declared = true}).has_value());
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

TEST(MediaInputTest, ResponsesChainContinuationWithAnImageIsRejected) {
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

  EXPECT_EQ(RejectionFor(request, {}), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(MediaInputTest, StoredMediaOnlyUserTurnIsReplayedNotSilentlyDropped) {
  // A first turn that was image-only is stored with no text. Replay cannot reproduce the bytes, but the turn must
  // still appear in the rebuilt conversation — dropping it would remove a user turn the model saw.
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
  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[0].role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(messages[0].VisibleText(), " ") << "the media-only user turn must survive replay";
  EXPECT_EQ(messages[1].VisibleText(), "A cat.");
  EXPECT_EQ(messages[2].VisibleText(), "What colour was it?");

  // The continuation itself carries no media, so it is a normal text turn and is accepted.
  EXPECT_FALSE(RejectionFor(request, {}).has_value());

  // Warm parity, so the placeholder is not a silent degradation unique to replay: a live session ingests the same
  // image-only message into exactly the same transcript message. The bytes go straight to the generator and never
  // enter the transcript, so the very next turn of that live session rebuilds its prompt without them too.
  std::vector<std::unique_ptr<Item>> live_parts;
  live_parts.push_back(std::make_unique<TextItem>(" "));
  live_parts.push_back(std::make_unique<ImageItem>(std::vector<std::uint8_t>{1, 2, 3}, "image/png"));

  Request live;
  live.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(live_parts)));

  auto live_messages = BuildTranscriptMessages(live.items);
  ASSERT_EQ(live_messages.size(), 1u);
  EXPECT_EQ(live_messages[0].role, messages[0].role);
  EXPECT_EQ(live_messages[0].VisibleText(), messages[0].VisibleText());
}
