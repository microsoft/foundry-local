// Live Audio Transcription — Foundry Local C++ SDK Example
//
// Demonstrates real-time microphone-to-text using the v2 C++ SDK.
// Uses PortAudio for optional microphone capture and falls back to synthetic PCM.
//
// Usage:
//   live-audio-transcription-example
//   live-audio-transcription-example --synth

#include <foundry_local/foundry_local_cpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef HAS_PORTAUDIO
#include <portaudio.h>
#endif

using namespace foundry_local;

namespace {

std::atomic<bool> g_running{true};

void SignalHandler(int /*signum*/) {
  g_running = false;
}

class AudioQueue {
 public:
  void Push(std::vector<uint8_t> chunk) {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.size() >= kMaxSize) {
      queue_.pop_front();
    }
    queue_.push_back(std::move(chunk));
  }

  bool TryPop(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  bool Empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.empty();
  }

 private:
  static constexpr size_t kMaxSize = 100;
  mutable std::mutex mu_;
  std::deque<std::vector<uint8_t>> queue_;
};

std::vector<uint8_t> GenerateSineWavePcm(int sample_rate, int duration_seconds, double frequency_hz) {
  const auto total_samples = static_cast<size_t>(sample_rate * duration_seconds);
  std::vector<uint8_t> pcm(total_samples * 2, 0);

  for (size_t i = 0; i < total_samples; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sample_rate);
    const auto sample = static_cast<int16_t>(
        static_cast<double>(INT16_MAX) * 0.5 * std::sin(2.0 * 3.14159265358979323846 * frequency_hz * t));
    const auto encoded = static_cast<uint16_t>(sample);
    pcm[i * 2] = static_cast<uint8_t>(encoded & 0xFF);
    pcm[i * 2 + 1] = static_cast<uint8_t>((encoded >> 8) & 0xFF);
  }

  return pcm;
}

Item OwnPcmChunk(std::vector<uint8_t> chunk) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(std::move(chunk));
  return Item::Bytes(FOUNDRY_LOCAL_ITEM_BYTES, bytes->data(), bytes->size(),
                     [bytes](const flBytesData*) {});
}

#ifdef HAS_PORTAUDIO
struct Microphone {
  bool initialized = false;
  PaStream* stream = nullptr;

  void Close() {
    if (stream) {
      Pa_StopStream(stream);
      Pa_CloseStream(stream);
      stream = nullptr;
    }

    if (initialized) {
      Pa_Terminate();
      initialized = false;
    }
  }

  ~Microphone() { Close(); }
};

int PaCallback(const void* input,
               void* /*output*/,
               unsigned long frame_count,
               const PaStreamCallbackTimeInfo* /*time_info*/,
               PaStreamCallbackFlags /*status_flags*/,
               void* user_data) {
  auto* queue = static_cast<AudioQueue*>(user_data);
  const auto* pcm = static_cast<const uint8_t*>(input);
  if (!pcm) {
    return g_running ? paContinue : paComplete;
  }

  const size_t byte_count = frame_count * 2;
  std::vector<uint8_t> chunk(pcm, pcm + byte_count);
  queue->Push(std::move(chunk));
  return g_running ? paContinue : paComplete;
}
#endif

}  // namespace

int main(int argc, char* argv[]) {
  [[maybe_unused]] bool use_synth = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--synth") {
      use_synth = true;
    }
  }

  std::signal(SIGINT, SignalHandler);

  try {
    std::cout << "===========================================================\n";
    std::cout << "   Foundry Local -- Live Audio Transcription Demo (C++)\n";
    std::cout << "===========================================================\n\n";

    Configuration config("foundry_local_samples");
    Manager manager(std::move(config));

    manager.DownloadAndRegisterEps({}, [](std::string_view name, float percent) {
      std::cout << "\r  " << name << ": " << percent << "%" << std::flush;
      return g_running.load();
    });
    std::cout << "\n";

    auto& catalog = manager.GetCatalog();
    auto model = catalog.GetModel("nemotron-speech-streaming-en-0.6b");
    if (!model) {
      throw std::runtime_error("Model \"nemotron-speech-streaming-en-0.6b\" not found in catalog");
    }

    std::cout << "Using model: " << model->GetInfo().Id() << "\n";
    if (!model->IsCached()) {
      std::cout << "Downloading model...\n";
      model->Download([](float progress) -> bool {
        std::cout << "\r  " << progress << "%" << std::flush;
        return g_running.load();
      });
      std::cout << "\n";
    }

    if (!model->IsLoaded()) {
      std::cout << "Loading model...\n";
      model->Load();
    }

    {
      AudioSession session(*model);
      session.SetStreamingCallback([](Item item) -> int {
        if (item.GetType() != FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT) {
          return 0;
        }

        auto seg = item.GetSpeechSegment();
        if (seg.text.empty()) {
          return 0;
        }

        if (seg.kind == FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL) {
          std::cout << "\n  [FINAL] " << seg.text << std::endl;
        } else if (seg.kind == FOUNDRY_LOCAL_SPEECH_SEGMENT_PARTIAL) {
          std::cout << "\r" << seg.text << std::flush;
        } else {
          std::cout.write(seg.text.data(), static_cast<std::streamsize>(seg.text.size()));
          std::cout.flush();
        }
        return 0;
      });

      ItemQueue audio_input;
      Request request;
      request.AddItem(Item::AudioFromData("pcm", nullptr, 0, 16000, 1));
      request.AddItem(audio_input, false);

      Response response = [&]() -> Response {
#ifdef HAS_PORTAUDIO
        AudioQueue mic_queue;
        Microphone microphone;
        auto& pa_stream = microphone.stream;
        std::exception_ptr producer_error;
        std::jthread producer;
        bool mic_active = false;

        if (!use_synth) {
          microphone.initialized = Pa_Initialize() == paNoError;
          if (microphone.initialized) {
            PaStreamParameters input_params{};
            input_params.device = Pa_GetDefaultInputDevice();
            if (input_params.device != paNoDevice) {
              input_params.channelCount = 1;
              input_params.sampleFormat = paInt16;
              input_params.suggestedLatency = Pa_GetDeviceInfo(input_params.device)->defaultLowInputLatency;
              input_params.hostApiSpecificStreamInfo = nullptr;

              PaError err = Pa_OpenStream(
                  &pa_stream, &input_params, nullptr, 16000, 3200, paClipOff, PaCallback, &mic_queue);
              if (err == paNoError) {
                err = Pa_StartStream(pa_stream);
              }

              if (err == paNoError && pa_stream) {
                mic_active = true;
                std::cout << "===========================================================\n";
                std::cout << "  LIVE TRANSCRIPTION ACTIVE\n";
                std::cout << "  Speak into your microphone.\n";
                std::cout << "  Press Ctrl+C to stop.\n";
                std::cout << "===========================================================\n\n";

                producer = std::jthread([&](std::stop_token stop) {
                  try {
                    while (!stop.stop_requested() && (g_running.load() || !mic_queue.Empty())) {
                      std::vector<uint8_t> chunk;
                      if (mic_queue.TryPop(chunk)) {
                        audio_input.Push(OwnPcmChunk(std::move(chunk)));
                      } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                      }
                    }
                  } catch (...) {
                    producer_error = std::current_exception();
                  }
                  audio_input.MarkFinished();
                });
              } else {
                std::cerr << "Could not initialize microphone; falling back to synthetic audio.\n\n";
                if (pa_stream) {
                  Pa_CloseStream(pa_stream);
                  pa_stream = nullptr;
                }
              }
            }
          }
        }

        if (!mic_active) {
#endif
          std::cout << "Pushing synthetic audio (440Hz sine, 2s)...\n";
          std::exception_ptr synthetic_error;
          std::jthread producer([&](std::stop_token stop) {
            try {
              auto pcm = GenerateSineWavePcm(16000, 2, 440.0);
              const size_t chunk_size = static_cast<size_t>(16000 / 10 * 2);
              for (size_t offset = 0; offset < pcm.size() && !stop.stop_requested(); offset += chunk_size) {
                const size_t len = std::min(chunk_size, pcm.size() - offset);
                audio_input.Push(OwnPcmChunk(std::vector<uint8_t>(pcm.begin() + offset, pcm.begin() + offset + len)));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }
            } catch (...) {
              synthetic_error = std::current_exception();
            }
            audio_input.MarkFinished();
          });

          Response result = session.ProcessRequest(request);
          producer.join();
          if (synthetic_error) {
            std::rethrow_exception(synthetic_error);
          }

          std::cout << "✓ Synthetic audio pushed\n";
          return result;
#ifdef HAS_PORTAUDIO
        }

        Response result = session.ProcessRequest(request);
        g_running = false;
        microphone.Close();
        if (producer.joinable()) {
          producer.join();
        }
        if (producer_error) {
          std::rethrow_exception(producer_error);
        }

        return result;
#endif
      }();

      if (!response.GetItems().empty() &&
          response.GetItems().front().GetType() == FOUNDRY_LOCAL_ITEM_SPEECH_RESULT) {
        auto result = response.GetItems().front().GetSpeechResult();
        std::cout << "\n[aggregate] " << result.text << "\n";
      }
    }

    model->Unload();
    std::cout << "✓ Done\n";
    return 0;
  } catch (const Error& e) {
    std::cerr << "Foundry error: " << e.what() << " (code " << e.Code() << ")\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
