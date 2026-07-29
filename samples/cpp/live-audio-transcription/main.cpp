// Live Audio Transcription — Foundry Local C++ SDK Example
//
// Demonstrates real-time microphone-to-text using the C++ SDK.
// Uses PortAudio for cross-platform mic capture (like naudiodon2 in the JS sample).
// Falls back to synthetic PCM if PortAudio is unavailable.
//
// Requires: PortAudio (libportaudio), Foundry Local C++ SDK
//
// Usage: ./live-audio-transcription-example [--synth]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "foundry_local.h"

// PortAudio is optional — compile with -DHAS_PORTAUDIO and link -lportaudio
// to enable live microphone capture.
#ifdef HAS_PORTAUDIO
#include <portaudio.h>
#endif

namespace {

// Global flag for Ctrl+C graceful shutdown (mirrors JS process.on('SIGINT'))
std::atomic<bool> g_running{true};

void SignalHandler(int /*signum*/) {
    g_running = false;
}

// Bounded audio queue (mirrors JS appendQueue with cap of 100)
class AudioQueue {
public:
    void Push(std::vector<uint8_t> chunk) {
        std::lock_guard<std::mutex> lock(mu_);
        if (queue_.size() >= kMaxSize) {
            queue_.pop_front();
            if (!warnedDrop_) {
                warnedDrop_ = true;
                std::cerr << "Audio append queue overflow; dropping oldest chunk to keep stream alive." << std::endl;
            }
        }
        queue_.push_back(std::move(chunk));
    }

    bool TryPop(std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

private:
    static constexpr size_t kMaxSize = 100;
    std::deque<std::vector<uint8_t>> queue_;
    std::mutex mu_;
    bool warnedDrop_ = false;
};

std::vector<uint8_t> GenerateSineWavePcm(int sampleRate, int durationSeconds, double frequencyHz) {
    const auto totalSamples = static_cast<size_t>(sampleRate * durationSeconds);
    std::vector<uint8_t> pcm(totalSamples * 2, 0); // 16-bit mono, little-endian

    for (size_t i = 0; i < totalSamples; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(sampleRate);
        const auto sample = static_cast<int16_t>(
            static_cast<double>(INT16_MAX) * 0.5 * std::sin(2.0 * 3.14159265358979323846 * frequencyHz * t));
        const auto encodedSample = static_cast<uint16_t>(sample);
        pcm[i * 2] = static_cast<uint8_t>(encodedSample & 0xFF);
        pcm[i * 2 + 1] = static_cast<uint8_t>((encodedSample >> 8) & 0xFF);
    }
    return pcm;
}

#ifdef HAS_PORTAUDIO
// PortAudio callback — captures 16-bit mono PCM and pushes to the queue
int PaCallback(const void* input, void* /*output*/,
               unsigned long frameCount,
               const PaStreamCallbackTimeInfo* /*timeInfo*/,
               PaStreamCallbackFlags /*statusFlags*/,
               void* userData) {
    auto* queue = static_cast<AudioQueue*>(userData);
    const auto* pcm = static_cast<const uint8_t*>(input);
    const size_t byteCount = frameCount * 2; // 16-bit mono = 2 bytes per frame
    std::vector<uint8_t> chunk(pcm, pcm + byteCount);
    queue->Push(std::move(chunk));
    return g_running ? paContinue : paComplete;
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    bool useSynth = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--synth") useSynth = true;
    }

    // Install Ctrl+C handler (mirrors JS process.on('SIGINT'))
    std::signal(SIGINT, SignalHandler);

    try {
        std::cout << "===========================================================" << std::endl;
        std::cout << "   Foundry Local -- Live Audio Transcription Demo (C++)" << std::endl;
        std::cout << "===========================================================" << std::endl;
        std::cout << std::endl;

        foundry_local::Configuration config;
        config.appName = "foundry_local_samples";

        foundry_local::Manager::Create(config);
        auto& manager = foundry_local::Manager::Instance();
        auto isCancellationRequested = [] { return !g_running.load(); };
        manager.DownloadAndRegisterEps(nullptr, isCancellationRequested);

        auto& catalog = manager.GetCatalog();
        // English-only:
        const char* modelAlias = "nemotron-speech-streaming-en-0.6b";
        // Multi-lingual (supports 30+ languages including auto-detect):
        // const char* modelAlias = "nemotron-3.5-asr-streaming-0.6b";
        auto* model = catalog.GetModel(modelAlias);
        if (!model) {
            throw std::runtime_error(std::string("Model \"") + modelAlias + "\" not found in catalog");
        }

        std::cout << "Downloading model (if needed)..." << std::endl;
        model->Download(
            [](float pct) {
                std::cout << "\rDownloading: " << pct << "%   " << std::flush;
                return true;
            },
            isCancellationRequested);
        std::cout << std::endl;
        std::cout << "Loading model..." << std::endl;
        model->Load();
        std::cout << "Model loaded" << std::endl;

        // NOTE: CreateLiveTranscriptionSession() is not yet available in the C++ SDK.
        // The audio client and session code below is forward-looking.
        foundry_local::OpenAIAudioClient audioClient(*model);
        auto session = audioClient.CreateLiveTranscriptionSession();

        session->Settings().sample_rate = 16000;
        session->Settings().channels = 1;
        session->Settings().bits_per_sample = 16;
        session->Settings().language = "en";                  // English (default)
        // Multi-lingual examples:
        // session->Settings().language = "de";     // German
        // session->Settings().language = "zh-CN";  // Chinese (Simplified)
        // session->Settings().language = "auto";   // Auto-detect language
        session->Start();
        std::cout << "Session started" << std::endl;

        // Read transcription results in a background thread (mirrors JS readPromise)
        std::thread readThread([&session]() {
            foundry_local::LiveAudioTranscriptionResponse result;
            while (g_running) {
                const auto status = session->TryGetNext(result, std::chrono::milliseconds(500));
                if (status == foundry_local::TranscriptionStatus::Result) {
                    if (result.is_final) {
                        std::cout << "\n  [FINAL] " << result.text << std::endl;
                    } else if (!result.text.empty()) {
                        std::cout << result.text << std::flush;
                    }
                } else if (status == foundry_local::TranscriptionStatus::Closed) {
                    break;
                } else if (status == foundry_local::TranscriptionStatus::Timeout) {
                    continue;
                } else {
                    std::cerr << "Transcription stream error: " << session->GetErrorMessage() << std::endl;
                    break;
                }
            }
        });

        // --- Microphone capture (mirrors JS naudiodon2 section) ---
        // Uses PortAudio for cross-platform audio capture. If PortAudio is not
        // available or --synth is passed, falls back to synthetic PCM.

        bool micActive = false;

#ifdef HAS_PORTAUDIO
        PaStream* paStream = nullptr;
        AudioQueue audioQueue;

        if (!useSynth) {
            PaError err = Pa_Initialize();
            if (err == paNoError) {
                PaStreamParameters inputParams{};
                inputParams.device = Pa_GetDefaultInputDevice();
                if (inputParams.device != paNoDevice) {
                    inputParams.channelCount = 1;
                    inputParams.sampleFormat = paInt16;
                    inputParams.suggestedLatency =
                        Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
                    inputParams.hostApiSpecificStreamInfo = nullptr;

                    // framesPerBuffer=3200 matches JS framesPerBuffer setting
                    err = Pa_OpenStream(&paStream, &inputParams, nullptr,
                                        16000, 3200, paClipOff,
                                        PaCallback, &audioQueue);
                    if (err == paNoError) {
                        err = Pa_StartStream(paStream);
                    }
                }

                if (err == paNoError && paStream) {
                    micActive = true;
                    std::cout << std::endl;
                    std::cout << "===========================================================" << std::endl;
                    std::cout << "  LIVE TRANSCRIPTION ACTIVE" << std::endl;
                    std::cout << "  Speak into your microphone." << std::endl;
                    std::cout << "  Press Ctrl+C to stop." << std::endl;
                    std::cout << "===========================================================" << std::endl;
                    std::cout << std::endl;

                    // Pump audio from the queue to the session (mirrors JS pumpAudio)
                    while (g_running) {
                        std::vector<uint8_t> chunk;
                        if (audioQueue.TryPop(chunk)) {
                            session->Append(chunk.data(), chunk.size());
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    }

                    Pa_StopStream(paStream);
                    Pa_CloseStream(paStream);
                } else {
                    std::cerr << "Could not initialize microphone: "
                              << Pa_GetErrorText(err) << std::endl;
                    std::cerr << "Falling back to synthetic audio test..." << std::endl;
                    std::cerr << std::endl;
                }
                Pa_Terminate();
            }
        }
#endif

        // Fallback: push synthetic PCM (440Hz sine wave) — mirrors JS catch block
        if (!micActive) {
            std::cout << "Pushing synthetic audio (440Hz sine, 2s)..." << std::endl;
            const auto pcm = GenerateSineWavePcm(16000, 2, 440.0);
            const size_t chunkSize = static_cast<size_t>(16000 / 10 * 2); // 100ms
            for (size_t offset = 0; offset < pcm.size() && g_running; offset += chunkSize) {
                const size_t len = std::min(chunkSize, pcm.size() - offset);
                session->Append(pcm.data() + offset, len);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::cout << "Synthetic audio pushed" << std::endl;

            // Wait briefly for remaining transcription results
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        // Graceful shutdown (mirrors JS SIGINT handler)
        std::cout << "\n\nStopping..." << std::endl;
        session->Stop();
        readThread.join();
        model->Unload();
        foundry_local::Manager::Destroy();
        std::cout << "Done" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
        foundry_local::Manager::Destroy();
        return 1;
    }
}
