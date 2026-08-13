// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/audio/audio_generator.h"
#include "inferencing/generative/genai_model_instance.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>

// Forward declarations — avoid pulling ort_genai.h into the header
struct OgaGenerator;
struct OgaGeneratorParams;
struct OgaTokenizerStream;
struct OgaAudios;
struct OgaNamedTensors;

namespace fl {

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
  bool Cancel() noexcept override;

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
                     int prompt_token_count);

  // Destruction is reverse-declaration order. audios_ and inputs_ are declared first
  // so they are destroyed last — the generator holds pointers into them.
  std::unique_ptr<OgaAudios> audios_;
  std::unique_ptr<OgaNamedTensors> inputs_;
  std::unique_ptr<OgaGeneratorParams> gen_params_;
  std::unique_ptr<OgaGenerator> generator_;
  std::unique_ptr<OgaTokenizerStream> stream_;
  int prompt_token_count_ = 0;
  /// Numeric accounting only; generator publication and operation sealing provide lifecycle synchronization.
  std::atomic<int> token_count_{0};
  /// Local cancellation intent; not by itself evidence that the OGA session terminated.
  std::atomic<bool> cancelled_{false};

  /// Set only after SetRuntimeOption("terminate_session", ...) actually *succeeds* in Cancel(). Distinct from
  /// cancelled_: a throwing call may still have terminated the pinned OGA session, while a later unrelated
  /// runtime_error must not be suppressed without either this bit or IsSessionTerminated().
  std::atomic<bool> engine_termination_delivered_{false};
};

}  // namespace fl
