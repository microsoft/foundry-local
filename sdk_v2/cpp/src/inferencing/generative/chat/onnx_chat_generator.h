// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/prepared_chat_prompt.h"
#include "inferencing/generative/chat/reasoning_stream_splitter.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/genai_model_instance.h"
#include "items/audio_item.h"
#include "items/image_item.h"

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
struct OgaSequences;
struct OgaNamedTensors;

namespace fl {

/// Resolve the reasoning boundary markers for a request: request/catalog overrides first, then the markers the loaded
/// GenAI model publishes. Single source of truth for both the prompt-state probe and the generation-time splitter.
///
/// The token IDs always describe the marker strings that are actually in effect. A model's published marker ID is
/// reused only when it decodes to exactly that string; an overridden marker is encoded with the model's tokenizer
/// instead, so token-aware matching can never flip the reasoning state on a token that is not the boundary.
ReasoningMarkers ResolveReasoningMarkers(const ToolCallContext& tool_ctx, GenAIModelInstance& model);

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
  std::optional<int32_t> CurrentTokenId() const override;
  int TokenCount() const override;
  int PromptTokenCount() const override;
  void Cancel() override;

  /// Encode new messages and append their tokens to the generator's sequence.
  /// Used for continuous decoding — only the new turn's messages are encoded and appended.
  /// Returns the number of new prompt tokens appended.
  int AppendMessages(const std::vector<TranscriptMessage>& new_messages,
                     const std::vector<TranscriptMessage>& full_messages,
                     GenAIModelInstance& model,
                     const ToolCallContext& tool_ctx,
                     const SearchOptions& options) override;
  int AppendPreparedPrompt(const std::vector<TranscriptMessage>& new_messages,
                           const PreparedChatPrompt& prepared,
                           GenAIModelInstance& model,
                           const ToolCallContext& tool_ctx,
                           const SearchOptions& options) override;

  /// Rewind the generator to a previous token position.
  /// Used for error recovery — restores the KV cache to the state before the last turn.
  bool CanRewind() const override { return true; }
  void RewindTo(int token_count) override;

  /// True when the most recent prompt fed to the generator ends inside an open reasoning block, because the model's
  /// chat template emitted the opening marker itself. Callers seed their reasoning splitter with this so generation
  /// that only ever emits the closing marker is still classified as reasoning.
  bool PromptOpensReasoning() const override { return prompt_opens_reasoning_; }

  /// Factory: create a text-only chat generator.
  ///
  /// @param messages       Transcript messages for the whole conversation
  /// @param options        Search/generation options (temperature, top_p, max_output_tokens, etc.)
  /// @param model          The loaded ORT GenAI model (not owned — must outlive the generator)
  /// @param tool_ctx       Tool calling context with tool defs, markers, and grammar flags.
  ///                       Default (empty context) means no tool calling.
  /// @param use_full_context When true, set max_length to the model's full context window.
  ///                       Used for continuous decoding with cached generators.
  /// @throws fl::Exception on invalid request or configuration error
  static std::unique_ptr<OnnxChatGenerator> Create(const std::vector<TranscriptMessage>& messages,
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

  /// Create a generator from the exact artifact produced by request preflight preparation.
  static std::unique_ptr<OnnxChatGenerator> CreatePrepared(PreparedChatPrompt prepared,
                                                           const SearchOptions& options,
                                                           GenAIModelInstance& model,
                                                           const ToolCallContext& tool_ctx,
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
                    GenAIModelInstance& model,
                    int prompt_token_count,
                    ReasoningMarkers reasoning_markers,
                    bool prompt_opens_reasoning,
                    std::unique_ptr<OgaNamedTensors> named_tensors = nullptr);

  // Shared implementation for text and media creation paths. The caller supplies the fully rendered prompt because
  // the two paths project their messages differently: text builds from the transcript, media rewrites MessageItems
  // so the template inserts media sentinels. Both share search-options validation, guidance setup, media tensor
  // preparation, and generator construction.
  static std::unique_ptr<OnnxChatGenerator> CreateImpl(PreparedChatPrompt prepared,
                                                       const SearchOptions& options,
                                                       GenAIModelInstance& model,
                                                       const ToolCallContext& tool_ctx,
                                                       bool use_full_context);

  std::unique_ptr<OgaGeneratorParams> gen_params_;
  std::unique_ptr<OgaGenerator> generator_;
  std::unique_ptr<OgaTokenizerStream> stream_;
  // Holds the named tensors produced by OgaMultiModalProcessor media processing
  // for the lifetime of the generator. Generator retains shared_ptr<Tensor>
  // copies internally, but we keep the wrapper alive for symmetry with
  // upstream C# and to guarantee defensive lifetime safety.
  std::unique_ptr<OgaNamedTensors> named_tensors_;
  GenAIModelInstance& model_;  // non-owning reference — model outlives generator
  int prompt_token_count_ = 0;
  // Kept so each appended turn can be re-probed for a template-opened reasoning block.
  ReasoningMarkers reasoning_markers_;
  bool prompt_opens_reasoning_ = false;
  std::optional<int32_t> current_token_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace fl
