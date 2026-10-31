// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/item_codec.h"

#include "exception.h"
#include "inferencing/session/request.h"
#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/speech_result_item.h"
#include "items/tensor_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "util/file_uri.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;

std::unique_ptr<Item> RoundTrip(const Item& item, const ItemEncodingContext& context = {}) {
  const auto bytes = Json::to_cbor(EncodeItem(item, context));
  return DecodeItem(Json::from_cbor(bytes), {});
}

TEST(ItemCodecTest, BorrowedMediaIsCapturedAtEncodingWithoutTransferringCallerDeleter) {
  std::vector<uint8_t> bytes{1, 2, 3, 4};
  int releases = 0;
  auto image = std::make_unique<ImageItem>();
  flImageData input{};
  input.version = FOUNDRY_LOCAL_DATA_VERSION;
  input.data = bytes.data();
  input.data_size = bytes.size();
  input.format = "png";
  input.deleter_user_data = &releases;
  input.deleter = +[](const flImageData*, void* context) { ++*static_cast<int*>(context); };
  image->SetImageData(input);
  image->GetMetadata().Add("origin", "caller");
  Request request;
  request.AddBorrowedItem(image.get());
  bytes[0] = 9;
  const auto wire = Json::to_cbor(EncodeItem(*request.items.front(), {}));
  bytes[0] = 7;
  image.reset();
  EXPECT_EQ(releases, 1);

  auto decoded = DecodeItem(Json::from_cbor(wire), {});
  const auto& received = static_cast<const ImageItem&>(*decoded);
  ASSERT_EQ(received.data_size, 4u);
  EXPECT_EQ(static_cast<const uint8_t*>(received.data)[0], 9);
  EXPECT_EQ(received.format, "png");
  EXPECT_EQ(received.deleter_, nullptr);
  EXPECT_EQ(received.deleter_user_data_, nullptr);
  EXPECT_STREQ(received.GetMetadata()->Find("origin"), "caller");
  decoded.reset();
  EXPECT_EQ(releases, 1);
}

TEST(ItemCodecTest, MessageOwnsDecodedMediaAndPreservesAudioAttributes) {
  auto audio = std::make_unique<AudioItem>(std::vector<uint8_t>{4, 5, 6}, "pcm");
  audio->sample_rate = 16000;
  audio->channels = 2;
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("Transcribe this", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
  parts.push_back(std::move(audio));
  auto message = std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(parts), "speaker");
  auto decoded = RoundTrip(*message);
  message.reset();

  const auto& received = static_cast<const MessageItem&>(*decoded);
  ASSERT_EQ(received.content.size(), 2u);
  EXPECT_EQ(received.role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(received.name, "speaker");
  const auto& received_audio = static_cast<const AudioItem&>(*received.content[1].view);
  EXPECT_TRUE(received.content[1].IsOwned());
  EXPECT_EQ(received_audio.sample_rate, 16000);
  EXPECT_EQ(received_audio.channels, 2);
  EXPECT_EQ(received_audio.data_size, 3u);
  EXPECT_EQ(static_cast<const uint8_t*>(received_audio.data)[2], 6);
}

TEST(ItemCodecTest, CallerRelativeUriPreservesLiteralPercentEscapes) {
  const auto origin = std::filesystem::current_path() / "caller";
  ImageItem image("file://images/a%2520.png", "png");
  auto decoded = RoundTrip(image, {origin, {}});
  const auto& received = static_cast<const ImageItem&>(*decoded);
  EXPECT_EQ(PathFromFileUri(received.uri), (origin / "images" / "a%20.png").lexically_normal().string());
  EXPECT_EQ(image.uri, "file://images/a%2520.png");
  EXPECT_THROW((void)EncodeItem(image, {}), fl::Exception);
}

TEST(ItemCodecTest, TensorOwnsExactNumericPayloadAndRejectsMismatchedShape) {
  std::vector<float> values{1.25f, -2.0f, 0.0f, 3.5f};
  TensorItem tensor(FOUNDRY_LOCAL_TENSOR_FLOAT, values.data(), {2, 2});
  auto encoded = EncodeItem(tensor, {});
  auto decoded = DecodeItem(Json::from_cbor(Json::to_cbor(encoded)), {});
  values[0] = 99;
  const auto& received = static_cast<const TensorItem&>(*decoded);
  EXPECT_EQ(received.shape, (std::vector<int64_t>{2, 2}));
  float first = 0;
  std::memcpy(&first, received.data, sizeof(first));
  EXPECT_FLOAT_EQ(first, 1.25f);

  encoded["shape"] = std::vector<int64_t>{3, 2};
  EXPECT_THROW((void)DecodeItem(encoded, {}), fl::Exception);
  tensor.shape = {std::numeric_limits<int64_t>::max(), 2};
  EXPECT_THROW((void)EncodeItem(tensor, {}), fl::Exception);
  tensor.shape = {-1};
  EXPECT_THROW((void)EncodeItem(tensor, {}), fl::Exception);
}

TEST(ItemCodecTest, PackedTensorCopiesOnlyThePackedByteCount) {
  const std::vector<uint8_t> values{0x21, 0x03};
  TensorItem tensor(FOUNDRY_LOCAL_TENSOR_UINT4, values.data(), {3});
  const auto encoded = EncodeItem(tensor, {});
  ASSERT_EQ(encoded.at("bytes").get_binary().size(), 2u);
  auto decoded = RoundTrip(tensor);
  const auto& received = static_cast<const TensorItem&>(*decoded);
  EXPECT_EQ(static_cast<const uint8_t*>(received.data)[1], 0x03);
}

TEST(ItemCodecTest, ToolCallPreservesExactArgumentsAndHistoricalKind) {
  ToolCallItem call("call_1", "custom", "literal <tool_call> \"quoted\" \\u0041 \xc3\xa9", true, ToolKind::kCustom,
                    ToolKind::kCustom, GeneratedCallEncoding::kRawEnvelope);
  call.replayed_arguments = "original raw arguments";
  call.replayed_kind = ToolKind::kCustom;
  auto decoded = RoundTrip(call);
  const auto& received = static_cast<const ToolCallItem&>(*decoded);
  EXPECT_EQ(received.arguments, call.arguments);
  EXPECT_EQ(received.kind, ToolKind::kCustom);
  EXPECT_EQ(received.declared_kind, ToolKind::kCustom);
  EXPECT_EQ(received.generated_encoding, GeneratedCallEncoding::kRawEnvelope);
  EXPECT_TRUE(received.replayed_from_store);
  EXPECT_EQ(received.replayed_arguments, call.replayed_arguments);
  EXPECT_EQ(received.replayed_kind, ToolKind::kCustom);
}

TEST(ItemCodecTest, SpeechResultMaterializesStableBorrowedAbiViews) {
  SpeechResultItem result("hello");
  result.language = "en";
  result.duration_ms = 25;
  auto segment = std::make_unique<SpeechSegmentItem>(FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL, "hello");
  segment->start_time_ms = 0;
  segment->utterance_start = true;
  segment->words.push_back({"hello", 0, std::nullopt, 0.75f, "speaker-1"});
  result.segments.push_back(std::move(segment));
  result.Finalize();

  auto decoded = RoundTrip(result);
  const auto& received = static_cast<const SpeechResultItem&>(*decoded);
  flSpeechResultData view{};
  received.GetApiData(view);
  EXPECT_STREQ(view.text, "hello");
  EXPECT_EQ(view.duration_ms, 25);
  ASSERT_EQ(received.segments.size(), 1u);
  flSpeechSegmentData segment_view{};
  received.segments[0]->GetApiData(segment_view);
  EXPECT_EQ(segment_view.start_time_ms, 0);
  ASSERT_EQ(segment_view.words_count, 1u);
  EXPECT_STREQ(segment_view.words[0].text, "hello");
  EXPECT_STREQ(segment_view.words[0].speaker_id, "speaker-1");
}

TEST(ItemCodecTest, QueueEncodingRegistersIdentityWithoutConsumingOrFinishingInput) {
  ItemQueue queue;
  queue.Push(std::make_unique<TextItem>("first"));
  int registrations = 0;
  auto encoded = EncodeItem(queue, {{}, [&](const ItemQueue& registered) {
                                      EXPECT_EQ(&registered, &queue);
                                      ++registrations;
                                      return uint64_t{12};
                                    }});
  EXPECT_EQ(registrations, 1);
  EXPECT_EQ(queue.Size(), 1u);
  EXPECT_FALSE(queue.IsFinished());
  ItemQueue* receiver = nullptr;
  auto decoded = DecodeItem(encoded, {[&](uint64_t id) {
                              EXPECT_EQ(id, 12u);
                              auto peer = std::make_unique<ItemQueue>();
                              receiver = peer.get();
                              return peer;
                            }});
  EXPECT_EQ(decoded.get(), receiver);
  EXPECT_EQ(receiver->Size(), 0u);
  EXPECT_FALSE(receiver->IsFinished());
  EXPECT_THROW((void)EncodeItem(queue, {}), fl::Exception);
  EXPECT_THROW((void)DecodeItem(encoded, {}), fl::Exception);
}

TEST(ItemCodecTest, MalformedPayloadIsRejectedBeforeReturningAnItem) {
  EXPECT_THROW((void)DecodeItem(Json{{"type", 999}}, {}), fl::Exception);
  auto encoded = EncodeItem(TextItem("text"), {});
  encoded["metadata"] = Json::array();
  EXPECT_THROW((void)DecodeItem(encoded, {}), fl::Exception);
  encoded = EncodeItem(ImageItem(std::vector<uint8_t>{1}, "png"), {});
  encoded["bytes"] = "not binary";
  EXPECT_THROW((void)DecodeItem(encoded, {}), fl::Exception);
  encoded = EncodeItem(ItemQueue{}, {{}, [](const ItemQueue&) { return uint64_t{1}; }});
  encoded["queue"] = -1;
  EXPECT_THROW((void)DecodeItem(encoded, {[](uint64_t) { return std::make_unique<ItemQueue>(); }}), fl::Exception);
}

}  // namespace
}  // namespace fl::hosting
