// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Example: Realtime audio session with the Foundry Local C++ SDK.
// Demonstrates: streaming audio input via an ItemQueue of bytes items,
// streaming text output via a callback, and reading the final Response.

#include <foundry_local/foundry_local_cpp.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace foundry_local;

/// Simulate reading audio from a file in small chunks, as if receiving from a microphone.
/// In a real application this would come from an audio capture API.
std::vector<uint8_t> LoadAudioFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("Failed to open audio file: " + path);
  }

  auto size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(buffer.data()), size);
  return buffer;
}

void RealtimeAudioChat(IModel& model, const std::string& audio_path) {
  // 1. Create a session for realtime audio.
  AudioSession session(model);

  // 2. Set a streaming callback to receive incremental text as words are generated.
  //    Each callback invocation delivers one item in the queue.

  // lambda can capture user data if needed (equivalent to the additional `void* user_data` parameter in the C API).
  session.SetStreamingCallback([/*user_data*/](flStreamingCallbackData event) -> int {
    const auto* item_api = detail::item_api();

    flItem* raw_item = nullptr;
    if (item_api->ItemQueue_TryPop(event.item_queue, &raw_item)) {
      Item item(*raw_item);
      if (item.GetType() == FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT) {
        auto seg = item.GetSpeechSegment();
        std::cout.write(seg.text.data(), seg.text.size());
        std::cout.flush();
      } else {
        std::cerr << "Unexpected item type" << std::endl;
      }

      // ~Item calls release.
    } else {
      // should never happen. adding item to queue to callback should be 1:1.
      std::cerr << "Callback invoked but no item in queue" << std::endl;
    }

    return 0;  // return non-zero to cancel
  });

  // 3. Create an ItemQueue to hold the streamed audio chunks.
  //    The queue lets us push audio data incrementally while ProcessRequest is running.
  ItemQueue audio_input;

  // 4. Build the request.
  //    First item: AudioItem format descriptor (pcm, 16kHz, mono) — tells the session how to interpret the stream.
  //    Second item: ItemQueue — added with take_ownership=false so we retain it to push chunks.
  Request request;
  request.AddItem(Item::AudioFromData("pcm", nullptr, 0, 16000, 1));
  request.AddItem(audio_input, /*take_ownership*/ false);

  auto audio_data = LoadAudioFile(audio_path);

  // 5. Simulate streaming audio in 4KB chunks on a background thread.
  //    In a real application, this would be a microphone capture loop.
  constexpr size_t kChunkSize = 4096;
  std::thread producer([&]() {
    size_t offset = 0;
    while (offset < audio_data.size()) {
      size_t chunk_size = std::min(kChunkSize, audio_data.size() - offset);
      Item chunk = Item::Bytes(FOUNDRY_LOCAL_ITEM_BYTES, audio_data.data() + offset, chunk_size);
      audio_input.Push(std::move(chunk));
      offset += chunk_size;
      std::this_thread::sleep_for(std::chrono::milliseconds(125));
    }

    audio_input.MarkFinished();
  });

  // 6. Process the request. This blocks until inference completes.
  //    The streaming callback fires on a separate thread as words are generated.
  std::cout << "Transcription: ";
  Response response = [&]() -> Response {
    try {
      return session.ProcessRequest(request);
    } catch (...) {
      // Ensure the producer thread is signaled and joined before propagating,
      // otherwise it would be detached at end-of-scope and outlive `audio_input`.
      audio_input.MarkFinished();
      if (producer.joinable()) {
        producer.join();
      }

      throw;
    }
  }();
  std::cout << "\n";

  producer.join();

  // 7. Check finish reason and usage.
  flFinishReason reason = response.GetFinishReason();
  switch (reason) {
    case FOUNDRY_LOCAL_FINISH_STOP:
      std::cout << "Finished: stop\n";
      break;
    case FOUNDRY_LOCAL_FINISH_LENGTH:
      std::cout << "Finished: max length reached\n";
      break;
    case FOUNDRY_LOCAL_FINISH_ERROR:
      std::cerr << "Finished: error\n";
      break;
    default:
      std::cout << "Finished: reason=" << static_cast<int>(reason) << "\n";
      break;
  }

  flUsage usage = response.GetUsage();
  std::cout << "Tokens - prompt: " << usage.prompt_tokens
            << ", completion: " << usage.completion_tokens
            << ", total: " << usage.total_tokens << "\n";

  // 8. The full response is a single SpeechResultItem carrying the complete
  // transcript and per-segment detail.
  const auto& items = response.GetItems();
  if (!items.empty()) {
    const auto& item = items.front();
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_SPEECH_RESULT) {
      auto result = item.GetSpeechResult();
      std::cout << "Full response: ";
      std::cout.write(result.text.data(), result.text.size());
      std::cout << "\n";
      std::cout << "Segments: " << result.segments.size() << "\n";
    } else {
      std::cerr << "Unexpected item type" << std::endl;
    }
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: realtime_audio_example <audio_file_path>\n";
    return 1;
  }

  const std::string audio_path = argv[1];

  try {
    // 1. Create configuration and manager.
    Configuration config("realtime_audio");
    Manager manager(std::move(config));

    // 2. Find a nemotron model that supports streaming audio transcription.
    auto& catalog = manager.GetCatalog();
    auto model = catalog.GetModel("nemotron-speech-streaming-en-0.6b");
    if (!model) {
      std::cerr << "Audio model not found. Available models:\n";
      ModelList all = catalog.GetModels();
      for (const auto& m : all.Models()) {
        std::cout << "  " << m->GetInfo().Alias() << "\n";
      }
      return 1;
    }

    std::cout << "Using model: " << model->GetInfo().Name() << "\n";

    // 3. Download if needed.
    if (!model->IsCached()) {
      std::cout << "Downloading...\n";
      model->Download([](float progress) -> bool {
        std::cout << "\r  " << static_cast<int>(progress) << "%" << std::flush;
        return true;
      });
      std::cout << "\n";
    }

    // 4. Load the model.
    if (!model->IsLoaded()) {
      model->Load();
    }

    // 5. Run the realtime audio session.
    RealtimeAudioChat(*model, audio_path);

    // 6. Cleanup.
    model->Unload();
  } catch (const Error& e) {
    std::cerr << "Foundry error: " << e.what() << " (code " << e.Code() << ")\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
