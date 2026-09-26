// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/audio/onnx_audio_generator.h"
#include "exception.h"
#include "inferencing/generative/audio/pcm_utils.h"

#include <ort_genai.h>
#include <algorithm>
#include <cmath>
#include <span>
#include <unordered_set>

namespace fl {

bool AudioInternal::IsWhisperLanguageSupported(const std::string& language) {
  static const std::unordered_set<std::string> kValidLanguages = {
      "en", "zh", "de", "es", "ru", "ko", "fr", "ja", "pt", "tr", "pl", "ca", "nl", "ar", "sv", "it", "id",
      "hi", "fi", "vi", "he", "uk", "el", "ms", "cs", "ro", "da", "hu", "ta", "no", "th", "ur", "hr", "bg",
      "lt", "la", "mi", "ml", "cy", "sk", "te", "fa", "lv", "bn", "sr", "az", "sl", "kn", "et", "mk", "br",
      "eu", "is", "hy", "ne", "mn", "bs", "kk", "sq", "sw", "gl", "mr", "pa", "si", "km", "sn", "yo", "so",
      "af", "oc", "ka", "be", "tg", "sd", "gu", "am", "yi", "lo", "uz", "fo", "ht", "ps", "tk", "nn", "mt",
      "sa", "lb", "my", "bo", "tl", "mg", "as", "tt", "haw", "ln", "ha", "ba", "jw", "su"};
  return kValidLanguages.contains(language);
}

// ---------------------------------------------------------------------------
// Whisper prompt construction
// ---------------------------------------------------------------------------

/// Build the special-token prompt that tells Whisper what task to perform.
/// Defaults to English when no language is provided or language is unrecognized.
std::string AudioInternal::BuildWhisperPrompt(const std::string& language, bool enable_timestamps) {
  // Default to English when language is empty or unrecognized
  const auto& lang = (!language.empty() && AudioInternal::IsWhisperLanguageSupported(language)) ? language : "en";

  std::string prompt = "<|startoftranscript|><|" + lang + "|><|transcribe|>";
  if (!enable_timestamps) {
    prompt += "<|notimestamps|>";
  }

  return prompt;
}

// Resolve a special token to its model-specific ID; nullopt when it does not encode to exactly one token.
static std::optional<int32_t> ResolveSingleToken(Preprocessor& preprocessor, const char* token) {
  auto sequences = preprocessor.Encode(token);
  if (sequences->Count() != 1 || sequences->SequenceCount(0) != 1) {
    return std::nullopt;
  }

  return sequences->SequenceData(0)[0];
}

static std::optional<AudioInternal::WhisperTimestampTokens> ResolveWhisperTimestampTokens(Preprocessor& preprocessor) {
  auto eot = ResolveSingleToken(preprocessor, "<|endoftext|>");
  auto no_timestamps = ResolveSingleToken(preprocessor, "<|notimestamps|>");
  auto timestamp_begin = ResolveSingleToken(preprocessor, "<|0.00|>");
  auto timestamp_end = ResolveSingleToken(preprocessor, "<|30.00|>");
  if (!eot || !no_timestamps || !timestamp_begin || !timestamp_end) {
    return std::nullopt;
  }

  AudioInternal::WhisperTimestampTokens tokens{
      .eot = *eot,
      .no_timestamps = *no_timestamps,
      .timestamp_begin = *timestamp_begin,
      .timestamp_end = *timestamp_end,
  };
  return AudioInternal::IsValidWhisperTimestampTokens(tokens) ? std::optional(tokens) : std::nullopt;
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
                                       int prompt_token_count,
                                       std::optional<AudioInternal::WhisperTimestampTokens> timestamp_tokens,
                                       std::optional<int> audio_end_timestamp_index)
    : audios_(std::move(audios)),
      inputs_(std::move(inputs)),
      gen_params_(std::move(gen_params)),
      generator_(std::move(generator)),
      stream_(std::move(stream)),
      prompt_token_count_(prompt_token_count),
      timestamp_tokens_(timestamp_tokens),
      audio_end_timestamp_index_(audio_end_timestamp_index) {}

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
  last_generated_token_.reset();
  if (cancelled_) {
    return;
  }

  try {
    if (timestamp_tokens_) {
      ApplyTimestampRules();
    }

    generator_->GenerateNextToken();

    auto next_tokens = generator_->GetNextTokens();
    if (!next_tokens.empty()) {
      last_generated_token_ = next_tokens[0];
      generated_tokens_.push_back(*last_generated_token_);
    }
  } catch (const std::runtime_error& e) {
    // If cancelled while generating, the OGA engine throws when the session is terminated.
    // This is expected — not an error.
    if (cancelled_) {
      return;
    }

    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("audio token generation failed: ") + e.what());
  }
}

void OnnxAudioGenerator::ApplyTimestampRules() {
  // GetLogits returns a CPU copy holding only the last position's logits; SetLogits copies it back to the device.
  auto logits = generator_->GetLogits();
  if (logits->Type() != OgaElementType_float32) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Whisper timestamp decoding requires float32 logits.");
  }

  auto shape = logits->Shape();
  if (shape.empty() || shape.back() <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Whisper timestamp decoding received an invalid logits shape.");
  }

  for (size_t i = 0; i + 1 < shape.size(); ++i) {
    if (shape[i] != 1) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "Whisper timestamp decoding currently supports only one logits row (batch size 1, one beam).");
    }
  }

  const auto vocab = static_cast<size_t>(shape.back());
  if (static_cast<size_t>(timestamp_tokens_->timestamp_end) >= vocab) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Whisper timestamp token IDs exceed the model vocabulary.");
  }

  auto* data = static_cast<float*>(logits->Data());
  AudioInternal::ApplyWhisperTimestampRules(std::span<float>(data, vocab), generated_tokens_, *timestamp_tokens_,
                                            AudioInternal::kWhisperMaxInitialTimestampIndex, audio_end_timestamp_index_);
  generator_->SetLogits(*logits);
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

std::optional<int64_t> OnnxAudioGenerator::LastTimestampMilliseconds() const {
  if (!timestamp_tokens_ || !last_generated_token_) {
    return std::nullopt;
  }

  return AudioInternal::WhisperTimestampMilliseconds(*last_generated_token_, *timestamp_tokens_);
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

  // 2. Resolve model-specific timestamp token IDs before building the prompt. If resolution fails, retain the previous
  //    <|notimestamps|> behavior rather than allowing the model to emit an unmasked control token into user-visible text.
  auto timestamp_tokens = ResolveWhisperTimestampTokens(model.GetPreprocessor());

  // 3. Build the Whisper prompt with optional language tag.
  //    Use the multi-prompt overload (vector) with size 1 — the single-prompt overload
  //    sets Payload::prompt but WhisperProcessor::Process reads Payload::prompts,
  //    which would be empty and cause a divide-by-zero in EncodeBatch.
  //    https://github.com/microsoft/onnxruntime-genai/issues/2067
  std::string prompt = AudioInternal::BuildWhisperPrompt(language, timestamp_tokens.has_value());
  std::vector<const char*> prompts = {prompt.c_str()};

  // 4. Process audio through the multimodal processor to get model inputs
  auto inputs = model.GetPreprocessor().ProcessAudios(prompts, audios.get());

  // 5. Create generator params and configure temperature (only override model default if explicitly set)
  auto gen_params = OgaGeneratorParams::Create(model.GetOgaModel());

  if (temperature.has_value()) {
    gen_params->SetSearchOption("temperature", *temperature);
  }

  // 6. Create the generator and feed it the processed inputs
  std::unique_ptr<OgaGenerator> generator;

  try {
    generator = OgaGenerator::Create(model.GetOgaModel(), *gen_params);
    generator->SetInputs(*inputs);
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("failed to create audio generator: ") + e.what());
  }

  // 7. Capture prompt token count after inputs are set
  int prompt_token_count = static_cast<int>(generator->GetSequenceCount(0));

  // 8. Create the tokenizer stream for ordinary text decoding. Timestamp boundaries are classified by verified token
  //    IDs rather than locale-sensitive parsing of decoded marker text.
  auto stream = model.GetPreprocessor().CreateTokenizerStream();

  // 9. Audio end in timestamp steps, capped at the single 30 s window this generator decodes. Only WAV headers are
  //    probed; for other formats the end is unknown and the timestamp rules fall back to the reference EOT behavior.
  std::optional<int> audio_end_timestamp_index;
  if (auto duration = AudioInternal::TryReadWavDurationSeconds(audio_file_path)) {
    const double steps = std::floor(*duration / AudioInternal::kWhisperTimestampStepSeconds);
    audio_end_timestamp_index = static_cast<int>(std::min(steps, double{AudioInternal::kWhisperWindowTimestampSteps}));
  }

  // `std::make_unique` cannot access the private constructor, so use `new` directly.
  return std::unique_ptr<OnnxAudioGenerator>(new OnnxAudioGenerator(std::move(audios),
                                                                    std::move(inputs),
                                                                    std::move(gen_params),
                                                                    std::move(generator),
                                                                    std::move(stream),
                                                                    prompt_token_count,
                                                                    timestamp_tokens,
                                                                    audio_end_timestamp_index));
}

}  // namespace fl
