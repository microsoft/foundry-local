// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/genai_model_instance.h"
#include "items/audio_item.h"
#include "items/image_item.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward declarations — avoid pulling ort_genai.h into the header
struct OgaGenerator;
struct OgaGeneratorParams;
struct OgaTokenizerStream;
struct OgaSequences;
struct OgaNamedTensors;

namespace fl {

/// ORT GenAI implementation of the ChatGenerator interface.
/// Creates an OgaGenerator from a loaded model and a set of search options,
/// then drives token-by-token generation through the pull-based IsDone/GenerateNextToken/Decode loop.
///
/// Lifetime: one OnnxChatGenerator per request. The GenAIModelInstance must outlive the generator
/// (guaranteed by ModelLoadManager owning the model).
class OnnxChatGenerator : public ChatGenerator {
 public:
  ~OnnxChatGenerator() override;

  bool IsDone() const override;
  void GenerateNextToken() override;
  std::string Decode() override;
  int TokenCount() const override;
  int PromptTokenCount() const override;
  void Cancel() override;

  /// True when the most recently rendered prompt ends with the reasoning open
  /// marker (e.g. `<think>\n`) supplied by the chat template's generation prompt.
  /// Reasoning-model chat templates pre-fill this marker so the model's first
  /// generated token is reasoning content rather than the marker itself. The
  /// stream splitter consumes this flag to start in the reasoning state.
  bool PromptEndsInReasoning() const { return prompt_ends_in_reasoning_; }

  /// Encode new messages and append their tokens to the generator's sequence.
  /// Used for continuous decoding — only the new turn's messages are encoded and appended.
  /// Returns the number of new prompt tokens appended.
  ///
  /// @param reasoning_start Reasoning-open marker (e.g. `<think>`) used to detect
  ///        whether the chat template prefilled it as part of the assistant
  ///        generation prompt. Pass an empty string to skip the detection; the
  ///        reasoning-prefill flag is left unchanged in that case.
  int AppendMessages(const std::vector<MessageItem>& new_messages,
                     GenAIModelInstance& model,
                     const std::string& tools_json,
                     const std::string& reasoning_start = {});

  /// Recompute whether the appended prompt ends in an open reasoning block.
  /// Callers must invoke this after `AppendMessages` when they use the streaming
  /// splitter — the assistant generation prompt is re-emitted each turn and may
  /// or may not prefill the reasoning open marker depending on the template.
  void SetPromptEndsInReasoning(bool value) { prompt_ends_in_reasoning_ = value; }

  /// Rewind the generator to a previous token position.
  /// Used for error recovery — restores the KV cache to the state before the last turn.
  void RewindTo(int token_count);

  /// Factory: create a text-only chat generator.
  ///
  /// @param messages       Chat messages (system, user, assistant, etc.)
  /// @param options        Search/generation options (temperature, top_p, max_output_tokens, etc.)
  /// @param model          The loaded ORT GenAI model (not owned — must outlive the generator)
  /// @param tool_ctx       Tool calling context with tool defs, markers, and grammar flags.
  ///                       Default (empty context) means no tool calling.
  /// @param use_full_context When true, set max_length to the model's full context window.
  ///                       Used for continuous decoding with cached generators.
  /// @throws fl::Exception on invalid request or configuration error
  static std::unique_ptr<OnnxChatGenerator> Create(const std::vector<MessageItem>& messages,
                                                   const SearchOptions& options,
                                                   GenAIModelInstance& model,
                                                   const ToolCallContext& tool_ctx = {},
                                                   bool use_full_context = false);

  /// Factory: create a multimodal chat generator with image and/or audio inputs.
  static std::unique_ptr<OnnxChatGenerator> CreateWithMedia(
      const std::vector<MessageItem>& messages,
      const SearchOptions& options,
      GenAIModelInstance& model,
      const std::vector<const ImageItem*>& images,
      const std::vector<const AudioItem*>& audios,
      const ToolCallContext& tool_ctx = {},
      bool use_full_context = false);

  // ---- Static helpers exposed for unit testing ----

  /// Build the JSON messages array fed to OgaTokenizer::ApplyChatTemplate when
  /// the request includes one or more images. The last user message's content
  /// is rewritten to the structured form
  /// `[{"type":"image"},{"type":"text","text":"..."}]` so that the model's
  /// chat template inserts the appropriate vision sentinel tokens. Other
  /// messages are emitted in their plain `{"role","content"}` form.
  ///
  static std::string TransformMessagesForMedia(const std::vector<MessageItem>& messages);

 private:
  OnnxChatGenerator(std::unique_ptr<OgaGeneratorParams> gen_params,
                    std::unique_ptr<OgaGenerator> generator,
                    std::unique_ptr<OgaTokenizerStream> stream,
                    std::unique_ptr<OgaTokenizerStream> stream_with_special,
                    GenAIModelInstance& model,
                    int prompt_token_count,
                    bool prompt_ends_in_reasoning,
                    std::unique_ptr<OgaNamedTensors> named_tensors = nullptr);

  // Shared implementation for text and media creation paths. Empty image and
  // audio lists select text; either media list selects multimodal processing.
  // Both public entry points share search-options validation, guidance setup,
  // and generator construction.
  static std::unique_ptr<OnnxChatGenerator> CreateImpl(const std::vector<MessageItem>& messages,
                                                       const SearchOptions& options,
                                                       GenAIModelInstance& model,
                                                       const ToolCallContext& tool_ctx,
                                                       bool use_full_context,
                                                       const std::vector<const ImageItem*>& images,
                                                       const std::vector<const AudioItem*>& audios);

  std::unique_ptr<OgaGeneratorParams> gen_params_;
  std::unique_ptr<OgaGenerator> generator_;
  std::unique_ptr<OgaTokenizerStream> stream_;
  std::unique_ptr<OgaTokenizerStream> stream_with_special_;  // for tool call token detection
  // Holds the named tensors produced by OgaMultiModalProcessor media processing
  // for the lifetime of the generator. Generator retains shared_ptr<Tensor>
  // copies internally, but we keep the wrapper alive for symmetry with
  // upstream C# and to guarantee defensive lifetime safety.
  std::unique_ptr<OgaNamedTensors> named_tensors_;
  GenAIModelInstance& model_;  // non-owning reference — model outlives generator
  int prompt_token_count_ = 0;
  bool prompt_ends_in_reasoning_ = false;
  std::atomic<bool> cancelled_{false};
};

}  // namespace fl
