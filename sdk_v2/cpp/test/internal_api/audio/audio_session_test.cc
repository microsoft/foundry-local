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
#include "logger.h"
#include "model.h"
#include "internal_api/null_session_manager.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace fl;

namespace fl {
class AudioSessionTestAccessor {
 public:
  static std::vector<float> LoadPcmWavAsFloatSamples(const std::string& audio_file_path) {
    return AudioSession::LoadPcmWavAsFloatSamples(audio_file_path);
  }
};
}  // namespace fl

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

template <typename T>
void WriteLe(std::ofstream& out, T value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

void WriteBytes(std::ofstream& out, const char* data, size_t size) {
  out.write(data, static_cast<std::streamsize>(size));
}

void WriteWavPcm16(const std::filesystem::path& path, int sample_rate_hz, int channels,
                   const std::vector<int16_t>& samples) {
  const uint16_t audio_format = 1;  // PCM
  const uint16_t bits_per_sample = 16;
  const uint16_t block_align = static_cast<uint16_t>((channels * bits_per_sample) / 8);
  const uint32_t byte_rate = static_cast<uint32_t>(sample_rate_hz * block_align);
  const uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
  const uint32_t riff_size = 4 + (8 + 16) + (8 + data_size);

  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out.is_open()) << "Failed to create temp WAV: " << path.string();

  WriteBytes(out, "RIFF", 4);
  WriteLe<uint32_t>(out, riff_size);
  WriteBytes(out, "WAVE", 4);

  WriteBytes(out, "fmt ", 4);
  WriteLe<uint32_t>(out, 16);
  WriteLe<uint16_t>(out, audio_format);
  WriteLe<uint16_t>(out, static_cast<uint16_t>(channels));
  WriteLe<uint32_t>(out, static_cast<uint32_t>(sample_rate_hz));
  WriteLe<uint32_t>(out, byte_rate);
  WriteLe<uint16_t>(out, block_align);
  WriteLe<uint16_t>(out, bits_per_sample);

  WriteBytes(out, "data", 4);
  WriteLe<uint32_t>(out, data_size);
  if (!samples.empty()) {
    out.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
  }
}

void WriteWavFloat32(const std::filesystem::path& path, int sample_rate_hz, int channels,
                     const std::vector<float>& samples) {
  const uint16_t audio_format = 3;  // IEEE float
  const uint16_t bits_per_sample = 32;
  const uint16_t block_align = static_cast<uint16_t>((channels * bits_per_sample) / 8);
  const uint32_t byte_rate = static_cast<uint32_t>(sample_rate_hz * block_align);
  const uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(float));
  const uint32_t riff_size = 4 + (8 + 16) + (8 + data_size);

  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out.is_open()) << "Failed to create temp WAV: " << path.string();

  WriteBytes(out, "RIFF", 4);
  WriteLe<uint32_t>(out, riff_size);
  WriteBytes(out, "WAVE", 4);

  WriteBytes(out, "fmt ", 4);
  WriteLe<uint32_t>(out, 16);
  WriteLe<uint16_t>(out, audio_format);
  WriteLe<uint16_t>(out, static_cast<uint16_t>(channels));
  WriteLe<uint32_t>(out, static_cast<uint32_t>(sample_rate_hz));
  WriteLe<uint32_t>(out, byte_rate);
  WriteLe<uint16_t>(out, block_align);
  WriteLe<uint16_t>(out, bits_per_sample);

  WriteBytes(out, "data", 4);
  WriteLe<uint32_t>(out, data_size);
  if (!samples.empty()) {
    out.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(samples.size() * sizeof(float)));
  }
}

void WriteWavWithDataChunkSize(const std::filesystem::path& path, uint32_t data_size) {
  const uint32_t riff_size = 4 + (8 + 16) + (8 + data_size);
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out.is_open()) << "Failed to create temp WAV: " << path.string();

  WriteBytes(out, "RIFF", 4);
  WriteLe<uint32_t>(out, riff_size);
  WriteBytes(out, "WAVE", 4);
  WriteBytes(out, "fmt ", 4);
  WriteLe<uint32_t>(out, 16);
  WriteLe<uint16_t>(out, 1);      // PCM
  WriteLe<uint16_t>(out, 1);      // mono
  WriteLe<uint32_t>(out, 16000);  // sample rate
  WriteLe<uint32_t>(out, 32000);  // byte rate (16-bit mono at 16kHz)
  WriteLe<uint16_t>(out, 2);      // block align
  WriteLe<uint16_t>(out, 16);     // bits per sample
  WriteBytes(out, "data", 4);
  WriteLe<uint32_t>(out, data_size);

  if (data_size > 0) {
    out.seekp(static_cast<std::streamoff>(data_size) - 1, std::ios::cur);
    out.put('\0');
  }
}

void WriteRawFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out.is_open()) << "Failed to create temp file: " << path.string();
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
}

class ScopedModelTypeOverride {
 public:
  ScopedModelTypeOverride(GenAIModelInstance& model, std::string model_type)
      : cfg_(const_cast<GenAIConfig&>(model.GetGenAIConfig())), old_model_(cfg_.model) {
    if (!cfg_.model.has_value()) {
      cfg_.model = GenAIConfig::OnnxModel{};
    }
    cfg_.model->type = std::move(model_type);
  }

  ~ScopedModelTypeOverride() {
    cfg_.model = old_model_;
  }

 private:
  GenAIConfig& cfg_;
  std::optional<GenAIConfig::OnnxModel> old_model_;
};

Request BuildOpenAiJsonAudioRequest(const std::filesystem::path& file_path) {
  nlohmann::json req_json = {
      {"model", "nemotron-speech-streaming-en-0.6b-generic-cpu"},
      {"filename", file_path.string()},
      {"language", "en"}};

  Request request;
  request.AddOwnedItem(std::make_unique<TextItem>(req_json.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  return request;
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
  TelemetryLogger null_telemetry_{"test", fl::test::NullLog()};
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
  TelemetryLogger null_telemetry_{"test", fl::test::NullLog()};
  fl::test::NullSessionManager null_session_manager_;
};

class AudioSessionNemotronInferenceTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);

    fs::path cache_dir;
    try {
      cache_dir = fl::test::GetTestModelCacheDir();
    } catch (const std::exception&) {
      GTEST_SKIP() << "Model cache unavailable — skipping Nemotron inference test";
      return;
    }

    std::optional<std::string> nemotron_alias;
    for (const auto& entry : fs::directory_iterator(cache_dir)) {
      if (!entry.is_directory()) {
        continue;
      }
      const auto alias = entry.path().filename().string();
      std::string lower = alias;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lower.find("nemotron") != std::string::npos) {
        nemotron_alias = alias;
        break;
      }
    }

    if (!nemotron_alias.has_value()) {
      GTEST_SKIP() << "No nemotron model found in test cache";
      return;
    }

    nemotron_alias_ = *nemotron_alias;
    fs::path model_path = fl::test::GetTestModelPath(nemotron_alias_);
    auto result = load_manager_->LoadModel(model_path.string(), nemotron_alias_);
    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess)
        << "Failed to load Nemotron model from: " << model_path;
    model_ = result.model;
  }

  static void TearDownTestSuite() {
    if (load_manager_ && !nemotron_alias_.empty()) {
      load_manager_->UnloadModel(nemotron_alias_);
    }
    load_manager_.reset();
    ep_detector_.reset();
    model_ = nullptr;
    nemotron_alias_.clear();
  }

  GenAIModelInstance& GetModel() { return *model_; }
  const Model& GetCatalogModel() { return catalog_model_; }

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline std::string nemotron_alias_;
  static inline fl::test::FakeServiceBindings svc_;
  static inline Model catalog_model_ = Model::FromModelInfo(
      ModelInfo{}, "", svc_.download_manager, svc_.model_load_manager);
  TelemetryLogger null_telemetry_{"test", fl::test::NullLog()};
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

TEST_F(AudioSessionTest, NemotronOpenAIJsonWithInvalidRiffThrows) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  ScopedModelTypeOverride force_nemotron(GetModel(), "nemotron_speech");
  auto temp = fl::test::TempPath::CreateTempFile("audio_bad_riff_");
  WriteRawFile(temp.path(), {0x4E, 0x4F, 0x54, 0x57});  // "NOTW"

  auto request = BuildOpenAiJsonAudioRequest(temp.path());
  Response response;

  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("RIFF/WAVE"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, NemotronOpenAIJsonWithCorruptedChunkBoundsThrows) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  ScopedModelTypeOverride force_nemotron(GetModel(), "nemotron_speech");
  auto temp = fl::test::TempPath::CreateTempFile("audio_bad_chunk_");

  // RIFF/WAVE + a "data" chunk that claims size far larger than file content.
  std::vector<uint8_t> bytes = {
      'R', 'I', 'F', 'F', 0x24, 0x00, 0x00, 0x00, 'W', 'A', 'V', 'E',
      'd', 'a', 't', 'a', 0xFF, 0xFF, 0x00, 0x00};
  WriteRawFile(temp.path(), bytes);

  auto request = BuildOpenAiJsonAudioRequest(temp.path());
  Response response;

  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("Corrupted WAV chunk"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, NemotronOpenAIJsonMissingFmtBeforeDataThrows) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  ScopedModelTypeOverride force_nemotron(GetModel(), "nemotron_speech");
  auto temp = fl::test::TempPath::CreateTempFile("audio_missing_fmt_");

  // Valid RIFF/WAVE header, but "data" appears before any "fmt " chunk.
  std::vector<uint8_t> bytes = {
      'R', 'I', 'F', 'F', 0x14, 0x00, 0x00, 0x00, 'W', 'A', 'V', 'E',
      'd', 'a', 't', 'a', 0x00, 0x00, 0x00, 0x00};
  WriteRawFile(temp.path(), bytes);

  auto request = BuildOpenAiJsonAudioRequest(temp.path());
  Response response;

  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("Missing WAV fmt chunk before data"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, NemotronOpenAIJsonRejectsUnsupportedSampleRateBeforeDataRead) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  ScopedModelTypeOverride force_nemotron(GetModel(), "nemotron_speech");
  auto temp = fl::test::TempPath::CreateTempFile("audio_bad_sr_");
  // 8 kHz PCM WAV should fail fast in fmt parsing path.
  WriteWavPcm16(temp.path(), /*sample_rate_hz=*/8000, /*channels=*/1, {0, 1, 2, 3});

  auto request = BuildOpenAiJsonAudioRequest(temp.path());
  Response response;

  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("16kHz"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, NemotronOpenAIJsonRejectsUnsupportedWavFormatInFmtChunk) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  ScopedModelTypeOverride force_nemotron(GetModel(), "nemotron_speech");
  auto temp = fl::test::TempPath::CreateTempFile("audio_bad_fmt_");

  // RIFF/WAVE with fmt chunk that advertises PCM 24-bit (unsupported by this path).
  std::ofstream out(temp.path(), std::ios::binary);
  ASSERT_TRUE(out.is_open()) << "Failed to create temp WAV: " << temp.path().string();
  WriteBytes(out, "RIFF", 4);
  WriteLe<uint32_t>(out, 36);
  WriteBytes(out, "WAVE", 4);
  WriteBytes(out, "fmt ", 4);
  WriteLe<uint32_t>(out, 16);
  WriteLe<uint16_t>(out, 1);      // PCM
  WriteLe<uint16_t>(out, 1);      // mono
  WriteLe<uint32_t>(out, 16000);  // sample rate
  WriteLe<uint32_t>(out, 48000);  // byte rate for 24-bit mono
  WriteLe<uint16_t>(out, 3);      // block align
  WriteLe<uint16_t>(out, 24);     // unsupported bits-per-sample
  WriteBytes(out, "data", 4);
  WriteLe<uint32_t>(out, 0);
  out.flush();
  out.close();

  auto request = BuildOpenAiJsonAudioRequest(temp.path());
  Response response;

  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("Unsupported WAV format"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
}

TEST_F(AudioSessionTest, LoadPcmWavAsFloatSamples_DecodesFloat32Path) {
  auto temp = fl::test::TempPath::CreateTempFile("audio_float32_");
  const std::vector<float> input = {-0.75f, -0.25f, 0.0f, 0.25f, 0.75f};
  WriteWavFloat32(temp.path(), /*sample_rate_hz=*/16000, /*channels=*/1, input);

  auto samples = fl::AudioSessionTestAccessor::LoadPcmWavAsFloatSamples(temp.path().string());
  ASSERT_EQ(samples.size(), input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    EXPECT_NEAR(samples[i], input[i], 1e-6f);
  }
}

TEST_F(AudioSessionTest, NemotronOpenAIJsonRejectsOversizedWavDataChunkBeforeAllocation) {
  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  ScopedModelTypeOverride force_nemotron(GetModel(), "nemotron_speech");

  auto temp = fl::test::TempPath::CreateTempFile("audio_oversized_data_");
  constexpr uint32_t kOversizedDataBytes = (64u * 1024u * 1024u) + 1u;
  WriteWavWithDataChunkSize(temp.path(), kOversizedDataBytes);

  auto request = BuildOpenAiJsonAudioRequest(temp.path());
  Response response;

  EXPECT_THROW(
      {
        try {
          session.ProcessRequest(request, response);
        } catch (const fl::Exception& e) {
          EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
          EXPECT_NE(std::string(e.what()).find("maximum supported size"), std::string::npos);
          throw;
        }
      },
      fl::Exception);
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

TEST_F(AudioSessionNemotronInferenceTest, OpenAIJsonNemotronFileTranscriptionMultiChunkNotTruncated) {
  if (!model_) {
    GTEST_SKIP() << "Nemotron model not loaded";
  }

  auto pcm_path = fl::test::GetTestDataPath("Recording.pcm");
  ASSERT_TRUE(fs::exists(pcm_path)) << "Recording.pcm not found: " << pcm_path;

  std::ifstream pcm_in(pcm_path, std::ios::binary);
  ASSERT_TRUE(pcm_in.is_open()) << "Failed to open Recording.pcm";
  std::vector<char> pcm_bytes((std::istreambuf_iterator<char>(pcm_in)),
                              std::istreambuf_iterator<char>());
  ASSERT_GT(pcm_bytes.size(), 3200u) << "Need multi-chunk PCM input";
  ASSERT_EQ(pcm_bytes.size() % sizeof(int16_t), 0u) << "PCM file byte count must align to int16";

  std::vector<int16_t> pcm_samples(pcm_bytes.size() / sizeof(int16_t));
  std::memcpy(pcm_samples.data(), pcm_bytes.data(), pcm_bytes.size());

  auto wav_temp = fl::test::TempPath::CreateTempFile("nemotron_multichunk_");
  WriteWavPcm16(wav_temp.path(), /*sample_rate_hz=*/16000, /*channels=*/1, pcm_samples);

  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  auto request = BuildOpenAiJsonAudioRequest(wav_temp.path());
  request.options["language"] = "en";

  Response response;
  ASSERT_NO_THROW(session.ProcessRequest(request, response));
  ASSERT_FALSE(response.items.empty()) << "No response items from Nemotron transcription";

  const Item* first_item = response.items.front().get();
  ASSERT_EQ(first_item->type, FOUNDRY_LOCAL_ITEM_TEXT);
  const auto& text_item = static_cast<const TextItem&>(*first_item);
  ASSERT_EQ(text_item.text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);

  auto resp_json = nlohmann::json::parse(text_item.text);
  ASSERT_TRUE(resp_json.contains("text"));
  std::string text = resp_json["text"].get<std::string>();
  EXPECT_FALSE(text.empty()) << "Nemotron transcript should not be empty";
  ExpectTranscriptionContent(text);
}

TEST_F(AudioSessionNemotronInferenceTest, OpenAIJsonNemotronFileTranscriptionAcceptsFloat32Wav) {
  if (!model_) {
    GTEST_SKIP() << "Nemotron model not loaded";
  }

  auto pcm_path = fl::test::GetTestDataPath("Recording.pcm");
  ASSERT_TRUE(fs::exists(pcm_path)) << "Recording.pcm not found: " << pcm_path;

  std::ifstream pcm_in(pcm_path, std::ios::binary);
  ASSERT_TRUE(pcm_in.is_open()) << "Failed to open Recording.pcm";
  std::vector<char> pcm_bytes((std::istreambuf_iterator<char>(pcm_in)), std::istreambuf_iterator<char>());
  ASSERT_GT(pcm_bytes.size(), 0u);
  ASSERT_EQ(pcm_bytes.size() % sizeof(int16_t), 0u) << "PCM file byte count must align to int16";

  std::vector<float> float_samples(pcm_bytes.size() / sizeof(int16_t));
  for (size_t i = 0; i < float_samples.size(); ++i) {
    int16_t sample = 0;
    std::memcpy(&sample, pcm_bytes.data() + (i * sizeof(int16_t)), sizeof(sample));
    float_samples[i] = static_cast<float>(sample) / 32768.0f;
  }

  auto wav_temp = fl::test::TempPath::CreateTempFile("nemotron_float32_");
  WriteWavFloat32(wav_temp.path(), /*sample_rate_hz=*/16000, /*channels=*/1, float_samples);

  AudioSession session(GetCatalogModel(), GetModel(), *logger_, null_telemetry_);
  auto request = BuildOpenAiJsonAudioRequest(wav_temp.path());
  request.options["language"] = "en";

  Response response;
  ASSERT_NO_THROW(session.ProcessRequest(request, response));
  ASSERT_FALSE(response.items.empty()) << "No response items from Nemotron transcription";

  const Item* first_item = response.items.front().get();
  ASSERT_EQ(first_item->type, FOUNDRY_LOCAL_ITEM_TEXT);
  const auto& text_item = static_cast<const TextItem&>(*first_item);
  ASSERT_EQ(text_item.text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);

  auto resp_json = nlohmann::json::parse(text_item.text);
  ASSERT_TRUE(resp_json.contains("text"));
  std::string text = resp_json["text"].get<std::string>();
  EXPECT_FALSE(text.empty()) << "Nemotron float32 WAV transcript should not be empty";
  ExpectTranscriptionContent(text);
}
