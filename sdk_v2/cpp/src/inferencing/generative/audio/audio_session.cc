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
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <ort_genai.h>

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
  auto tokenizer_stream = OgaTokenizerStream::Create(Model().GetOgaTokenizer());

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

  // Build generation options from session defaults
  SearchOptions options = session_options_;

  std::optional<float> temperature;
  if (req.temperature.has_value()) {
    temperature = *req.temperature;
  } else {
    temperature = options.temperature;
  }

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

}  // namespace fl
