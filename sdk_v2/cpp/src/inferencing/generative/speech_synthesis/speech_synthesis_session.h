// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/session.h"

#include <cstddef>
#include <string>
#include <vector>

namespace fl {

class GenAIModelInstance;

/// Text to speech on an ORT GenAI speech model (model type "vibevoice"). A request holds TEXT items; each item, and
/// each line in it, is a paragraph. The response holds one AUDIO item: signed 16-bit little-endian PCM ("pcm"), mono,
/// at the model's sample rate (24 kHz for VibeVoice), with metadata "voice". With a streaming callback, each chunk
/// (a sentence; short sentences are joined) also arrives as its own AUDIO item with metadata "text". Options:
/// FOUNDRY_LOCAL_PARAM_VOICE, and FOUNDRY_LOCAL_PARAM_SPEED, which VibeVoice supports only at 1.0. ORT GenAI runs the
/// generation loop; this session splits the text, observes cancellation, and builds the audio items.
class SpeechSynthesisSession : public Session {
 public:
  /// The sample rate when genai_config.json does not set "model.sample_rate".
  static constexpr int kDefaultSampleRate = 24000;

  SpeechSynthesisSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger,
                         ITelemetry& telemetry);
  ~SpeechSynthesisSession() override;

  SpeechSynthesisSession(SpeechSynthesisSession&&) = delete;
  SpeechSynthesisSession& operator=(SpeechSynthesisSession&&) = delete;

  SessionType Type() const override;

 protected:
  void ProcessRequestImpl(const Request& request, Response& response) override;

 private:
  struct Speech {
    std::vector<float> samples;
    size_t text_tokens = 0;
    size_t speech_frames = 0;
  };

  /// The requested voice, or the model's default voice when `requested` is null or empty. Throws INVALID_ARGUMENT
  /// for a voice that the model does not have.
  const std::string& ResolveVoice(const char* requested) const;

  /// Synthesize one chunk. Returns no samples when the request is cancelled before the chunk completes.
  Speech SynthesizeChunk(const std::string& text, const std::string& voice, const Request& request);

  std::string ExecutionProvider() const override;

  GenAIModelInstance& model_;
  std::vector<std::string> voices_;  ///< From ORT GenAI, default voice first.
  int sample_rate_ = kDefaultSampleRate;
};

}  // namespace fl
