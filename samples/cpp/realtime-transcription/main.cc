// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_cpp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <future>
#include <iostream>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#ifdef FOUNDRY_SAMPLE_HAS_PORTAUDIO
#include <portaudio.h>
#endif

#include "samples/cpp/sample_common.h"

namespace {

constexpr int kSampleRate = 16'000;
constexpr size_t kFramesPerChunk = 1'600;

std::vector<uint8_t> MakeSyntheticAudio() {
  constexpr int kDurationSeconds = 2;
  constexpr double kFrequency = 440.0;
  std::vector<uint8_t> pcm(kSampleRate * kDurationSeconds * sizeof(int16_t));

  for (size_t index = 0; index < pcm.size() / sizeof(int16_t); ++index) {
    const auto time = static_cast<double>(index) / kSampleRate;
    const auto sample = static_cast<int16_t>(
        std::sin(2.0 * std::numbers::pi * kFrequency * time) * 0.25 * INT16_MAX);
    pcm[index * 2] = static_cast<uint8_t>(sample & 0xff);
    pcm[index * 2 + 1] = static_cast<uint8_t>((static_cast<uint16_t>(sample) >> 8) & 0xff);
  }

  return pcm;
}

#ifdef FOUNDRY_SAMPLE_HAS_PORTAUDIO
class PortAudio final {
 public:
  PortAudio() {
    if (Pa_Initialize() != paNoError) {
      throw std::runtime_error("PortAudio initialization failed");
    }
  }

  ~PortAudio() {
    Pa_Terminate();
  }
};

void StreamMicrophone(foundry_local::ItemQueue& input, std::deque<std::vector<uint8_t>>& retained) {
  PortAudio port_audio;
  PaStream* stream = nullptr;
  auto status = Pa_OpenDefaultStream(&stream, 1, 0, paInt16, kSampleRate, kFramesPerChunk, nullptr, nullptr);
  if (status != paNoError) {
    throw std::runtime_error(Pa_GetErrorText(status));
  }

  status = Pa_StartStream(stream);
  if (status != paNoError) {
    Pa_CloseStream(stream);
    throw std::runtime_error(Pa_GetErrorText(status));
  }

  std::cout << "Listening for five seconds...\n";
  for (int chunk = 0; chunk < 50; ++chunk) {
    retained.emplace_back(kFramesPerChunk * sizeof(int16_t));
    status = Pa_ReadStream(stream, retained.back().data(), kFramesPerChunk);
    if (status != paNoError && status != paInputOverflowed) {
      break;
    }
    input.Push(foundry_local::Item::Bytes(
        FOUNDRY_LOCAL_ITEM_BYTES, retained.back().data(), retained.back().size()));
  }

  Pa_StopStream(stream);
  Pa_CloseStream(stream);
}
#endif

void StreamSynthetic(foundry_local::ItemQueue& input, std::deque<std::vector<uint8_t>>& retained) {
  const auto pcm = MakeSyntheticAudio();
  constexpr size_t kChunkBytes = kFramesPerChunk * sizeof(int16_t);
  for (size_t offset = 0; offset < pcm.size(); offset += kChunkBytes) {
    const auto end = std::min(offset + kChunkBytes, pcm.size());
    retained.emplace_back(pcm.begin() + static_cast<std::ptrdiff_t>(offset),
                          pcm.begin() + static_cast<std::ptrdiff_t>(end));
    input.Push(foundry_local::Item::Bytes(
        FOUNDRY_LOCAL_ITEM_BYTES, retained.back().data(), retained.back().size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void Transcribe(foundry_local::IModel& model, bool use_synthetic) {
  using namespace foundry_local;

  AudioSession session(model);
  session.SetStreamingCallback([](Item item) {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT) {
      std::cout << item.GetSpeechSegment().text << std::flush;
    }
    return 0;
  });

  ItemQueue audio_input;
  Request request;
  request.AddItem(Item::AudioFromData("pcm", nullptr, 0, kSampleRate, 1));
  request.AddItem(audio_input, false);

  auto response_future = std::async(std::launch::async, [&]() {
    return session.ProcessRequest(request);
  });

  std::deque<std::vector<uint8_t>> retained_chunks;
  try {
#ifdef FOUNDRY_SAMPLE_HAS_PORTAUDIO
    if (!use_synthetic) {
      StreamMicrophone(audio_input, retained_chunks);
    } else {
      StreamSynthetic(audio_input, retained_chunks);
    }
#else
    (void)use_synthetic;
    std::cout << "PortAudio was not found; using synthetic PCM.\n";
    StreamSynthetic(audio_input, retained_chunks);
#endif
    audio_input.MarkFinished();
    const auto response = response_future.get();
    std::cout << "\nFinal transcript: ";
    for (const auto& item : response.GetItems()) {
      if (item.GetType() == FOUNDRY_LOCAL_ITEM_SPEECH_RESULT) {
        std::cout << item.GetSpeechResult().text;
      }
    }
    std::cout << "\n";
  } catch (...) {
    request.Cancel();
    audio_input.MarkFinished();
    if (response_future.valid()) {
      response_future.wait();
    }
    throw;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  using namespace foundry_local;

  const bool use_synthetic = argc > 1 && std::string_view(argv[1]) == "--synthetic";
  try {
    Configuration configuration{std::string(sample::kAppName)};
    Manager manager(std::move(configuration));
    sample::RegisterExecutionProviders(manager);
    auto model = sample::GetModel(manager.GetCatalog(), "nemotron-speech-streaming-en-0.6b");
    sample::LoadedModel loaded_model(*model);
    Transcribe(*model, use_synthetic);
  } catch (const Error& error) {
    std::cerr << "Foundry Local error [" << error.Code() << "]: " << error.what() << "\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
