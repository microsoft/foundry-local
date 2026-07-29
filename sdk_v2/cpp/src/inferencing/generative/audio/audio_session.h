// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/search_options.h"
#include "inferencing/session/session.h"
#include "logger.h"

#include <memory>
#include <string>
#include <vector>

// Forward declarations — avoid pulling ort_genai.h into the header
struct OgaGenerator;
struct OgaStreamingProcessor;
struct OgaNamedTensors;
struct OgaTokenizerStream;

namespace fl {

class GenAIModelInstance;
struct AudioItem;
struct ItemQueue;
struct SpeechSegmentItem;

/// Audio transcription session.
/// Stateless — each request processes one audio file independently (no history).
/// Input: a Request with an AUDIO item (file path in uri) and optional parameters
///        (language, temperature) in request.options.
///        Alternatively, a Request with a TEXT item tagged OPENAI_JSON containing an
///        OpenAI AudioTranscriptionRequest payload.
///        Alternatively, a Request with an AUDIO item (format="pcm") + an ItemQueue for streaming PCM.
/// Output: a SpeechResultItem with the full transcribed text and per-segment detail, plus token usage stats.
///         When OPENAI_JSON input is used, output is an OPENAI_JSON-tagged TextItem with the
///         AudioTranscriptionResponse payload.
class AudioSession : public Session {
 public:
  AudioSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger, ITelemetry& telemetry);
  ~AudioSession();

  // Movable: transfers session refcount ownership to the moved-to instance.
  AudioSession(AudioSession&& other) noexcept;
  AudioSession& operator=(AudioSession&&) = delete;

  SessionType Type() const override;

 private:
  void SetSessionOptionsImpl(const KeyValuePairs& options) override;
  void ProcessRequestImpl(const Request& request, Response& response) override;

  /// Process a request whose first item is a TEXT item tagged OPENAI_JSON containing an
  /// OpenAI AudioTranscriptionRequest payload.
  void ProcessAudioTranscriptionJson(const std::string& request_json, const Request& original_request,
                                     Response& response);

  /// Process a streaming audio request: an AudioItem (format descriptor) + an ItemQueue (PCM chunks).
  void ProcessStreamingAudio(const AudioItem& format_item, ItemQueue& queue,
                             const Request& request, Response& response);

  /// Feed float32 PCM samples to the StreamingProcessor. If a full encoder chunk is ready,
  /// set the tensors on the generator and decode tokens.
  /// IMPORTANT: DecodeTokens must drain to IsDone() before the next SetInputs() call.
  /// `segments` accumulates a SpeechSegmentItem per decoded token; the same per-token
  /// segments are also what gets pushed to the streaming callback.
  void ProcessChunk(OgaStreamingProcessor& processor, OgaGenerator& generator,
                    OgaTokenizerStream& tokenizer_stream, const std::vector<float>& samples,
                    std::vector<std::string>& token_texts,
                    std::vector<std::unique_ptr<SpeechSegmentItem>>& segments,
                    const std::unique_ptr<CallbackHandler>& callback,
                    const Request& request,
                    int& completion_tokens);

  /// Decode all available tokens from the generator. This MUST run to completion
  /// (IsDone() == true) before the next SetInputs() call.
  void DecodeTokens(OgaGenerator& generator, OgaTokenizerStream& tokenizer_stream,
                    std::vector<std::string>& token_texts,
                    std::vector<std::unique_ptr<SpeechSegmentItem>>& segments,
                    const std::unique_ptr<CallbackHandler>& callback,
                    const Request& request,
                    int& completion_tokens);

  GenAIModelInstance& Model() { return model_; }
  const GenAIModelInstance& Model() const { return model_; }

  ILogger& logger_;
  GenAIModelInstance& model_;
  // Tracks who is responsible for calling model_.ReleaseSession(). Set to false on the
  // moved-from instance so the refcount transfers cleanly across moves.
  bool owns_session_ = true;
  SearchOptions session_options_;
};

}  // namespace fl
