// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Audio transcription integration tests. Two fixtures share Recording.mp3
// and the SharedTestEnv audio model:
//   - AudioSessionFixture        — direct C++ API (AudioSession, AudioFromUri)
//   - AudioWebServiceFixture     — HTTP path (/v1/audio/transcriptions)
//
// Streaming PCM transcription is covered separately in streaming_audio_test.cc.

#include "model_fixture.h"
#include "web_service_fixture.h"

#include <algorithm>
#include <sstream>
#include <string>

/// Verify that the transcription contains the key phrases from the expected output.
/// The exact wording may vary by model, so we check distinctive multi-word fragments
/// rather than requiring an exact match.
static void ExpectTranscriptionContent(const std::string& text) {
  // Convert to lowercase for case-insensitive matching.
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Key phrases from the expected output:
  // "And lots of times you need to give people more than one link at a time.
  //  You a band could give their fans a couple new videos from the live concert
  //  behind the scenes photo gallery and album to purchase like these next few links."
  static const char* key_phrases[] = {
      "give people",
      "more than one link",
      "live concert",
      "behind the scenes",
      "photo gallery",
      "album to purchase",
  };

  for (const char* phrase : key_phrases) {
    EXPECT_NE(lower.find(phrase), std::string::npos)
        << "Expected transcription to contain '" << phrase << "'.\nGot: " << text;
  }
}

// ========================================================================
// AudioSessionFixture — exercises non-streaming AudioSession through the
// public C++ API (AudioFromUri / AudioFromData → ProcessRequest).
// Complements streaming_audio_test.cc which covers AudioItem + ItemQueue.
// ========================================================================

class AudioSessionFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Audio});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.audio_model()) {
      GTEST_SKIP() << "No audio model available";
    }
    if (env.audio_file_path().empty()) {
      GTEST_SKIP() << "Test audio file (Recording.mp3) not found";
    }
  }

  static foundry_local::IModel& audio_model() {
    return *SharedTestEnv::Get().audio_model();
  }

  static const std::string& audio_file_path() {
    return SharedTestEnv::Get().audio_file_path();
  }
};

TEST_F(AudioSessionFixture, TranscribeFromUri) {
  using namespace foundry_local;

  Request request;
  request.AddItem(Item::AudioFromUri(audio_file_path()));

  AudioSession session(audio_model());
  Response response = session.ProcessRequest(request);

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);
  ASSERT_FALSE(response.GetItems().empty()) << "Expected output items";

  std::string text = CollectResponseText(response);
  EXPECT_FALSE(text.empty()) << "Transcription should not be empty";
  ExpectTranscriptionContent(text);

  flUsage usage = response.GetUsage();
  EXPECT_GT(usage.prompt_tokens, 0);
  EXPECT_GT(usage.completion_tokens, 0);
  EXPECT_EQ(usage.total_tokens, usage.prompt_tokens + usage.completion_tokens);
}

TEST_F(AudioSessionFixture, TranscribeWithRequestLanguageOption) {
  using namespace foundry_local;

  Request request;
  request.AddItem(Item::AudioFromUri(audio_file_path()));

  RequestOptions opts;
  opts.search.temperature = 0.0f;
  opts.additional_options.Set("language", "en");
  request.SetOptions(opts);

  AudioSession session(audio_model());
  Response response = session.ProcessRequest(request);

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);
  std::string text = CollectResponseText(response);
  ExpectTranscriptionContent(text);
}

TEST_F(AudioSessionFixture, TranscribeWithSessionLevelOptions) {
  using namespace foundry_local;

  AudioSession session(audio_model());

  // Set defaults at the session level — exercises SetSessionOptionsImpl.
  RequestOptions session_opts;
  session_opts.search.temperature = 0.0f;
  session_opts.additional_options.Set("language", "en");
  session.SetOptions(session_opts);

  Request request;
  request.AddItem(Item::AudioFromUri(audio_file_path()));

  Response response = session.ProcessRequest(request);

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);
  std::string text = CollectResponseText(response);
  ExpectTranscriptionContent(text);
}

TEST_F(AudioSessionFixture, TranscribeProducesSpeechResultItem) {
  using namespace foundry_local;

  Request request;
  request.AddItem(Item::AudioFromUri(audio_file_path()));

  AudioSession session(audio_model());
  Response response = session.ProcessRequest(request);

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);

  const Item* speech_item = nullptr;
  for (const auto& item : response.GetItems()) {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_SPEECH_RESULT) {
      speech_item = &item;
      break;
    }
  }
  ASSERT_NE(speech_item, nullptr) << "Expected a SPEECH_RESULT item by default";

  auto result = speech_item->GetSpeechResult();
  std::string result_text(result.text);
  EXPECT_FALSE(result_text.empty());
  ExpectTranscriptionContent(result_text);
  // One segment per decoded token, kind NONE. Concatenated text matches result.text.
  ASSERT_FALSE(result.segments.empty());
  std::string concatenated;
  for (const auto& seg_item : result.segments) {
    auto seg = seg_item.GetSpeechSegment();
    EXPECT_EQ(seg.kind, FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE);
    EXPECT_FALSE(seg.start_time_ms.has_value());
    EXPECT_FALSE(seg.end_time_ms.has_value());
    EXPECT_TRUE(seg.words.empty());
    concatenated.append(seg.text.data(), seg.text.size());
  }
  EXPECT_EQ(concatenated, result_text);
  // Detected source language is not reported by GenAI today — reserved for future translation.
  EXPECT_FALSE(result.language.has_value());
  EXPECT_FALSE(result.duration_ms.has_value());
}

TEST_F(AudioSessionFixture, RejectsEmptyRequest) {
  using namespace foundry_local;

  Request request;  // no items
  AudioSession session(audio_model());

  EXPECT_THROW(session.ProcessRequest(request), Error);
}

TEST_F(AudioSessionFixture, RejectsFirstItemNotAudio) {
  using namespace foundry_local;

  Request request;
  request.AddItem(Item::Text("not an audio item"));

  AudioSession session(audio_model());
  EXPECT_THROW(session.ProcessRequest(request), Error);
}

TEST_F(AudioSessionFixture, RejectsAudioWithoutUriOrData) {
  using namespace foundry_local;

  // AudioFromData with null data and no queue → no uri AND no data.
  Request request;
  request.AddItem(Item::AudioFromData("mp3", nullptr, 0));

  AudioSession session(audio_model());
  EXPECT_THROW(session.ProcessRequest(request), Error);
}

TEST_F(AudioSessionFixture, RejectsInlineDataWithoutQueue) {
  using namespace foundry_local;

  // Read the mp3 into memory and pass it without an ItemQueue.
  // The non-streaming path requires a uri (inline-data-only is NOT_IMPLEMENTED).
  std::string bytes = ReadFileContents(audio_file_path());
  ASSERT_FALSE(bytes.empty());

  Request request;
  request.AddItem(Item::AudioFromData("mp3", bytes.data(), bytes.size()));

  AudioSession session(audio_model());
  EXPECT_THROW(session.ProcessRequest(request), Error);
}

// ========================================================================
// AudioWebServiceFixture — exercises POST /v1/audio/transcriptions.
// Audio model + test file come from SharedTestEnv. The HTTP service is
// started process-wide by SharedTestEnv.
// ========================================================================

class AudioWebServiceFixture : public WebServiceFixture {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Audio});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.audio_model()) {
      GTEST_SKIP() << "No audio model available";
    }

    if (env.audio_file_path().empty()) {
      GTEST_SKIP() << "Test audio file (Recording.mp3) not found";
    }
  }

  static const std::string& audio_model_id() { return SharedTestEnv::Get().audio_model_id(); }
  static const std::string& audio_file_path() { return SharedTestEnv::Get().audio_file_path(); }
};

TEST_F(AudioWebServiceFixture, NonStreamingTranscription) {
  auto client = MakeClient();

  json request_body = {
      {"model", audio_model_id()},
      {"filename", audio_file_path()},
      {"language", "en"},
  };

  auto result = client.Post("/v1/audio/transcriptions",
                            request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  ASSERT_TRUE(response.contains("text")) << "Response missing 'text': " << response.dump(2);

  std::string text = response["text"].get<std::string>();
  EXPECT_FALSE(text.empty()) << "Transcription text should not be empty";
  ExpectTranscriptionContent(text);
}

TEST_F(AudioWebServiceFixture, NonStreamingTranscriptionWithTemperature) {
  auto client = MakeClient();

  json request_body = {
      {"model", audio_model_id()},
      {"filename", audio_file_path()},
      {"language", "en"},
      {"temperature", 0.0},
  };

  auto result = client.Post("/v1/audio/transcriptions",
                            request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  ASSERT_TRUE(response.contains("text")) << "Response missing 'text': " << response.dump(2);

  std::string text = response["text"].get<std::string>();
  EXPECT_FALSE(text.empty()) << "Transcription text should not be empty";
  ExpectTranscriptionContent(text);
}

TEST_F(AudioWebServiceFixture, StreamingTranscription) {
  auto client = MakeClient();

  json request_body = {
      {"model", audio_model_id()},
      {"filename", audio_file_path()},
      {"language", "en"},
      {"stream", true},
  };

  auto result = client.Post("/v1/audio/transcriptions",
                            request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  // Parse SSE frames: each is "data: {...}\n\n" or "data: [DONE]\n\n"
  std::string accumulated_text;
  int chunk_count = 0;
  bool got_done = false;

  std::istringstream stream(result->body);
  std::string line;

  while (std::getline(stream, line)) {
    if (line.substr(0, 6) != "data: ") {
      continue;
    }

    std::string payload = line.substr(6);

    // Trim trailing \r if present (from \r\n line endings)
    if (!payload.empty() && payload.back() == '\r') {
      payload.pop_back();
    }

    if (payload == "[DONE]") {
      got_done = true;
      break;
    }

    auto chunk_json = json::parse(payload);
    ASSERT_TRUE(chunk_json.contains("text"))
        << "Chunk missing 'text': " << chunk_json.dump(2);

    accumulated_text += chunk_json["text"].get<std::string>();
    chunk_count++;
  }

  EXPECT_TRUE(got_done) << "Stream did not end with [DONE]";
  EXPECT_GT(chunk_count, 0) << "Expected at least one text chunk";
  EXPECT_FALSE(accumulated_text.empty()) << "Accumulated transcription should not be empty";
  ExpectTranscriptionContent(accumulated_text);
}

TEST_F(AudioWebServiceFixture, RejectsUnknownModel) {
  auto client = MakeClient();

  json request_body = {
      {"model", "nonexistent-model"},
      {"filename", audio_file_path()},
  };

  auto result = client.Post("/v1/audio/transcriptions",
                            request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  EXPECT_GE(result->status, 400) << "Expected error status for unknown model. Body: " << result->body;
}
