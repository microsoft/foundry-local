// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/search_options.h"
#include "inferencing/model_session_lease.h"
#include "inferencing/session/session.h"
#include "inferencing/session/session_runtime.h"
#include "logger.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

// Forward declarations — avoid pulling ort_genai.h into the header
struct OgaGenerator;
struct OgaStreamingProcessor;
struct OgaNamedTensors;
struct OgaTokenizerStream;

namespace fl {

class AudioSessionTestAccessor;
class GenAIModelInstance;
struct AudioTranscriptionRequest;
struct AudioItem;
struct ItemQueue;
struct SpeechSegmentItem;

/// Executable body of an audio transcription session.
/// Stateless — each request processes one audio file independently (no history).
/// Input: a Request with an AUDIO item (file path in uri) and optional parameters
///        (language, temperature) in request.options.
///        Alternatively, a Request with a TEXT item tagged OPENAI_JSON containing an
///        OpenAI AudioTranscriptionRequest payload.
///        Alternatively, a Request with an AUDIO item (format="pcm") + an ItemQueue for streaming PCM.
/// Output: a SpeechResultItem with the full transcribed text and per-segment detail, plus token usage stats.
///         When OPENAI_JSON input is used, output is an OPENAI_JSON-tagged TextItem with the
///         AudioTranscriptionResponse payload.
///
/// Every path builds a complete pending response before OperationContext::TrySeal(). A successful seal
/// publishes it by noexcept swap; a failed seal publishes the same generated-so-far response with FINISH_NONE
/// for legacy callers, while explicit operations erase it centrally.
class AudioRuntime : public SessionRuntime {
 public:
  AudioRuntime(const fl::Model& catalog_model, ModelSessionLease lease, ILogger& logger, ITelemetry& telemetry);
  ~AudioRuntime() noexcept override;

  SessionType Type() const override;

 private:
  friend class AudioSession;
  friend class AudioSessionTestAccessor;

  void SetSessionOptionsImpl(const KeyValuePairs& options) override;
  void ProcessRequestImpl(const OperationContext& operation, const Request& request, Response& response) override;

  /// Process a request whose first item is a TEXT item tagged OPENAI_JSON containing an
  /// OpenAI AudioTranscriptionRequest payload.
  void ProcessAudioTranscriptionJson(const OperationContext& operation, const std::string& request_json,
                                     Response& response);

  bool IsNemotronSpeechModel() const;

  void ProcessNemotronFileTranscription(const OperationContext& operation, const AudioTranscriptionRequest& req,
                                        Response& response);

  void RunNemotronDecodePass(const OperationContext& operation, std::unique_ptr<OgaNamedTensors> tensors,
                             OgaGenerator& generator, OgaTokenizerStream& tokenizer_stream, std::string& text,
                             const std::unique_ptr<CallbackHandler>& streaming_callback,
                             const std::string& response_id, int& completion_tokens) const;

  void DecodeNemotronTokens(const OperationContext& operation, OgaGenerator& generator,
                            OgaTokenizerStream& tokenizer_stream, std::string& text,
                            const std::unique_ptr<CallbackHandler>& streaming_callback,
                            const std::string& response_id, int& completion_tokens) const;

  void TryNemotronLanguageId(OgaGenerator& generator, const std::string& language) const;

  static std::vector<float> LoadPcmWavAsFloatSamples(const std::string& audio_file_path);

  /// Process a streaming audio request: an AudioItem (format descriptor) + an ItemQueue (PCM chunks).
  void ProcessStreamingAudio(const OperationContext& operation, const AudioItem& format_item, ItemQueue& queue,
                             const Request& request, Response& response);

  /// Feed float32 PCM samples to the StreamingProcessor. If a full encoder chunk is ready,
  /// set the tensors on the generator and decode tokens.
  /// IMPORTANT: DecodeTokens must drain to IsDone() before the next SetInputs() call.
  /// `segments` accumulates a SpeechSegmentItem per decoded token; the same per-token
  /// segments are also what gets pushed to the streaming callback.
  void ProcessChunk(const OperationContext& operation, OgaStreamingProcessor& processor, OgaGenerator& generator,
                    OgaTokenizerStream& tokenizer_stream, const std::vector<float>& samples,
                    std::vector<std::string>& token_texts,
                    std::vector<std::unique_ptr<SpeechSegmentItem>>& segments,
                    const std::unique_ptr<CallbackHandler>& callback,
                    int& completion_tokens);

  /// Decode all available tokens from the generator. This MUST run to completion
  /// (IsDone() == true) before the next SetInputs() call.
  void DecodeTokens(const OperationContext& operation, OgaGenerator& generator,
                    OgaTokenizerStream& tokenizer_stream,
                    std::vector<std::string>& token_texts,
                    std::vector<std::unique_ptr<SpeechSegmentItem>>& segments,
                    const std::unique_ptr<CallbackHandler>& callback,
                    int& completion_tokens);

  ILogger& logger_;
  SearchOptions session_options_;
};

/// Audio transcription session.
///
/// Stateless facade over a shared AudioRuntime — see Session. Movable (it moves a shared_ptr) and destroyed
/// without waiting; the runtime holds the ModelSessionLease that keeps the model loaded.
class AudioSession : public Session {
 public:
  /// Compatibility construction against an already-pinned model. The caller must exclude a concurrent
  /// unload until construction returns; production paths should pass a manager-acquired ModelSessionLease.
  AudioSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger, ITelemetry& telemetry)
      : AudioSession(catalog_model, ModelSessionLease::Adopt(model), logger, telemetry) {}

  /// Construction from a lease acquired atomically against unload (Session::Create).
  AudioSession(const fl::Model& catalog_model, ModelSessionLease lease, ILogger& logger, ITelemetry& telemetry)
      : Session(MakeSessionRuntime<AudioRuntime>(catalog_model, std::move(lease), logger, telemetry)) {}

  AudioSession(AudioSession&&) noexcept = default;
  AudioSession& operator=(AudioSession&&) = delete;

 private:
  friend class AudioSessionTestAccessor;

  /// Test seam: the WAV decoder is a pure function of its input and is covered directly.
  static std::vector<float> LoadPcmWavAsFloatSamples(const std::string& audio_file_path) {
    return AudioRuntime::LoadPcmWavAsFloatSamples(audio_file_path);
  }
};

}  // namespace fl
