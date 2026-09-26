// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/audio/audio_generator.h"
#include "inferencing/generative/audio/whisper_timestamp_rules.h"
#include "inferencing/generative/genai_model_instance.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Forward declarations — avoid pulling ort_genai.h into the header
struct OgaGenerator;
struct OgaGeneratorParams;
struct OgaTokenizerStream;
struct OgaAudios;
struct OgaNamedTensors;

namespace fl {

namespace AudioInternal {

bool IsWhisperLanguageSupported(const std::string& language);
std::string BuildWhisperPrompt(const std::string& language, bool enable_timestamps);

}  // namespace AudioInternal

/// ORT GenAI implementation of the AudioGenerator interface.
/// Creates an OgaGenerator from a loaded Whisper model, processes audio input
/// through the multimodal processor, then drives token-by-token transcription
/// through the pull-based IsDone/GenerateNextToken/Decode loop.
///
/// Lifetime: one OnnxAudioGenerator per request. The GenAIModelInstance must outlive the generator.
/// OgaAudios and OgaNamedTensors are stored as members because the OgaGenerator holds
/// pointers into them — destroying them early would cause use-after-free.
class OnnxAudioGenerator : public AudioGenerator {
 public:
  ~OnnxAudioGenerator() override;

  bool IsDone() const override;
  void GenerateNextToken() override;
  std::string Decode() override;
  int TokenCount() const override;
  int PromptTokenCount() const override;
  void Cancel() override;

  /// Timestamp represented by the most recently generated token, or nullopt when it was not a timestamp.
  std::optional<int64_t> LastTimestampMilliseconds() const;

  /// Factory: create a fully configured generator for audio transcription from a file.
  ///
  /// @param audio_file_path  Path to the audio file on disk
  /// @param temperature      Generation temperature. nullopt = use model default.
  /// @param model            The loaded ORT GenAI model (must outlive the generator)
  /// @param language         Optional ISO-639-1 language code (e.g. "en"). Empty = auto-detect.
  /// @throws fl::Exception on invalid request or configuration error
  static std::unique_ptr<OnnxAudioGenerator> Create(const std::string& audio_file_path,
                                                    std::optional<float> temperature,
                                                    GenAIModelInstance& model,
                                                    const std::string& language = "");

 private:
  OnnxAudioGenerator(std::unique_ptr<OgaAudios> audios,
                     std::unique_ptr<OgaNamedTensors> inputs,
                     std::unique_ptr<OgaGeneratorParams> gen_params,
                     std::unique_ptr<OgaGenerator> generator,
                     std::unique_ptr<OgaTokenizerStream> stream,
                     int prompt_token_count,
                     std::optional<AudioInternal::WhisperTimestampTokens> timestamp_tokens,
                     std::optional<int> audio_end_timestamp_index);

  /// Mask next-token logits with Whisper's timestamp rules so the decoder emits timed segment boundaries.
  void ApplyTimestampRules();

  // Destruction is reverse-declaration order. audios_ and inputs_ are declared first
  // so they are destroyed last — the generator holds pointers into them.
  std::unique_ptr<OgaAudios> audios_;
  std::unique_ptr<OgaNamedTensors> inputs_;
  std::unique_ptr<OgaGeneratorParams> gen_params_;
  std::unique_ptr<OgaGenerator> generator_;
  std::unique_ptr<OgaTokenizerStream> stream_;
  int prompt_token_count_ = 0;
  std::optional<AudioInternal::WhisperTimestampTokens> timestamp_tokens_;
  std::optional<int> audio_end_timestamp_index_;  // audio end in 0.02s steps, capped at one window; nullopt if unknown
  std::vector<int32_t> generated_tokens_;  // tokens generated after the prompt, input to the timestamp rules
  std::optional<int32_t> last_generated_token_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace fl
