// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for AudioTranscriptionResponse JSON serialization.

#include "contracts/audio_transcriptions.h"
#include "inferencing/generative/audio/asr_transcript.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace fl;

TEST(ASRTranscriptTest, TextWithoutCompletedRecordsDoesNotCreateSegments) {
  size_t callbacks = 0;
  ASRTranscript transcript("all", [&](const SpeechSegmentItem&) { ++callbacks; });
  transcript.AppendText("hel");
  auto result = transcript.Finish();
  EXPECT_EQ(result->text, "hel");
  EXPECT_TRUE(result->segments.empty());
  EXPECT_EQ(callbacks, 0U);
}

TEST(ASRTranscriptTest, PlainTextRetainsUntimedFragments) {
  ASRTranscript transcript;
  transcript.AppendText("hel");
  transcript.AppendText("");
  transcript.AppendText("lo");
  auto result = transcript.Finish();
  ASSERT_EQ(result->segments.size(), 2U);
  EXPECT_EQ(result->text, "hello");
  EXPECT_EQ(result->segments[0]->kind, FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE);
  EXPECT_FALSE(result->segments[0]->start_time_ms.has_value());
}

TEST(ASRTranscriptTest, WordModePublishesOnlyCompletedWords) {
  ASRTranscript transcript("word");
  transcript.AppendText("Hello");
  transcript.AppendWord("Hello", 100, 400);
  auto result = transcript.Finish();
  ASSERT_EQ(result->segments.size(), 1U);
  const auto& segment = *result->segments[0];
  EXPECT_EQ(segment.kind, FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL);
  EXPECT_EQ(segment.start_time_ms, 100);
  EXPECT_EQ(segment.end_time_ms, 400);
  ASSERT_EQ(segment.words.size(), 1U);
  EXPECT_EQ(segment.words[0].text, "Hello");
}

TEST(ASRTranscriptTest, SegmentModeDoesNotRequireWordRecords) {
  ASRTranscript transcript("segment");
  transcript.AppendText("Hello world.");
  transcript.AppendSegment("Hello world.", 100, 900);
  auto result = transcript.Finish();
  ASSERT_EQ(result->segments.size(), 1U);
  EXPECT_EQ(result->segments[0]->text, result->text);
  EXPECT_EQ(result->segments[0]->end_time_ms, 900);
  EXPECT_TRUE(result->segments[0]->words.empty());
}

TEST(ASRTranscriptTest, ExportedPointersOutliveTheAccumulator) {
  std::unique_ptr<SpeechResultItem> retained;
  {
    ASRTranscript transcript("all");
    transcript.AppendText("Hello.");
    transcript.AppendWord("Hello.", 100, 800);
    transcript.AppendSegment("Hello.", 100, 800);
    retained = transcript.Finish();
  }
  flSpeechResultData result{};
  retained->GetApiData(result);
  EXPECT_STREQ(result.text, "Hello.");
  ASSERT_EQ(result.segments_count, 1U);
  flSpeechSegmentData segment{};
  retained->segments[0]->GetApiData(segment);
  EXPECT_STREQ(segment.text, "Hello.");
  ASSERT_EQ(segment.words_count, 1U);
  EXPECT_STREQ(segment.words[0].text, "Hello.");
  EXPECT_EQ(segment.words[0].start_time_ms, 100);
}

TEST(ASRTranscriptTest, AllModeAssociatesWordsByOrderNotEqualTimestamps) {
  std::vector<std::unique_ptr<SpeechSegmentItem>> streamed;
  ASRTranscript transcript("all", [&](const SpeechSegmentItem& segment) {
    streamed.push_back(CopySpeechSegment(segment));
  });
  transcript.AppendText(" Go. Go.");
  transcript.AppendWord(" Go.", 100, 200);
  transcript.AppendWord(" Go.", 100, 200);
  EXPECT_TRUE(streamed.empty());
  transcript.AppendSegment(" Go.", 100, 200);
  transcript.AppendSegment(" Go.", 100, 200);
  auto result = transcript.Finish();
  ASSERT_EQ(result->segments.size(), 2U);
  ASSERT_EQ(streamed.size(), 2U);
  ASSERT_EQ(streamed[0]->words.size(), 1U);
  ASSERT_EQ(streamed[1]->words.size(), 1U);
  EXPECT_EQ(streamed[1]->words[0].text, " Go.");
  result.reset();
  EXPECT_EQ(streamed[0]->text, " Go.");
  EXPECT_EQ(streamed[1]->start_time_ms, 100);
}

TEST(AudioTranscriptionResponseJsonTest, ToJson_EmitsIdWhenNonEmpty) {
  AudioTranscriptionResponse r{.id = "audio_xyz", .text = "hi"};
  nlohmann::json j = r;

  ASSERT_TRUE(j.contains("id"));
  EXPECT_EQ(j.at("id").get<std::string>(), "audio_xyz");
  EXPECT_EQ(j.at("text").get<std::string>(), "hi");

  // Also check the serialized string contains the field.
  EXPECT_NE(j.dump().find("\"id\":\"audio_xyz\""), std::string::npos);
}

TEST(AudioTranscriptionResponseJsonTest, ToJson_OmitsIdWhenEmpty) {
  AudioTranscriptionResponse r{.id = "", .text = "hi"};
  nlohmann::json j = r;

  EXPECT_FALSE(j.contains("id"));
  EXPECT_EQ(j.at("text").get<std::string>(), "hi");
  EXPECT_EQ(j.dump().find("\"id\""), std::string::npos);
}

TEST(AudioTranscriptionResponseJsonTest, CompletedSegmentsCarryIntegerMillisecondsAndWords) {
  ASRTranscript transcript("all");
  transcript.AppendText("Hello.");
  transcript.AppendWord("Hello.", 125, 950);
  transcript.AppendSegment("Hello.", 125, 950);
  auto result = transcript.Finish();
  const auto json = ToAudioTranscriptionJson(*result, "audio_test", true);
  EXPECT_EQ(json.at("text"), "Hello.");
  ASSERT_EQ(json.at("segments").size(), 1U);
  EXPECT_EQ(json["segments"][0]["start_time_ms"], 125);
  EXPECT_EQ(json["segments"][0]["end_time_ms"], 950);
  EXPECT_EQ(json["segments"][0]["words"][0]["text"], "Hello.");
  EXPECT_FALSE(ToAudioTranscriptionJson(*result, "audio_test", false).contains("segments"));
}
