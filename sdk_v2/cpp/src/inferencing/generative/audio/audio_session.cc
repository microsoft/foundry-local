// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/audio/audio_session.h"

#include "contracts/audio_transcriptions.h"
#include "inferencing/generative/audio/onnx_audio_generator.h"
#include "inferencing/generative/audio/pcm_utils.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/openresponses/response_converter.h"
#include "items/audio_item.h"
#include "items/bytes_item.h"
#include "items/item_queue.h"
#include "items/speech_result_item.h"
#include "items/speech_segment_item.h"
#include "items/text_item.h"
#include "model.h"
#include "util/file_uri.h"
#include "utils.h"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <ort_genai.h>
#include <unordered_map>

namespace fl {

namespace {

// Build a single SpeechSegmentItem with kind NONE wrapping the given text.
//
// TODO: emit kPartial / kFinal once we integrate a model that exposes segmentation
// hypotheses (e.g. a streaming ASR that revises in-flight transcripts before finalising).
// Today's audio models (Whisper, Nemotron streaming) only surface decoded tokens, so the
// stream has no notion of "hypothesis being revised" vs "utterance finalised" — NONE is
// the honest label.
std::unique_ptr<SpeechSegmentItem> MakeNoneSegment(std::string text) {
  auto seg = std::make_unique<SpeechSegmentItem>(FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE, std::move(text));
  seg->Finalize();
  return seg;
}

// Assemble the final SpeechResultItem from the cumulative text and the per-token segments
// accumulated during generation. `language` and `duration_ms` are intentionally left unset:
// the request-side language is just a hint, and GenAI does not report a detected source
// language or audio duration.
std::unique_ptr<SpeechResultItem> BuildSpeechResult(
    std::string text, std::vector<std::unique_ptr<SpeechSegmentItem>> segments) {
  auto result = std::make_unique<SpeechResultItem>(std::move(text));
  result->segments = std::move(segments);
  result->Finalize();
  return result;
}

// Initial capacity for the per-token accumulators. Picked empirically: a few seconds of speech
// (~10s on Whisper, ~5s on Nemotron streaming) produces under 256 tokens, so most short-form
// transcriptions avoid any reallocation. Longer transcriptions still grow geometrically.
constexpr size_t kInitialTokenCapacity = 256;
constexpr size_t kMaxWavDataBytes = 64ull * 1024ull * 1024ull;
constexpr size_t kMaxWavSamples = kMaxWavDataBytes / sizeof(float);

// Concatenate the per-token strings into a single buffer with one allocation.
std::string JoinTokens(const std::vector<std::string>& token_texts) {
  size_t total = 0;
  for (const auto& t : token_texts) {
    total += t.size();
  }
  std::string out;
  out.reserve(total);
  for (const auto& t : token_texts) {
    out.append(t);
  }
  return out;
}

const std::unordered_map<std::string, std::string>& NemotronLanguageIdMap() {
  // Language id 5 is intentionally missing because upstream Nemotron lang_id assignments skip it.
  static const std::unordered_map<std::string, std::string> kMap = {
      {"en", "0"},       {"en-us", "0"},    {"en-gb", "1"},    {"es-es", "2"},   {"es", "3"},
      {"es-us", "3"},    {"zh-cn", "4"},    {"hi", "6"},       {"hi-in", "6"},   {"ar", "7"},
      {"ar-ar", "7"},    {"fr", "8"},       {"fr-fr", "8"},    {"fr-ca", "100"}, {"de", "9"},
      {"de-de", "9"},    {"ja", "10"},      {"ja-jp", "10"},   {"ru", "11"},     {"ru-ru", "11"},
      {"pt-br", "12"},   {"pt", "13"},      {"pt-pt", "13"},   {"ko", "14"},     {"ko-kr", "14"},
      {"it", "15"},      {"it-it", "15"},   {"nl", "16"},      {"nl-nl", "16"},  {"pl", "17"},
      {"pl-pl", "17"},   {"tr", "18"},      {"tr-tr", "18"},   {"uk", "19"},     {"uk-ua", "19"},
      {"ro", "20"},      {"ro-ro", "20"},   {"el", "21"},      {"el-gr", "21"},  {"cs", "22"},
      {"cs-cz", "22"},   {"hu", "23"},      {"hu-hu", "23"},   {"sv", "24"},     {"sv-se", "24"},
      {"da", "25"},      {"da-dk", "25"},   {"fi", "26"},      {"fi-fi", "26"},  {"sk", "28"},
      {"sk-sk", "28"},   {"hr", "29"},      {"hr-hr", "29"},   {"bg", "30"},     {"bg-bg", "30"},
      {"lt", "31"},      {"lt-lt", "31"},   {"th", "32"},      {"th-th", "32"},  {"vi", "33"},
      {"vi-vn", "33"},   {"et", "60"},      {"et-ee", "60"},   {"lv", "61"},     {"lv-lv", "61"},
      {"sl", "62"},      {"sl-si", "62"},   {"he", "64"},      {"he-il", "64"},  {"auto", "101"},
      {"mt", "102"},     {"mt-mt", "102"},  {"nb", "103"},     {"nb-no", "103"}, {"nn", "104"},
      {"nn-no", "104"},
  };
  return kMap;
}

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool IsLanguageToken(const std::string& token) {
  size_t start = token.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return false;
  }

  size_t end = token.find_last_not_of(" \t\r\n");
  return end >= start && token[start] == '<' && token[end] == '>';
}

}  // namespace


AudioSession::AudioSession(const fl::Model& catalog_model, GenAIModelInstance& model,
                           ILogger& logger, ITelemetry& telemetry)
    : Session(catalog_model, logger, telemetry), logger_(logger), model_(model) {
  logger_.Log(LogLevel::Debug, fmt::format("Creating AudioSession for model: {}", model.ModelId()));
  // Last so a throw above does not leak a refcount; nothing below can throw.
  model_.AcquireSession();
}

AudioSession::~AudioSession() {
  if (owns_session_) {
    model_.ReleaseSession();
  }
}

AudioSession::AudioSession(AudioSession&& other) noexcept
    : Session(std::move(other)),
      logger_(other.logger_),
      model_(other.model_),
      owns_session_(other.owns_session_),
      session_options_(std::move(other.session_options_)) {
  other.owns_session_ = false;
}

SessionType AudioSession::Type() const {
  return SessionType::kAudio;
}

void AudioSession::SetSessionOptionsImpl(const KeyValuePairs& options) {
  session_options_ = SearchOptions::FromParameters(options);
}

void AudioSession::ProcessRequestImpl(const Request& request, Response& response) {
  // OpenAI audio transcription JSON pass-through: a TEXT item tagged OPENAI_JSON.
  for (const auto* item : request.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
      const auto& text_item = static_cast<const TextItem&>(*item);

      if (text_item.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
        ProcessAudioTranscriptionJson(text_item.text, request, response);
        return;
      }
    }
  }

  // Valid inputs: 1 item (AudioItem) or 2 items (AudioItem + ItemQueue).
  const size_t n = request.items.size();

  if (n == 0 || n > 2) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     fmt::format("Audio request expects 1 or 2 items, got {}", n));
  }

  if (request.items[0]->type != FOUNDRY_LOCAL_ITEM_AUDIO) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     fmt::format("First item must be AUDIO, got type {}",
                                 static_cast<int>(request.items[0]->type)));
  }

  const auto& audio_item = static_cast<const AudioItem&>(*request.items[0]);

  // 2 items → AudioItem + ItemQueue (streaming PCM)
  if (n == 2) {
    if (request.items[1]->type != FOUNDRY_LOCAL_ITEM_QUEUE) {
      FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                       fmt::format("Second item must be QUEUE, got type {}",
                                   static_cast<int>(request.items[1]->type)));
    }

    auto& queue = static_cast<ItemQueue&>(*request.items[1]);
    ProcessStreamingAudio(audio_item, queue, request, response);
    return;
  }

  // 1 item → AudioItem with file path or inline data
  bool has_uri = !audio_item.uri.empty();
  bool has_data = audio_item.data != nullptr && audio_item.data_size > 0;

  if (!has_uri && !has_data) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     "AUDIO item must have a uri (file path) or inline data");
  }

  // Merge session-level and per-request options once for this turn (parity with ChatSession:
  // per-request keys overlay session defaults rather than replacing the entire option set).
  auto effective_kvp = MergedOptions(request.options);
  SearchOptions options = SearchOptions::FromParameters(effective_kvp);

  std::optional<float> temperature = options.temperature;

  // Language is audio-specific prompt configuration, not a generation parameter.
  // Extract directly from request options (session-level language set via base SessionOptions).
  std::string language;
  auto lang_it = request.options.find("language");
  if (lang_it != request.options.end()) {
    language = lang_it->second;
  } else {
    // Fall back to session-level option
    auto session_lang_it = SessionOptions().find("language");
    if (session_lang_it != SessionOptions().end()) {
      language = session_lang_it->second;
    }
  }

  // Create the audio generator
  // TODO: support inline data (has_data path) — requires OnnxAudioGenerator overload for byte buffers
  if (!has_uri) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED,
                     "Inline audio data without streaming (ItemQueue) is not yet supported. "
                     "Provide a file path via uri, or use AudioItem + ItemQueue for streaming.");
  }

  // Strip optional `file://` scheme prefix and percent-decode so URIs like
  // `file:///C:/My%20Audio.wav` work the same as a plain path.
  std::string audio_path = PathFromFileUri(audio_item.uri);

  auto generator = OnnxAudioGenerator::Create(audio_path, temperature, Model(), language);
  int prompt_tokens = generator->PromptTokenCount();

  // Token-by-token generation with optional streaming.
  // Check request.canceled each iteration — a streaming callback returning
  // non-zero sets this flag asynchronously via CallbackHandler.
  std::vector<std::string> token_texts;
  token_texts.reserve(kInitialTokenCapacity);
  auto streaming_callback = CreateCallbackHandler(request);
  std::vector<std::unique_ptr<SpeechSegmentItem>> segments;
  segments.reserve(kInitialTokenCapacity);

  while (!generator->IsDone() && !request.canceled) {
    generator->GenerateNextToken();
    std::string token = generator->Decode();

    if (!token.empty()) {
      segments.push_back(MakeNoneSegment(token));

      if (streaming_callback) {
        streaming_callback->PushItem(MakeNoneSegment(token));
      }

      token_texts.push_back(std::move(token));
    }

    if (request.canceled) {
      generator->Cancel();
    }
  }

  int total_tokens = generator->TokenCount();
  int completion_tokens = total_tokens - prompt_tokens;

  std::string text = JoinTokens(token_texts);

  response.items.push_back(BuildSpeechResult(std::move(text), std::move(segments)));

  // Set finish reason
  if (request.canceled) {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
  } else {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
  }

  // Set token usage
  response.usage.prompt_tokens = prompt_tokens;
  response.usage.completion_tokens = completion_tokens;
  response.usage.total_tokens = total_tokens;

  logger_.Log(LogLevel::Verbose,
              fmt::format("Completion stats: Total Tokens: {}, Prompt Tokens: {}, Completion Tokens: {}",
                          total_tokens, prompt_tokens, completion_tokens));
}

// ---------------------------------------------------------------------------
// Streaming audio path
// ---------------------------------------------------------------------------

void AudioSession::ProcessStreamingAudio(const AudioItem& format_item, ItemQueue& queue,
                                         const Request& request, Response& response) {
  // 1. Validate format
  if (format_item.format != "pcm") {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     fmt::format("Streaming audio requires format 'pcm', got '{}'", format_item.format));
  }

  if (format_item.sample_rate != 0 && format_item.sample_rate != 16000) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     fmt::format("Streaming audio requires 16000 Hz sample rate, got {}",
                                 format_item.sample_rate));
  }

  if (format_item.channels != 0 && format_item.channels != 1) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     fmt::format("Streaming audio requires mono (1 channel), got {}",
                                 format_item.channels));
  }

  logger_.Log(LogLevel::Debug, "Starting streaming audio transcription");

  // 2. Create ORT GenAI streaming pipeline
  auto& oga_model = Model().GetOgaModel();
  auto processor = OgaStreamingProcessor::Create(oga_model);
  auto gen_params = OgaGeneratorParams::Create(oga_model);

  // Apply temperature from session/request options. Merge so per-request keys overlay
  // session defaults (parity with ChatSession).
  auto effective_kvp = MergedOptions(request.options);
  SearchOptions options = SearchOptions::FromParameters(effective_kvp);

  if (options.temperature.has_value()) {
    gen_params->SetSearchOption("temperature", *options.temperature);
  }

  auto generator = OgaGenerator::Create(oga_model, *gen_params);
  auto tokenizer_stream = OgaTokenizerStream::Create(Model().Tokenizer().Oga());

  auto streaming_callback = CreateCallbackHandler(request);
  std::vector<std::string> token_texts;
  token_texts.reserve(kInitialTokenCapacity);
  std::vector<std::unique_ptr<SpeechSegmentItem>> segments;
  segments.reserve(kInitialTokenCapacity);
  // Streaming ASR has no text prompt (input is audio), so prompt_tokens stays 0.
  // We track every decoded token (whether it produced visible text or not) as completion_tokens.
  int completion_tokens = 0;

  // 3. If the AudioItem itself has initial data, process it first
  if (format_item.data && format_item.data_size > 0) {
    auto float_samples = ConvertS16LEToFloat(
        static_cast<const uint8_t*>(format_item.data), format_item.data_size);
    ProcessChunk(*processor, *generator, *tokenizer_stream,
                 float_samples, token_texts, segments, streaming_callback, request, completion_tokens);
  }

  // 4. Read from queue until finished or cancelled
  while (!request.canceled) {
    auto item = queue.WaitAndPop(std::chrono::milliseconds(100));

    if (!item) {
      if (queue.IsFinished()) {
        break;
      }
      continue;
    }

    if (item->type != FOUNDRY_LOCAL_ITEM_BYTES) {
      FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                       fmt::format("Streaming audio queue expects BYTES items, got item type {}",
                                   static_cast<int>(item->type)));
    }

    auto& bytes = static_cast<BytesItem&>(*item);
    auto float_samples = ConvertS16LEToFloat(
        static_cast<const uint8_t*>(bytes.data), bytes.data_size);

    ProcessChunk(*processor, *generator, *tokenizer_stream,
                 float_samples, token_texts, segments, streaming_callback, request, completion_tokens);
  }

  // 5. Flush remaining buffered audio
  if (!request.canceled) {
    auto flush_tensors = processor->Flush();

    if (flush_tensors) {
      generator->SetInputs(*flush_tensors);
      DecodeTokens(*generator, *tokenizer_stream, token_texts, segments, streaming_callback, request,
                   completion_tokens);
    }
  }

  // 6. Produce response — SpeechResultItem carrying all per-token segments.
  std::string full_text = JoinTokens(token_texts);
  const size_t full_text_size = full_text.size();
  response.items.push_back(BuildSpeechResult(std::move(full_text), std::move(segments)));

  if (request.canceled) {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
  } else {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
  }

  response.usage.prompt_tokens = 0;
  response.usage.completion_tokens = completion_tokens;
  response.usage.total_tokens = completion_tokens;

  logger_.Log(LogLevel::Debug, fmt::format("Streaming audio transcription complete, text length: {}",
                                           response.items.empty() ? 0 : full_text_size));
}

void AudioSession::ProcessChunk(OgaStreamingProcessor& processor, OgaGenerator& generator,
                                OgaTokenizerStream& tokenizer_stream,
                                const std::vector<float>& samples,
                                std::vector<std::string>& token_texts,
                                std::vector<std::unique_ptr<SpeechSegmentItem>>& segments,
                                const std::unique_ptr<CallbackHandler>& callback,
                                const Request& request,
                                int& completion_tokens) {
  auto tensors = processor.Process(samples.data(), samples.size());

  if (tensors) {
    generator.SetInputs(*tensors);
    DecodeTokens(generator, tokenizer_stream, token_texts, segments, callback, request, completion_tokens);
  }
}

void AudioSession::DecodeTokens(OgaGenerator& generator, OgaTokenizerStream& tokenizer_stream,
                                std::vector<std::string>& token_texts,
                                std::vector<std::unique_ptr<SpeechSegmentItem>>& segments,
                                const std::unique_ptr<CallbackHandler>& callback,
                                const Request& request,
                                int& completion_tokens) {
  while (!generator.IsDone() && !generator.IsSessionTerminated() && !request.canceled) {
    generator.GenerateNextToken();
    auto next_tokens = generator.GetNextTokens();

    if (next_tokens.empty()) {
      continue;
    }

    int32_t token_id = next_tokens[0];
    const char* token_text = tokenizer_stream.Decode(token_id);

    if (token_text && token_text[0] != '\0') {
      ++completion_tokens;
      segments.push_back(MakeNoneSegment(token_text));

      if (callback) {
        callback->PushItem(MakeNoneSegment(token_text));
      }

      token_texts.emplace_back(token_text);
    }
  }
}

void AudioSession::ProcessAudioTranscriptionJson(const std::string& request_json,
                                                 const Request& original_request,
                                                 Response& response) {
  // Parse the OpenAI audio transcription request
  auto req_json = nlohmann::json::parse(request_json);
  auto req = req_json.get<AudioTranscriptionRequest>();

  // Validate required fields
  if (req.filename.empty()) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Missing required field: filename");
  }

  // Validate file exists
  namespace fs = std::filesystem;
  if (!fs::exists(req.filename)) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, fmt::format("Audio file not found: '{}'", req.filename));
  }

  // Nemotron speech models are RNNT-based and do not use the Whisper-oriented OnnxAudioGenerator path below.
  // Route file transcription through StreamingProcessor so nemotron_speech models can decode correctly.
  if (IsNemotronSpeechModel()) {
    ProcessNemotronFileTranscription(req, original_request, response);
    return;
  }

  // Build generation options from session defaults
  SearchOptions options = session_options_;
  std::optional<float> temperature = req.temperature.has_value() ? req.temperature : options.temperature;

  // Language from request, falling back to session-level option
  std::string language;
  if (req.language.has_value()) {
    language = *req.language;
  } else {
    auto session_lang_it = SessionOptions().find("language");
    if (session_lang_it != SessionOptions().end()) {
      language = session_lang_it->second;
    }
  }

  // Create the audio generator
  auto generator = OnnxAudioGenerator::Create(req.filename, temperature, Model(), language);
  int prompt_tokens = generator->PromptTokenCount();

  auto streaming_callback = CreateCallbackHandler(original_request);
  bool is_streaming = (streaming_callback != nullptr);
  std::string response_id = ResponseConverter::GenerateId("audio");

  // Generate token-by-token
  std::string text;
  while (!generator->IsDone() && !original_request.canceled) {
    generator->GenerateNextToken();
    std::string token = generator->Decode();

    if (!token.empty()) {
      text += token;

      if (is_streaming) {
        // Emit streaming chunk as an OPENAI_JSON-tagged TextItem wrapping AudioTranscriptionResponse.
        AudioTranscriptionResponse chunk;
        chunk.id = response_id;
        chunk.text = token;
        streaming_callback->PushItem(std::make_unique<TextItem>(nlohmann::json(chunk).dump(),
                                                                FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
      }
    }

    if (original_request.canceled) {
      generator->Cancel();
    }
  }

  int total_tokens = generator->TokenCount();
  int completion_tokens = total_tokens - prompt_tokens;

  // Set finish reason
  if (original_request.canceled) {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
  } else {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
  }

  // Set token usage
  response.usage.prompt_tokens = prompt_tokens;
  response.usage.completion_tokens = completion_tokens;
  response.usage.total_tokens = total_tokens;

  // Build AudioTranscriptionResponse and wrap as an OPENAI_JSON-tagged TextItem.
  AudioTranscriptionResponse output;
  output.id = response_id;
  output.text = std::move(text);
  response.items.push_back(std::make_unique<TextItem>(nlohmann::json(output).dump(),
                                                      FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  logger_.Log(LogLevel::Verbose,
              fmt::format("Audio transcription stats: Total Tokens: {}, Prompt Tokens: {}, Completion Tokens: {}",
                          total_tokens, prompt_tokens, completion_tokens));
}


bool AudioSession::IsNemotronSpeechModel() const {
  const auto& cfg = Model().GetGenAIConfig();
  return cfg.model.has_value() && cfg.model->type == "nemotron_speech";
}

void AudioSession::TryNemotronLanguageId(OgaGenerator& generator, const std::string& language) const {
  if (language.empty()) {
    return;
  }

  const auto key = ToLowerAscii(language);
  auto it = NemotronLanguageIdMap().find(key);
  if (it == NemotronLanguageIdMap().end()) {
    return;
  }

  try {
    generator.SetRuntimeOption("lang_id", it->second.c_str());
  } catch (const std::exception& e) {
    logger_.Log(LogLevel::Warning,
                fmt::format("Failed to set Nemotron lang_id '{}' for '{}': {}",
                            it->second, language, e.what()));
  }
}

void AudioSession::DecodeNemotronTokens(OgaGenerator& generator, OgaTokenizerStream& tokenizer_stream, std::string& text,
                                        const std::unique_ptr<CallbackHandler>& streaming_callback,
                                        const std::string& response_id, const Request& original_request,
                                        int& completion_tokens) const {
  const bool is_streaming = (streaming_callback != nullptr);

  while (!generator.IsDone() && !generator.IsSessionTerminated() && !original_request.canceled) {
    generator.GenerateNextToken();
    auto next_tokens = generator.GetNextTokens();
    if (next_tokens.empty()) {
      continue;
    }

    ++completion_tokens;
    const char* decoded = tokenizer_stream.Decode(next_tokens[0]);
    if (!decoded || decoded[0] == '\0') {
      continue;
    }

    std::string token(decoded);
    if (IsLanguageToken(token)) {
      continue;
    }

    text += token;

    if (is_streaming) {
      AudioTranscriptionResponse chunk;
      chunk.id = response_id;
      chunk.text = token;
      streaming_callback->PushItem(std::make_unique<TextItem>(nlohmann::json(chunk).dump(),
                                                              FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    }
  }
}

void AudioSession::RunNemotronDecodePass(std::unique_ptr<OgaNamedTensors> tensors, OgaGenerator& generator,
                                         OgaTokenizerStream& tokenizer_stream, std::string& text,
                                         const std::unique_ptr<CallbackHandler>& streaming_callback,
                                         const std::string& response_id, const Request& original_request,
                                         int& completion_tokens) const {
  if (!tensors || original_request.canceled) {
    return;
  }

  generator.SetInputs(*tensors);
  DecodeNemotronTokens(generator, tokenizer_stream, text, streaming_callback, response_id, original_request,
                       completion_tokens);
}

void AudioSession::ProcessNemotronFileTranscription(const AudioTranscriptionRequest& req,
                                                    const Request& original_request,
                                                    Response& response) {
  if (original_request.canceled) {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
    return;
  }

  std::optional<float> temperature = req.temperature.has_value() ? req.temperature : session_options_.temperature;

  std::string language;
  if (req.language.has_value()) {
    language = *req.language;
  } else {
    auto session_lang_it = SessionOptions().find("language");
    if (session_lang_it != SessionOptions().end()) {
      language = session_lang_it->second;
    }
  }

  auto samples = LoadPcmWavAsFloatSamples(req.filename);
  auto& oga_model = Model().GetOgaModel();
  auto processor = OgaStreamingProcessor::Create(oga_model);
  auto tokenizer = OgaTokenizer::Create(oga_model);
  auto tokenizer_stream = OgaTokenizerStream::Create(*tokenizer);
  auto generator_params = OgaGeneratorParams::Create(oga_model);
  if (temperature.has_value()) {
    generator_params->SetSearchOption("temperature", *temperature);
  }
  auto generator = OgaGenerator::Create(oga_model, *generator_params);
  TryNemotronLanguageId(*generator, language);

  auto streaming_callback = CreateCallbackHandler(original_request);
  std::string response_id = ResponseConverter::GenerateId("audio");

  std::string text;
  text.reserve(512);
  int completion_tokens = 0;

  constexpr size_t kNemotronSamplesPerChunk = 1600;  // 100ms at 16kHz
  for (size_t offset = 0; offset < samples.size() && !original_request.canceled;
       offset += kNemotronSamplesPerChunk) {
    size_t count = std::min(kNemotronSamplesPerChunk, samples.size() - offset);
    RunNemotronDecodePass(processor->Process(samples.data() + offset, count), *generator, *tokenizer_stream, text,
                          streaming_callback, response_id, original_request, completion_tokens);
  }
  if (!original_request.canceled) {
    RunNemotronDecodePass(processor->Flush(), *generator, *tokenizer_stream, text, streaming_callback, response_id,
                          original_request, completion_tokens);
  }

  response.finish_reason = original_request.canceled ? FOUNDRY_LOCAL_FINISH_NONE : FOUNDRY_LOCAL_FINISH_STOP;
  // Nemotron file-transcription path feeds audio tensors directly and does not expose prompt token accounting.
  response.usage.prompt_tokens = 0;
  response.usage.completion_tokens = completion_tokens;
  response.usage.total_tokens = completion_tokens;

  AudioTranscriptionResponse output;
  output.id = response_id;
  output.text = std::move(text);
  response.items.push_back(std::make_unique<TextItem>(nlohmann::json(output).dump(),
                                                      FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
}

std::vector<float> AudioSession::LoadPcmWavAsFloatSamples(const std::string& audio_file_path) {
  std::ifstream in(audio_file_path, std::ios::binary);
  if (!in) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             fmt::format("Failed to open audio file: '{}'", audio_file_path));
  }

  in.seekg(0, std::ios::end);
  std::streamoff file_size = in.tellg();
  in.seekg(0, std::ios::beg);
  if (file_size < 12) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Invalid WAV file: too small for RIFF/WAVE header.");
  }

  char riff[4];
  uint32_t riff_size = 0;
  char wave[4];
  in.read(riff, sizeof(riff));
  in.read(reinterpret_cast<char*>(&riff_size), sizeof(riff_size));
  in.read(wave, sizeof(wave));

  if (!in || std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Invalid WAV file: missing RIFF/WAVE header.");
  }

  int16_t audio_format = 0;
  int16_t channels = 0;
  int32_t sample_rate = 0;
  int16_t bits_per_sample = 0;
  std::vector<uint8_t> data;

  while (in && in.tellg() < file_size) {
    char chunk_id_chars[4];
    uint32_t chunk_size = 0;
    in.read(chunk_id_chars, sizeof(chunk_id_chars));
    in.read(reinterpret_cast<char*>(&chunk_size), sizeof(chunk_size));
    if (!in) {
      break;
    }

    const std::string chunk_id(chunk_id_chars, sizeof(chunk_id_chars));
    const std::streamoff chunk_start = in.tellg();
    if (chunk_start < 0 || chunk_start + static_cast<std::streamoff>(chunk_size) > file_size) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Corrupted WAV chunk.");
    }

    if (chunk_id == "fmt ") {
      if (chunk_size < 16) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                 fmt::format("Invalid WAV fmt chunk size {}; expected at least 16.", chunk_size));
      }

      in.read(reinterpret_cast<char*>(&audio_format), sizeof(audio_format));
      in.read(reinterpret_cast<char*>(&channels), sizeof(channels));
      in.read(reinterpret_cast<char*>(&sample_rate), sizeof(sample_rate));

      int32_t byte_rate = 0;
      int16_t block_align = 0;
      in.read(reinterpret_cast<char*>(&byte_rate), sizeof(byte_rate));
      in.read(reinterpret_cast<char*>(&block_align), sizeof(block_align));
      in.read(reinterpret_cast<char*>(&bits_per_sample), sizeof(bits_per_sample));

      int32_t remaining = static_cast<int32_t>(chunk_size) - 16;
      if (remaining > 0) {
        in.seekg(remaining, std::ios::cur);
      }
      if (!in) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Corrupted WAV fmt chunk.");
      }

      if (channels <= 0 || sample_rate <= 0 || bits_per_sample <= 0) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Missing or invalid WAV fmt fields.");
      }
      if (sample_rate != 16000) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                 fmt::format("Expected 16kHz WAV input, got {}Hz.", sample_rate));
      }
      if ((audio_format != 1 || bits_per_sample != 16) && (audio_format != 3 || bits_per_sample != 32)) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                 fmt::format("Unsupported WAV format: audioFormat={}, bitsPerSample={}.", audio_format,
                             bits_per_sample));
      }
    } else if (chunk_id == "data") {
      if (channels <= 0 || sample_rate <= 0 || bits_per_sample <= 0) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Missing WAV fmt chunk before data.");
      }
      if (chunk_size > kMaxWavDataBytes) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                 fmt::format("WAV data chunk exceeds the maximum supported size ({} bytes).",
                             kMaxWavDataBytes));
      }

      data.resize(chunk_size);
      if (chunk_size > 0) {
        in.read(reinterpret_cast<char*>(data.data()), chunk_size);
      }
      if (!in) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Corrupted WAV data chunk.");
      }
      break;
    } else {
      in.seekg(chunk_size, std::ios::cur);
      if (!in) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Corrupted WAV chunk.");
      }
    }

    if (chunk_size % 2 == 1) {
      in.seekg(1, std::ios::cur);
      if (!in) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Corrupted WAV padding byte.");
      }
    }
  }

  if (channels <= 0 || sample_rate <= 0 || bits_per_sample <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Missing or invalid WAV fmt chunk.");
  }
  if (sample_rate != 16000) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             fmt::format("Expected 16kHz WAV input, got {}Hz.", sample_rate));
  }
  if (data.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "WAV data chunk is missing or empty.");
  }

  const size_t bytes_per_sample = static_cast<size_t>(bits_per_sample / 8);
  if ((audio_format != 1 || bits_per_sample != 16) && (audio_format != 3 || bits_per_sample != 32)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             fmt::format("Unsupported WAV format: audioFormat={}, bitsPerSample={}.", audio_format, bits_per_sample));
  }
  if (bytes_per_sample == 0 || data.size() % (bytes_per_sample * static_cast<size_t>(channels)) != 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Corrupted WAV data size.");
  }

  const size_t frame_count = data.size() / (bytes_per_sample * static_cast<size_t>(channels));
  if (frame_count > kMaxWavSamples) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             fmt::format("WAV sample count exceeds the maximum supported size ({} samples).", kMaxWavSamples));
  }

  std::vector<float> samples;
  samples.reserve(frame_count);

  if (audio_format == 1 && bits_per_sample == 16) {
    for (size_t frame = 0; frame < frame_count; ++frame) {
      float mixed = 0.0f;
      for (int16_t ch = 0; ch < channels; ++ch) {
        size_t idx = frame * static_cast<size_t>(channels) + static_cast<size_t>(ch);
        size_t byte_offset = idx * bytes_per_sample;
        int16_t sample = 0;
        std::memcpy(&sample, data.data() + byte_offset, sizeof(sample));
        mixed += static_cast<float>(sample) / 32768.0f;
      }
      samples.push_back(mixed / static_cast<float>(channels));
    }
    return samples;
  }

  for (size_t frame = 0; frame < frame_count; ++frame) {
    float mixed = 0.0f;
    for (int16_t ch = 0; ch < channels; ++ch) {
      size_t idx = frame * static_cast<size_t>(channels) + static_cast<size_t>(ch);
      float value = 0.0f;
      std::memcpy(&value, data.data() + (idx * bytes_per_sample), sizeof(float));
      mixed += value;
    }
    samples.push_back(mixed / static_cast<float>(channels));
  }

  return samples;
}

}  // namespace fl
