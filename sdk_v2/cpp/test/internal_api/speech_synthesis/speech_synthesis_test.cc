// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_catalog.h"
#include "exception.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/speech_synthesis/speech_synthesis_session.h"
#include "inferencing/model_load_manager.h"
#include "items/audio_item.h"
#include "items/item_queue.h"
#include "items/text_item.h"
#include "model.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fl {
namespace {

constexpr auto kInvalid = FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT;

/// The tiny VibeVoice package: five deterministic float16 graphs, a byte tokenizer and two voices. See
/// testdata/tiny-vibevoice/generate-model.py.
std::filesystem::path TinyVibeVoicePath() {
  return test::GetTestDataPath("tiny-vibevoice");
}

/// The fl::Exception code that `action` throws, or FOUNDRY_LOCAL_OK.
flErrorCode ErrorCode(const std::function<void()>& action) {
  try {
    action();
  } catch (const Exception& ex) {
    return ex.code();
  }

  return FOUNDRY_LOCAL_OK;
}

/// What the tiny graphs produce for `text` (with the "\n" that the text prep appends): four samples per frame.
std::vector<float> TinySpeech(const std::string& text, int64_t tts_prompt, float negative) {
  std::vector<int> ids(text.begin(), text.end());
  ids.push_back('\n');
  const size_t max_frames = 32 + 4 * ids.size();
  int64_t length = tts_prompt;
  float condition = 0.0f;
  std::vector<float> samples;
  for (size_t begin = 0;; begin += 5) {
    if (begin < ids.size()) {
      const auto end = std::min(begin + 5, ids.size());
      condition = 0.001f * static_cast<float>(ids[end - 1]) + 0.1f;
      length += static_cast<int64_t>(end - begin);
    }

    for (int i = 0; i < 6; ++i) {
      const float latent = negative + 2.0f * (condition - negative);
      samples.insert(samples.end(), 4, latent / 0.5f - 0.1f);
      condition = 0.5f * latent;
      ++length;
      if ((begin + 5 >= ids.size() && length > 40) || samples.size() / 4 >= max_frames) {
        return samples;
      }

      negative = condition;
    }
  }
}

class SpeechSynthesisSessionTest : public ::testing::Test {
 protected:
  static constexpr const char* kModelId = "tiny-vibevoice:1";

  static void SetUpTestSuite() {
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, test::NullLog());
    const auto result = load_manager_->LoadModel(TinyVibeVoicePath().string(), kModelId);
    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess);
    model_ = result.model;
  }

  static void TearDownTestSuite() {
    if (load_manager_) {
      load_manager_->UnloadModel(kModelId);
    }

    load_manager_.reset();
    ep_detector_.reset();
    model_ = nullptr;
  }

  void SetUp() override {
    ASSERT_NE(model_, nullptr);
  }

  static void ExpectAudio(const Item& item, const std::vector<float>& expected) {
    ASSERT_EQ(item.type, FOUNDRY_LOCAL_ITEM_AUDIO);
    const auto& audio = static_cast<const AudioItem&>(item);
    EXPECT_EQ(audio.format, "pcm");
    EXPECT_EQ(audio.sample_rate, 24000);
    EXPECT_EQ(audio.channels, 1);
    ASSERT_EQ(audio.data_size, expected.size() * 2);
    const auto* bytes = static_cast<const uint8_t*>(audio.data);
    for (size_t i = 0; i < expected.size(); ++i) {
      // The float16 graphs leave a small error in every sample.
      const auto sample = static_cast<int16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
      EXPECT_NEAR(sample / 32767.0f, expected[i], 0.02f) << "sample " << i;
    }
  }

  std::unique_ptr<SpeechSynthesisSession> MakeSession() {
    return std::make_unique<SpeechSynthesisSession>(catalog_model_, *model_, test::NullLog(),
                                                    test::TestTelemetrySink());
  }

  static Request MakeRequest(const std::string& text, std::map<std::string, std::string> options = {}) {
    Request request;
    request.AddOwnedItem(std::make_unique<TextItem>(text));
    for (auto& [key, value] : options) {
      request.options.Add(key, value);
    }

    return request;
  }

  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline test::FakeServiceBindings svc_;
  static inline Model catalog_model_ =
      Model::FromModelInfo(ModelInfo{}, "", svc_.download_manager, svc_.model_load_manager);
};

TEST_F(SpeechSynthesisSessionTest, SynthesizesText) {
  auto session = MakeSession();
  EXPECT_EQ(session->Type(), SessionType::kSpeechSynthesis);
  Response response;
  session->ProcessRequest(MakeRequest("Hello world."), response);
  ASSERT_EQ(response.items.size(), 1u);
  const auto expected = TinySpeech("Hello world.", 3, 0.05f);
  ExpectAudio(*response.items[0], expected);
  EXPECT_STREQ(response.items[0]->GetMetadata().Find("voice"), "Alpha");
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
  EXPECT_EQ(response.usage.prompt_tokens, 13);
  EXPECT_EQ(response.usage.completion_tokens, static_cast<int64_t>(expected.size() / 4));
  EXPECT_EQ(response.usage.total_tokens, 13 + static_cast<int64_t>(expected.size() / 4));

  Response again;
  session->ProcessRequest(MakeRequest("Hello world."), again);
  const auto& first = static_cast<const AudioItem&>(*response.items[0]);
  const auto& second = static_cast<const AudioItem&>(*again.items[0]);
  ASSERT_EQ(first.data_size, second.data_size);
  EXPECT_EQ(std::memcmp(first.data, second.data, first.data_size), 0) << "the package sets a fixed random seed";
}

TEST_F(SpeechSynthesisSessionTest, ReadsSpecialTokenTextLiterally) {
  auto session = MakeSession();
  Response response;
  session->ProcessRequest(MakeRequest("<|endoftext|>"), response);
  // 13 bytes and the "\n" of the text prep; the special token would be one token.
  EXPECT_EQ(response.usage.prompt_tokens, 14);
  ExpectAudio(*response.items[0], TinySpeech("<|endoftext|>", 3, 0.05f));
}

TEST_F(SpeechSynthesisSessionTest, AppliesVoiceAndAcceptsSpeedOne) {
  auto session = MakeSession();
  KeyValuePairs defaults;
  defaults.Add(FOUNDRY_LOCAL_PARAM_VOICE, "Beta");
  session->SetSessionOptions(defaults);

  Response beta;
  session->ProcessRequest(MakeRequest("Hello world.", {{FOUNDRY_LOCAL_PARAM_SPEED, "1.0"}}), beta);
  ExpectAudio(*beta.items[0], TinySpeech("Hello world.", 5, -0.05f));
  EXPECT_STREQ(beta.items[0]->GetMetadata().Find("voice"), "Beta");

  Response alpha;
  session->ProcessRequest(MakeRequest("Hello world.", {{FOUNDRY_LOCAL_PARAM_VOICE, "Alpha"}}), alpha);
  ExpectAudio(*alpha.items[0], TinySpeech("Hello world.", 3, 0.05f));
  EXPECT_STREQ(alpha.items[0]->GetMetadata().Find("voice"), "Alpha");
}

TEST_F(SpeechSynthesisSessionTest, StreamsOneAudioItemPerChunk) {
  auto session = MakeSession();
  std::vector<std::unique_ptr<Item>> streamed;
  session->SetStreamingCallback([&](flStreamingCallbackData event, void*) {
    auto* queue = reinterpret_cast<ItemQueue*>(event.item_queue);
    while (auto item = queue->TryPop()) {
      streamed.push_back(std::move(item));
    }

    return 0;
  });

  const std::vector<std::string> chunks = {"The first sentence is long enough.", "Hi. The second one is long too!",
                                           "A third line, also long."};
  Response response;
  session->ProcessRequest(MakeRequest(chunks[0] + " " + chunks[1] + "\n" + chunks[2]), response);
  ASSERT_EQ(streamed.size(), 3u);
  std::vector<float> combined;
  for (size_t i = 0; i < chunks.size(); ++i) {
    const auto expected = TinySpeech(chunks[i], 3, 0.05f);
    ExpectAudio(*streamed[i], expected);
    EXPECT_EQ(streamed[i]->GetMetadata().Find("text"), chunks[i]);
    combined.insert(combined.end(), expected.begin(), expected.end());
  }

  ExpectAudio(*response.items[0], combined);
}

TEST_F(SpeechSynthesisSessionTest, StopsAfterCancellation) {
  auto session = MakeSession();
  size_t streamed = 0;
  session->SetStreamingCallback([&](flStreamingCallbackData event, void*) {
    auto* queue = reinterpret_cast<ItemQueue*>(event.item_queue);
    while (queue->TryPop()) {
      ++streamed;
    }

    return 1;
  });

  // The callback runs on its own thread, so synthesis may finish more chunks before it sees the cancellation.
  Response response;
  const std::string line = "This sentence is long enough to be a chunk.\n";
  const auto code = ErrorCode([&] { session->ProcessRequest(MakeRequest(line + line + line + line), response); });
  EXPECT_EQ(code, FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  EXPECT_LE(streamed, 4u);

  streamed = 0;
  auto cancelled = MakeRequest(line + line);
  cancelled.CancelCurrentOrNext();
  Response cancelled_response;
  EXPECT_EQ(ErrorCode([&] { session->ProcessRequest(cancelled, cancelled_response); }),
            FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  EXPECT_EQ(streamed, 0u);
}

TEST_F(SpeechSynthesisSessionTest, RejectsInvalidRequests) {
  auto session = MakeSession();
  auto run = [&](Request request) {
    return ErrorCode([&] {
      Response response;
      session->ProcessRequest(request, response);
    });
  };

  EXPECT_EQ(run(Request{}), kInvalid);
  EXPECT_EQ(run(MakeRequest("hi", {{FOUNDRY_LOCAL_PARAM_SPEED, "1.5"}})), kInvalid);
  EXPECT_EQ(run(MakeRequest("hi", {{FOUNDRY_LOCAL_PARAM_SPEED, "fast"}})), kInvalid);
  EXPECT_EQ(run(MakeRequest("hi", {{FOUNDRY_LOCAL_PARAM_VOICE, "nobody"}})), kInvalid);
  EXPECT_EQ(run(MakeRequest(std::string(16385, 'a'))), kInvalid);
  // Continuation bytes alone count as no characters, so invalid UTF-8 must not get past the length limit.
  EXPECT_EQ(run(MakeRequest(std::string(20000, '\x80'))), kInvalid);
  EXPECT_EQ(run(MakeRequest("   \n  ")), FOUNDRY_LOCAL_OK);

  Request image;
  image.AddOwnedItem(Item::Create(FOUNDRY_LOCAL_ITEM_IMAGE));
  EXPECT_EQ(run(std::move(image)), kInvalid);
}

TEST_F(SpeechSynthesisSessionTest, RefusesUnloadWhileSessionIsAlive) {
  auto session = MakeSession();
  EXPECT_EQ(model_->SessionRefCount(), 1);
  EXPECT_EQ(ErrorCode([] { load_manager_->UnloadModel(kModelId); }), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  session.reset();
  EXPECT_EQ(model_->SessionRefCount(), 0);
}

TEST(SpeechSynthesisContext, RejectsChunkThatDoesNotFitTheContext) {
  // With one text token per window and 64 speech frames per window, 200 tokens need 199 * 64 frames, which does not
  // fit a 4096-token context. ORT GenAI rejects such a chunk instead of cutting it off.
  auto root = test::TempPath::CreateTempDir("speech_synthesis_context");
  const auto package = root.path() / "narrow";
  std::filesystem::copy(TinyVibeVoicePath(), package, std::filesystem::copy_options::recursive);
  auto config = nlohmann::json::parse(std::ifstream(package / "genai_config.json"));
  config["model"]["context_length"] = 4096;
  config["model"]["vibevoice"]["text_window_size"] = 1;
  config["model"]["vibevoice"]["speech_window_size"] = 64;
  std::ofstream(package / "genai_config.json", std::ios::trunc) << config.dump();

  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager load_manager(ep_detector, test::NullLog());
  const auto result = load_manager.LoadModel(package.string(), "narrow:1");
  ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess);
  test::FakeServiceBindings svc;
  const auto catalog_model = Model::FromModelInfo(ModelInfo{}, "", svc.download_manager, svc.model_load_manager);
  {
    SpeechSynthesisSession session(catalog_model, *result.model, test::NullLog(), test::TestTelemetrySink());
    Request request;
    request.AddOwnedItem(std::make_unique<TextItem>(std::string(199, 'a')));
    Response response;
    EXPECT_EQ(ErrorCode([&] { session.ProcessRequest(request, response); }), kInvalid);
  }

  load_manager.UnloadModel("narrow:1");
}

TEST(SpeechSynthesisRegistration, RegistersPackageInLocalCatalog) {
  auto root = test::TempPath::CreateTempDir("speech_synthesis_catalog");
  test::FakeServiceBindings bindings;
  LocalModelCatalog catalog(
      root.path() / "cache" / "models",
      [&](ModelInfo info, std::string path) {
        return Model::FromLocalRegistration(std::move(info), std::move(path), bindings.download_manager,
                                            bindings.model_load_manager);
      },
      test::NullLog());

  ModelInfo metadata;
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "text-to-speech");
  auto* model = catalog.RegisterModel(TinyVibeVoicePath().string(), "vibevoice-test:1", metadata);
  ASSERT_NE(model, nullptr);
  EXPECT_TRUE(model->IsCached());
  EXPECT_EQ(model->Info().task, "text-to-speech");
  EXPECT_EQ(*model->Info().GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR), "text");
  EXPECT_EQ(*model->Info().GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR), "audio");
  const auto io = model->GetInputOutputInfo();
  ASSERT_EQ(io.num_inputs, 1u);
  ASSERT_EQ(io.num_outputs, 1u);
  EXPECT_EQ(io.inputs[0]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(io.outputs[0]->type, FOUNDRY_LOCAL_ITEM_AUDIO);
}

}  // namespace
}  // namespace fl
