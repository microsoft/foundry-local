// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/speech_synthesis/speech_synthesis_session.h"

#include "exception.h"
#include "inferencing/execution_provider.h"
#include "inferencing/generative/audio/pcm_utils.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/speech_synthesis/speech_text.h"
#include "items/audio_item.h"
#include "items/text_item.h"
#include "util/string_utils.h"

#include <fmt/format.h>
#include <ort_genai.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace fl {

namespace {

/// Input limit of one request, in characters. The audio of a request stays in memory until it ends (about 2.9 MB per
/// minute as PCM16 plus float samples), and 16384 characters are about 15 to 20 minutes of speech.
constexpr size_t kMaxInputCharacters = 16384;

/// Chunk sizes, in characters. VibeVoice speaks a very short text (one to three words) unreliably, so short sentences
/// are joined. A long chunk delays its first audio: the CPU makes speech about three times faster than real time.
constexpr size_t kMinChunkCharacters = 24;
constexpr size_t kMaxChunkCharacters = 400;

std::unique_ptr<AudioItem> MakeAudioItem(const std::vector<float>& samples, int sample_rate) {
  auto item = std::make_unique<AudioItem>(ConvertFloatToS16LE(samples), "pcm");
  item->sample_rate = sample_rate;
  item->channels = 1;
  return item;
}

/// Stops a running generator step when the request is cancelled. The last step of a chunk runs the vocoder over all
/// frames of the chunk (up to about 3 s on a CPU), so a check between steps alone would delay a cancellation that long.
/// ORT GenAI allows "terminate_session" from another thread while a step runs; the step then fails.
class CancellationWatcher {
 public:
  CancellationWatcher(OgaGenerator& generator, const Request& request)
      : thread_([this, &generator, &request] { Watch(generator, request); }) {}

  ~CancellationWatcher() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }

    wake_.notify_one();
    thread_.join();
  }

  CancellationWatcher(const CancellationWatcher&) = delete;
  CancellationWatcher& operator=(const CancellationWatcher&) = delete;

 private:
  void Watch(OgaGenerator& generator, const Request& request) {
    constexpr auto kPollInterval = std::chrono::milliseconds(25);
    std::unique_lock<std::mutex> lock(mutex_);
    while (!wake_.wait_for(lock, kPollInterval, [this] { return stopped_; })) {
      if (request.IsCancellationRequested()) {
        try {
          generator.SetRuntimeOption("terminate_session", "1");
        } catch (const std::exception&) {
          // The step loop still sees the cancellation after the step.
        }

        return;
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopped_ = false;
  std::thread thread_;  // Last, so that the members above exist before the thread starts.
};

}  // namespace

SpeechSynthesisSession::SpeechSynthesisSession(const fl::Model& catalog_model, GenAIModelInstance& model,
                                               ILogger& logger, ITelemetry& telemetry)
    : Session(catalog_model, logger, telemetry), model_(model) {
  try {
    const auto voices = model_.GetOgaModel().GetVoices();
    for (size_t i = 0; i < voices->Count(); ++i) {
      voices_.emplace_back(voices->Get(i));
    }
  } catch (const std::runtime_error& ex) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, fmt::format("cannot read the voices of the speech model: {}", ex.what()));
  }

  const auto& config = model_.GetGenAIConfig();
  if (config.model && config.model->sample_rate > 0) {
    sample_rate_ = config.model->sample_rate;
  }

  // Last so a throw above does not leak a refcount; nothing below can throw.
  model_.AcquireSession();
}

SpeechSynthesisSession::~SpeechSynthesisSession() {
  model_.ReleaseSession();
}

SessionType SpeechSynthesisSession::Type() const {
  return SessionType::kSpeechSynthesis;
}

std::string SpeechSynthesisSession::ExecutionProvider() const {
  return std::string(EPUtils::EPtoTelemetryName(model_.EP(), model_.GetGenAIConfig().DefaultProviderView()));
}

void SpeechSynthesisSession::ProcessRequestImpl(const Request& request, Response& response) {
  if (request.items.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "speech synthesis request has no input items");
  }

  std::string input;
  for (const auto* item : request.items) {
    if (!item || item->type != FOUNDRY_LOCAL_ITEM_TEXT) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "speech synthesis expects TEXT items");
    }

    input += (input.empty() ? "" : "\n") + static_cast<const TextItem&>(*item).text;
  }

  if (!IsValidUtf8(input) || CountCodePoints(input) > kMaxInputCharacters) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             fmt::format("speech input must be valid UTF-8 of at most {} characters", kMaxInputCharacters));
  }

  const auto options = MergedOptions(request.options);
  const auto& voice = ResolveVoice(options.Find(FOUNDRY_LOCAL_PARAM_VOICE));
  if (const char* value = options.Find(FOUNDRY_LOCAL_PARAM_SPEED)) {
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || !std::isfinite(parsed) || parsed != 1.0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               fmt::format("this speech synthesis model supports only speed 1, got '{}'", value));
    }
  }

  // Each chunk is synthesized on its own, so that a long request starts streaming early and observes cancellation
  // between chunks.
  auto callback = CreateCallbackHandler(request);
  std::vector<float> samples;
  int64_t text_tokens = 0;
  int64_t speech_frames = 0;
  for (const auto& chunk : SplitSpeechText(input, kMinChunkCharacters, kMaxChunkCharacters)) {
    if (request.IsCancellationRequested()) {
      break;
    }

    const auto speech = SynthesizeChunk(chunk, voice, request);
    text_tokens += static_cast<int64_t>(speech.text_tokens);
    speech_frames += static_cast<int64_t>(speech.speech_frames);
    if (callback && !speech.samples.empty()) {
      auto item = MakeAudioItem(speech.samples, sample_rate_);
      item->GetMetadata().Add("text", chunk);
      callback->PushItem(std::move(item));
    }

    samples.insert(samples.end(), speech.samples.begin(), speech.samples.end());
  }

  response.items.push_back(MakeAudioItem(samples, sample_rate_));
  response.items.back()->GetMetadata().Add("voice", voice);
  response.usage.prompt_tokens = text_tokens;
  response.usage.completion_tokens = speech_frames;
  response.usage.total_tokens = text_tokens + speech_frames;
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
}

const std::string& SpeechSynthesisSession::ResolveVoice(const char* requested) const {
  if (voices_.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "the speech synthesis model package has no voices");
  }

  if (!requested || *requested == '\0') {
    return voices_.front();
  }

  const auto found = std::find(voices_.begin(), voices_.end(), requested);
  if (found == voices_.end()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, fmt::format("unknown voice '{}'", requested));
  }

  return *found;
}

SpeechSynthesisSession::Speech SpeechSynthesisSession::SynthesizeChunk(const std::string& text,
                                                                       const std::string& voice,
                                                                       const Request& request) {
  // The processor tokenizes literally (text cannot make special tokens) and applies the VibeVoice text prep.
  auto inputs = model_.GetPreprocessor().ProcessMedia(text.c_str(), nullptr, nullptr);
  const auto text_shape = inputs->Get(OgaInputName_TextIds)->Shape();
  Speech speech;
  speech.text_tokens = text_shape.size() == 2 ? static_cast<size_t>(text_shape[1]) : 0;
  if (speech.text_tokens == 0) {
    return speech;
  }

  auto& oga_model = model_.GetOgaModel();
  auto params = OgaGeneratorParams::Create(oga_model);
  auto generator = OgaGenerator::Create(oga_model, *params);
  // Before SetInputs: ORT GenAI checks the text against the context with the voice prompt that is set at that time.
  generator->SetRuntimeOption(OgaRuntimeOption_Voice, voice.c_str());
  try {
    generator->SetInputs(*inputs);
  } catch (const std::runtime_error& ex) {
    // ORT GenAI rejects a chunk that does not fit the TTS LM context.
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, fmt::format("speech chunk is too long: {}", ex.what()));
  }

  // One step is one speech frame; the last step also runs the vocoder.
  {
    CancellationWatcher watcher(*generator, request);
    while (!generator->IsDone()) {
      if (request.IsCancellationRequested()) {
        return {};
      }

      try {
        generator->GenerateNextToken();
      } catch (const std::runtime_error&) {
        // The watcher stopped the step.
        if (request.IsCancellationRequested()) {
          return {};
        }

        throw;
      }
    }
  }

  auto audio = generator->GetOutput(OgaOutputName_Audio);
  const auto shape = audio->Shape();
  if (audio->Type() != OgaElementType_float32 || shape.size() != 2 || shape[0] != 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "speech synthesis returned audio of an unexpected type or shape");
  }

  const auto* data = static_cast<const float*>(audio->Data());
  speech.samples.assign(data, data + shape[1]);
  speech.speech_frames = generator->TokenCount();
  return speech;
}

}  // namespace fl
