// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/audio/onnx_audio_generator.h"
#include "exception.h"

#include <ort_genai.h>
#include <unordered_set>

namespace fl {

// ---------------------------------------------------------------------------
// Whisper prompt construction
// ---------------------------------------------------------------------------

/// Build the special-token prompt that tells Whisper what task to perform.
/// Defaults to English when no language is provided or language is unrecognized.
static std::string BuildWhisperPrompt(const std::string& language) {
  // Supported Whisper language codes (ISO-639-1 and a few extended)
  static const std::unordered_set<std::string> kValidLanguages = {
      "en", "zh", "de", "es", "ru", "ko", "fr", "ja", "pt", "tr", "pl", "ca", "nl", "ar",
      "sv", "it", "id", "hi", "fi", "vi", "he", "uk", "el", "ms", "cs", "ro", "da", "hu",
      "ta", "no", "th", "ur", "hr", "bg", "lt", "la", "mi", "ml", "cy", "sk", "te", "fa",
      "lv", "bn", "sr", "az", "sl", "kn", "et", "mk", "br", "eu", "is", "hy", "ne", "mn",
      "bs", "kk", "sq", "sw", "gl", "mr", "pa", "si", "km", "sn", "yo", "so", "af", "oc",
      "ka", "be", "tg", "sd", "gu", "am", "yi", "lo", "uz", "fo", "ht", "ps", "tk", "nn",
      "mt", "sa", "lb", "my", "bo", "tl", "mg", "as", "tt", "haw", "ln", "ha", "ba", "jw", "su"};

  // Default to English when language is empty or unrecognized
  const auto& lang = (!language.empty() && kValidLanguages.contains(language)) ? language : "en";

  return "<|startoftranscript|><|" + lang + "|><|transcribe|><|notimestamps|>";
}

// Declared out-of-line so unique_ptr deleters see the complete OGA types.
// Destruction order matters: audios_ and inputs_ are declared before generator_
// so they are destroyed after it (reverse-declaration order).
OnnxAudioGenerator::~OnnxAudioGenerator() = default;

// ---------------------------------------------------------------------------
// Private constructor
// ---------------------------------------------------------------------------

OnnxAudioGenerator::OnnxAudioGenerator(std::unique_ptr<OgaAudios> audios,
                                       std::unique_ptr<OgaNamedTensors> inputs,
                                       std::unique_ptr<OgaGeneratorParams> gen_params,
                                       std::unique_ptr<OgaGenerator> generator,
                                       std::unique_ptr<OgaTokenizerStream> stream,
                                       int prompt_token_count)
    : audios_(std::move(audios)),
      inputs_(std::move(inputs)),
      gen_params_(std::move(gen_params)),
      generator_(std::move(generator)),
      stream_(std::move(stream)),
      prompt_token_count_(prompt_token_count) {}

// ---------------------------------------------------------------------------
// AudioGenerator interface
// ---------------------------------------------------------------------------

bool OnnxAudioGenerator::IsDone() const {
  if (cancelled_) {
    return true;
  }

  // OgaGenerator::IsDone() is non-const in the ORT GenAI API, so we need const_cast.
  // This is safe because IsDone only reads state.
  auto* gen = const_cast<OgaGenerator*>(generator_.get());
  return gen->IsDone() || gen->IsSessionTerminated();
}

void OnnxAudioGenerator::GenerateNextToken() {
  if (cancelled_) {
    return;
  }

  try {
    generator_->GenerateNextToken();
  } catch (const std::runtime_error& e) {
    // If cancelled while generating, the OGA engine throws when the session is terminated.
    // This is expected — not an error.
    if (cancelled_) {
      return;
    }

    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("audio token generation failed: ") + e.what());
  }
}

std::string OnnxAudioGenerator::Decode() {
  if (cancelled_) {
    return "";
  }

  auto next_tokens = generator_->GetNextTokens();

  if (next_tokens.empty()) {
    return "";
  }

  int32_t token_id = next_tokens[0];
  const char* token_text = stream_->Decode(token_id);

  return token_text ? std::string(token_text) : "";
}

int OnnxAudioGenerator::TokenCount() const {
  return static_cast<int>(generator_->GetSequenceCount(0));
}

int OnnxAudioGenerator::PromptTokenCount() const {
  return prompt_token_count_;
}

void OnnxAudioGenerator::Cancel() {
  cancelled_ = true;

  // Use the ORT GenAI engine-level termination to interrupt mid-compute
  // (e.g. during a long prefill), not just between token boundaries.
  try {
    generator_->SetRuntimeOption("terminate_session", "1");
  } catch (const std::exception&) {
    // SetRuntimeOption may not be supported by all ORT GenAI builds.
  }
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<OnnxAudioGenerator> OnnxAudioGenerator::Create(const std::string& audio_file_path,
                                                               std::optional<float> temperature,
                                                               GenAIModelInstance& model,
                                                               const std::string& language) {
  if (audio_file_path.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "audio_file_path must not be empty");
  }

  if (!model.GetPreprocessor().HasMultiModalProcessor()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model does not support audio processing");
  }

  // 1. Load audio from disk
  std::vector<const char*> paths = {audio_file_path.c_str()};
  auto audios = OgaAudios::Load(paths);

  // 2. Build the Whisper prompt with optional language tag.
  //    Use the multi-prompt overload (vector) with size 1 — the single-prompt overload
  //    sets Payload::prompt but WhisperProcessor::Process reads Payload::prompts,
  //    which would be empty and cause a divide-by-zero in EncodeBatch.
  //    https://github.com/microsoft/onnxruntime-genai/issues/2067
  std::string prompt = BuildWhisperPrompt(language);
  std::vector<const char*> prompts = {prompt.c_str()};

  // 3. Process audio through the multimodal processor to get model inputs
  auto inputs = model.GetPreprocessor().ProcessAudios(prompts, audios.get());

  // 4. Create generator params and configure temperature (only override model default if explicitly set)
  auto gen_params = OgaGeneratorParams::Create(model.GetOgaModel());

  if (temperature.has_value()) {
    gen_params->SetSearchOption("temperature", *temperature);
  }

  // 5. Create the generator and feed it the processed inputs
  std::unique_ptr<OgaGenerator> generator;

  try {
    generator = OgaGenerator::Create(model.GetOgaModel(), *gen_params);
    generator->SetInputs(*inputs);
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("failed to create audio generator: ") + e.what());
  }

  // 6. Capture prompt token count after inputs are set
  int prompt_token_count = static_cast<int>(generator->GetSequenceCount(0));

  // 7. Create tokenizer stream for decoding (no special-token stream needed for audio)
  auto stream = model.GetPreprocessor().CreateTokenizerStream();

  // `std::make_unique` cannot access the private constructor, so use `new` directly.
  return std::unique_ptr<OnnxAudioGenerator>(new OnnxAudioGenerator(std::move(audios),
                                                                    std::move(inputs),
                                                                    std::move(gen_params),
                                                                    std::move(generator),
                                                                    std::move(stream),
                                                                    prompt_token_count));
}

}  // namespace fl
