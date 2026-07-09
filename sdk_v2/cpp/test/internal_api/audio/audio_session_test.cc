// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for AudioSession.
// Validation tests exercise input checking without needing an audio model —
// they throw before the generator is created, so any GenAIModelInstance works.

#include "inferencing/generative/audio/audio_session.h"

#include "ep_detection/ep_detector.h"
#include "exception.h"
#include "inferencing/model_load_manager.h"
#include "items/audio_item.h"
#include "items/bytes_item.h"
#include "items/item_queue.h"
#include "items/speech_result_item.h"
#include "items/text_item.h"
#include "items/text_item.h"
#include "logger.h"
#include "model.h"
#include "internal_api/null_session_manager.h"
#include "internal_api/null_telemetry.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

using namespace fl;

namespace {

/// Verify that the transcription contains key phrases from the expected output.
/// The exact wording may vary by model, so we check distinctive fragments.
static void ExpectTranscriptionContent(const std::string& text) {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Expected: "And lots of times you need to give people more than one link at a time.
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

}  // namespace

// ===========================================================================
// Test fixture: loads the tiny-random-gpt2 model bundled in testdata as a
// stand-in for GenAIModelInstance&. Validation tests throw before generation,
// so the model type doesn't matter — we just need any loaded model. Using the
// bundled tiny model keeps unit tests lightweight (no shared cache or large
// model download required, unlike the integration tests).
// ===========================================================================

class AudioSessionTest : public ::testing::Test {
 protected:
  static constexpr const char* kStubModelAlias = "tiny-random-gpt2-fp32-1";

  static void SetUpTestSuite() {
    auto model_path = fl::test::GetTestDataPath(kStubModelAlias);
    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);

    auto result = load_manager_->LoadModel(model_path.string(), kStubModelAlias);

    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess)
        << "Failed to load stub test model from: " << model_path;

    model_ = result.model;
  }

  static void TearDownTestSuite() {
    if (load_manager_) {
      load_manager_->UnloadModel(kStubModelAlias);
    }

    load_manager_.reset();
    ep_detector_.reset();
    model_ = nullptr;
  }

  GenAIModelInstance& GetModel() { return *model_; }
  const Model& GetCatalogModel() { return catalog_model_; }

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline fl::test::FakeServiceBindings svc_;
  static inline Model catalog_model_ = Model::FromModelInfo(
      ModelInfo{}, "", svc_.download_manager, svc_.model_load_manager);
  fl::test::NullTelemetry null_telemetry_;
  fl::test::NullSessionManager null_session_manager_;
};

// ===========================================================================
// Test fixture: loads the whisper model for real audio inference tests.
// Skips if the audio model is not available in the test cache.
// ===========================================================================

class AudioSessionInferenceTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);

    fs::path model_path;
    try {
      model_path = fl::test::GetTestModelPath(fl::test::kTestAudioModelAlias);
    } catch (const std::exception&) {
      GTEST_SKIP() << "Audio test model not found — skipping inference tests";
      return;
    }

    auto result = load_manager_->LoadModel(model_path.string(), fl::test::kTestAudioModelAlias);

    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess)
        << "Failed to load audio test model from: " << model_path;

    model_ = result.model;
  }

  static void TearDownTestSuite() {
    if (load_manager_) {
      load_manager_->UnloadModel(fl::test::kTestAudioModelAlias);
    }

    load_manager_.reset();
    ep_detector_.reset();
    model_ = nullptr;
  }

  GenAIModelInstance& GetModel() { return *model_; }
  const Model& GetCatalogModel() { return catalog_model_; }

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline fl::test::FakeServiceBindings svc_;
  static inline Model catalog_model_ = Model::FromModelInfo(
      ModelInfo{}, "", svc_.download_manager, svc_.model_load_manager);
  fl::test::NullTelemetry null_telemetry_;
  fl::test::NullSessionManager null_session_manager_;
};

// ===========================================================================
// Construction & Type
// ===========================================================================

TEST_F(AudioSessionTest, TypeReturnsAudio) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  EXPECT_EQ(session.Type(), SessionType::kAudio);
}

// ===========================================================================
// Input validation — no model inference happens for these
// ===========================================================================

TEST_F(AudioSessionTest, ThrowsWhenFirstItemIsNotAudio) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  // Request with a text item instead of an audio item
  Request request;
  auto text_item = std::make_unique<TextItem>("hello");
  request.items.push_back(text_item.get());

  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("First item must be AUDIO"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, ThrowsWhenAudioItemHasNoUriOrData) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  // Audio item with no uri and no data
  Request request;
  auto audio_item = std::make_unique<AudioItem>();
  request.items.push_back(audio_item.get());

  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("uri (file path) or inline data"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, ThrowsWhenRequestHasNoItems) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("expects 1 or 2 items"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, ThrowsWhenTooManyItems) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  auto audio = std::make_unique<AudioItem>();
  auto queue = std::make_unique<ItemQueue>();
  auto extra = std::make_unique<TextItem>("extra");
  request.items.push_back(audio.get());
  request.items.push_back(queue.get());
  request.items.push_back(extra.get());

  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("expects 1 or 2 items"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, ThrowsWhenSecondItemIsNotQueue) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  auto audio = std::make_unique<AudioItem>();
  auto text = std::make_unique<TextItem>("not a queue");
  request.items.push_back(audio.get());
  request.items.push_back(text.get());

  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("Second item must be QUEUE"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

// ===========================================================================
// Streaming validation — these throw before OGA objects are created
// ===========================================================================

TEST_F(AudioSessionTest, StreamingThrowsWhenFormatIsNotPcm) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  auto audio = std::make_unique<AudioItem>();
  audio->format = "mp3";
  auto queue = std::make_unique<ItemQueue>();
  request.items.push_back(audio.get());
  request.items.push_back(queue.get());

  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("format 'pcm'"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, StreamingThrowsWhenSampleRateIsWrong) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  auto audio = std::make_unique<AudioItem>();
  audio->format = "pcm";
  audio->sample_rate = 44100;
  auto queue = std::make_unique<ItemQueue>();
  request.items.push_back(audio.get());
  request.items.push_back(queue.get());

  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("16000 Hz"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, StreamingThrowsWhenChannelsIsWrong) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  auto audio = std::make_unique<AudioItem>();
  audio->format = "pcm";
  audio->channels = 2;
  auto queue = std::make_unique<ItemQueue>();
  request.items.push_back(audio.get());
  request.items.push_back(queue.get());

  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("mono (1 channel)"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

// ===========================================================================
// OpenAI JSON input validation — no audio model needed
// ===========================================================================

TEST_F(AudioSessionTest, OpenAIJsonWithEmptyFileFieldThrows) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  nlohmann::json req_json = {
      {"model", "openai-whisper-tiny-generic-cpu-4"},
      {"filename", ""},
      {"language", "en"}};

  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>(req_json.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("Missing required field: filename"), std::string::npos)
              << "Expected 'Missing required field: filename' in: " << e.what();
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, OpenAIJsonWithNonexistentFileThrows) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  nlohmann::json req_json = {
      {"model", "openai-whisper-tiny-generic-cpu-4"},
      {"filename", "/no/such/path/audio.mp3"},
      {"language", "en"}};

  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>(req_json.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  Response response;
  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("Audio file not found"), std::string::npos)
              << "Expected 'Audio file not found' in: " << e.what();
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, OpenAIJsonWithInvalidJsonThrows) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>("not valid json {{{",
                                                  FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  Response response;
  EXPECT_THROW(session.ProcessRequest(request, response), nlohmann::json::parse_error);
}

// ===========================================================================
// Real inference tests — require the whisper model in the test cache.
// These run AudioSession::ProcessRequest directly (no web service).
// ===========================================================================

TEST_F(AudioSessionInferenceTest, TranscribeFromFilePath) {
  if (!model_) {
    GTEST_SKIP() << "Audio model not loaded";
  }

  auto audio_path = fl::test::GetTestDataPath("Recording.mp3");
  ASSERT_TRUE(fs::exists(audio_path)) << "Test audio file not found: " << audio_path;

  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  Request request;
  auto audio_item = std::make_unique<AudioItem>(audio_path.string());
  request.AddOwnedItem(std::move(audio_item));
  request.options["language"] = "en";

  Response response;
  ASSERT_NO_THROW(session.ProcessRequest(request, response));

  // Should produce at least one item
  ASSERT_FALSE(response.items.empty()) << "No items in response";

  std::string text;
  for (const auto& item : response.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_SPEECH_RESULT) {
      text = static_cast<SpeechResultItem&>(*item).text;
      break;
    }
  }

  EXPECT_FALSE(text.empty()) << "Transcription text should not be empty";
  ExpectTranscriptionContent(text);

  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
}

TEST_F(AudioSessionInferenceTest, TranscribeViaOpenAIJson) {
  if (!model_) {
    GTEST_SKIP() << "Audio model not loaded";
  }

  auto audio_path = fl::test::GetTestDataPath("Recording.mp3");
  ASSERT_TRUE(fs::exists(audio_path)) << "Test audio file not found: " << audio_path;

  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);

  nlohmann::json req_json = {
      {"model", "openai-whisper-tiny-generic-cpu-2"},
      {"filename", audio_path.string()},
      {"language", "en"}};

  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>(req_json.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  Response response;
  ASSERT_NO_THROW(session.ProcessRequest(request, response));

  // Should produce at least one item — and it should be an OPENAI_JSON-tagged TextItem.
  ASSERT_FALSE(response.items.empty()) << "No items in response";

  const Item* first_item = response.items.front().get();
  ASSERT_EQ(first_item->type, FOUNDRY_LOCAL_ITEM_TEXT)
      << "Expected TEXT item in response, got type " << static_cast<int>(first_item->type);

  const auto& text_item = static_cast<const TextItem&>(*first_item);
  ASSERT_EQ(text_item.text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON)
      << "Expected OPENAI_JSON text item, got subtype " << static_cast<int>(text_item.text_type);
  EXPECT_FALSE(text_item.text.empty()) << "OPENAI_JSON TextItem text should not be empty";

  // Parse and verify the AudioTranscriptionResponse
  auto resp_json = nlohmann::json::parse(text_item.text);
  ASSERT_TRUE(resp_json.contains("text")) << "Response JSON missing 'text' key: " << text_item.text;

  std::string text = resp_json["text"].get<std::string>();
  EXPECT_FALSE(text.empty()) << "Transcription text should not be empty";
  ExpectTranscriptionContent(text);

  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
  EXPECT_GT(response.usage.total_tokens, 0) << "Expected non-zero total_tokens";
}
