// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Integration tests for streaming audio transcription via the public C++ API.
// These exercise AudioItem + ItemQueue → Session::ProcessRequest directly.
// Requires an audio model in the test cache and testdata/Recording.pcm
// (raw s16le, 16 kHz, mono — the WAV-data section of Recording.wav, no RIFF header).
#include "model_fixture.h"

#include "utils/string_utils.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using fl::test::ToLower;

// ========================================================================
// StreamingAudioFixture — exercises AudioItem + ItemQueue with a nemotron
// streaming-audio model. Doesn't need chat; inherits Test directly so we
// don't load the chat model just to skip past the chat assertion.
// ========================================================================

class StreamingAudioFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::StreamingAudio});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.streaming_audio_model()) {
      GTEST_SKIP() << "No nemotron streaming audio model available";
    }

#ifdef FOUNDRY_LOCAL_TEST_DATA_DIR
    pcm_path_ = fs::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "Recording.pcm";
#else
    pcm_path_ = fs::current_path() / "testdata" / "Recording.pcm";
#endif
    if (!fs::exists(pcm_path_)) {
      GTEST_SKIP() << "testdata/Recording.pcm not found";
    }
  }

  static foundry_local::IModel& audio_model() {
    return *SharedTestEnv::Get().streaming_audio_model();
  }

  /// Load the entire PCM file into memory.
  std::vector<uint8_t> LoadPcm() const {
    std::ifstream in(pcm_path_, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  /// Split a byte buffer into chunks of the given size.
  static std::vector<std::vector<uint8_t>> SplitIntoChunks(
      const std::vector<uint8_t>& data, size_t chunk_size) {
    std::vector<std::vector<uint8_t>> chunks;

    for (size_t offset = 0; offset < data.size(); offset += chunk_size) {
      size_t len = std::min(chunk_size, data.size() - offset);
      chunks.emplace_back(data.begin() + offset, data.begin() + offset + len);
    }

    return chunks;
  }

  /// Verify that the transcription contains key phrases from the expected output.
  static void ExpectTranscriptionContent(const std::string& text) {
    std::string lower = ToLower(text);

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

  fs::path pcm_path_;
};

// ========================================================================
// Tests
// ========================================================================

TEST_F(StreamingAudioFixture, StreamRecordingInChunksAndValidateTranscription) {
  using namespace foundry_local;

  auto pcm = LoadPcm();
  ASSERT_GT(pcm.size(), 0u) << "Recording.pcm is empty";

  // 100ms chunks at 16kHz mono s16le = 3200 bytes each
  auto chunks = SplitIntoChunks(pcm, 3200);
  ASSERT_GT(chunks.size(), 1u);

  // Format descriptor — no initial data
  auto audio = Item::AudioFromData("pcm", nullptr, 0, /*sample_rate=*/16000, /*channels=*/1);
  ItemQueue queue;

  Request request;
  request.AddItem(audio, /*take_ownership=*/true);   // we don't need to keep this alive
  request.AddItem(queue, /*take_ownership=*/false);  // we need to keep this alive to stream data

  AudioSession session(audio_model());
  auto future = std::async(std::launch::async, [&]() {
    return session.ProcessRequest(request);
  });

  // Stream all chunks
  for (const auto& chunk : chunks) {
    queue.Push(Item::Bytes(FOUNDRY_LOCAL_ITEM_BYTES, chunk.data(), chunk.size()));
  }

  queue.MarkFinished();

  Response response = future.get();

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);
  ASSERT_FALSE(response.GetItems().empty()) << "Expected output items";

  std::string text = CollectResponseText(response);
  EXPECT_FALSE(text.empty()) << "Transcription should not be empty";
  ExpectTranscriptionContent(text);

  std::cout << "Streaming transcription: " << text << "\n";
}

TEST_F(StreamingAudioFixture, StreamRecordingWithInitialData) {
  using namespace foundry_local;

  auto pcm = LoadPcm();
  ASSERT_GT(pcm.size(), 0u);

  // Put first 32000 bytes (~1 second) in the AudioItem, stream the rest
  const size_t initial_size = std::min<size_t>(pcm.size(), 32000);
  auto audio = Item::AudioFromData("pcm", pcm.data(), initial_size,
                                   /*sample_rate=*/16000, /*channels=*/1);
  ItemQueue queue;

  Request request;
  request.AddItem(audio, /*take_ownership=*/false);
  request.AddItem(queue, /*take_ownership=*/false);

  AudioSession session(audio_model());
  auto future = std::async(std::launch::async, [&]() {
    return session.ProcessRequest(request);
  });

  // Stream the remainder in 100ms chunks
  auto remainder = std::vector<uint8_t>(pcm.begin() + initial_size, pcm.end());
  auto chunks = SplitIntoChunks(remainder, 3200);

  for (const auto& chunk : chunks) {
    queue.Push(Item::Bytes(FOUNDRY_LOCAL_ITEM_BYTES, chunk.data(), chunk.size()));
  }

  queue.MarkFinished();

  Response response = future.get();

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);
  std::string text = CollectResponseText(response);
  EXPECT_FALSE(text.empty()) << "Transcription should not be empty";
  ExpectTranscriptionContent(text);
}

TEST_F(StreamingAudioFixture, EmptyQueueProducesEmptyOutput) {
  using namespace foundry_local;

  auto audio = Item::AudioFromData("pcm", nullptr, 0, /*sample_rate=*/16000, /*channels=*/1);
  ItemQueue queue;

  Request request;
  request.AddItem(audio, /*take_ownership=*/false);
  request.AddItem(queue, /*take_ownership=*/false);

  AudioSession session(audio_model());

  // Immediately mark finished — no audio data
  queue.MarkFinished();

  Response response = session.ProcessRequest(request);

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);
}

TEST_F(StreamingAudioFixture, CancellationMidStream) {
  using namespace foundry_local;

  auto pcm = LoadPcm();
  auto chunks = SplitIntoChunks(pcm, 3200);

  auto audio = Item::AudioFromData("pcm", nullptr, 0, /*sample_rate=*/16000, /*channels=*/1);
  ItemQueue queue;

  Request request;
  request.AddItem(audio, /*take_ownership=*/false);
  request.AddItem(queue, /*take_ownership=*/false);

  AudioSession session(audio_model());
  auto future = std::async(std::launch::async, [&]() {
    return session.ProcessRequest(request);
  });

  // Push roughly half the chunks, then cancel
  size_t half = chunks.size() / 2;
  for (size_t i = 0; i < half; ++i) {
    queue.Push(Item::Bytes(FOUNDRY_LOCAL_ITEM_BYTES, chunks[i].data(), chunks[i].size()));
  }

  request.Cancel();
  queue.MarkFinished();

  Response response = future.get();

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_NONE)
      << "Cancelled request should have NONE finish reason";
}

TEST_F(StreamingAudioFixture, StreamingCallbackReceivesTokens) {
  using namespace foundry_local;

  auto pcm = LoadPcm();
  auto chunks = SplitIntoChunks(pcm, 3200);

  auto audio = Item::AudioFromData("pcm", nullptr, 0, /*sample_rate=*/16000, /*channels=*/1);
  ItemQueue queue;

  Request request;
  request.AddItem(audio, /*take_ownership=*/false);
  request.AddItem(queue, /*take_ownership=*/false);

  AudioSession session(audio_model());

  // Collect streamed tokens via the callback.
  std::atomic<int> callback_count{0};
  std::mutex text_mutex;
  std::string streamed_text;

  session.SetStreamingCallback([&](flStreamingCallbackData data) {
    // Pop the item from the queue — this is the established contract.
    flItem* raw_item = nullptr;
    if (!detail::item_api()->ItemQueue_TryPop(data.item_queue, &raw_item) || !raw_item) {
      return 0;
    }

    // Wrap in Item for RAII release and checked accessors.
    Item item(*raw_item);

    // Audio output is always a SpeechSegmentItem per token.
    EXPECT_EQ(item.GetType(), FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT);
    auto seg = item.GetSpeechSegment();
    if (!seg.text.empty()) {
      std::lock_guard<std::mutex> lock(text_mutex);
      streamed_text.append(seg.text.data(), seg.text.size());
    }

    callback_count++;
    return 0;  // continue
  });

  auto future = std::async(std::launch::async, [&]() {
    return session.ProcessRequest(request);
  });

  for (const auto& chunk : chunks) {
    queue.Push(Item::Bytes(FOUNDRY_LOCAL_ITEM_BYTES, chunk.data(), chunk.size()));
  }

  queue.MarkFinished();

  Response response = future.get();

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);

  std::string full_text = CollectResponseText(response);

  EXPECT_FALSE(full_text.empty()) << "Expected non-empty transcription";
  EXPECT_GT(callback_count.load(), 0)
      << "Streaming callback should have been called at least once";
  ExpectTranscriptionContent(full_text);

  // Streamed text should match the final response text.
  {
    std::lock_guard<std::mutex> lock(text_mutex);
    EXPECT_EQ(streamed_text, full_text)
        << "Streamed callback text should match final response text";
  }

  std::cout << "Streaming callback test: " << callback_count.load()
            << " callbacks, text: " << full_text << "\n";
}
