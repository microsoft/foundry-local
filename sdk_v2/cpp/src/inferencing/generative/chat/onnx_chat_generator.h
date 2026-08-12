// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/genai_model_instance.h"
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

  /// Encode new messages and append their tokens to the generator's sequence.
  /// Used for continuous decoding — only the new turn's messages are encoded and appended.
  /// Returns the number of new prompt tokens appended.
  int AppendMessages(const std::vector<MessageItem>& new_messages,
                     GenAIModelInstance& model,
                     const std::string& tools_json);

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

  /// Factory: create a vision-enabled chat generator.
  ///
  /// Processes `images` via the model's OgaMultiModalProcessor and feeds the
  /// resulting named tensors into OgaGenerator::SetInputs (in place of the
  /// usual encoded-prompt path). The last user message in `messages` is
  /// rewritten to `[{"type":"image"},{"type":"text","text":...}]` so the chat
  /// template inserts the model's image sentinel tokens.
  ///
  /// Requires `model.IsMultiModal()`. `images` must be non-empty.
  /// Continuous decoding (AppendMessages) has no image-aware path; callers
  /// must always create a fresh generator for a vision turn.
  ///
  /// @throws fl::Exception (FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) on a
  ///         non-multimodal model, empty/oversize image list, or unreadable
  ///         image bytes.
  static std::unique_ptr<OnnxChatGenerator> CreateWithImages(
      const std::vector<MessageItem>& messages,
      const SearchOptions& options,
      GenAIModelInstance& model,
      const std::vector<const ImageItem*>& images,
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
  /// Mirrors upstream C# OnnxChatGenerator.TransformMessagesForVision.
  static std::string TransformMessagesForVision(const std::vector<MessageItem>& messages);

 private:
  OnnxChatGenerator(std::unique_ptr<OgaGeneratorParams> gen_params,
                    std::unique_ptr<OgaGenerator> generator,
                    std::unique_ptr<OgaTokenizerStream> stream,
                    std::unique_ptr<OgaTokenizerStream> stream_with_special,
                    GenAIModelInstance& model,
                    int prompt_token_count,
                    std::unique_ptr<OgaNamedTensors> named_tensors = nullptr);

  // Shared implementation for both Create (text) and CreateWithImages (vision).
  // `images` empty → text path; non-empty → vision path. Centralised here so
  // both public entry points share search-options validation, guidance setup,
  // and generator construction.
  static std::unique_ptr<OnnxChatGenerator> CreateImpl(const std::vector<MessageItem>& messages,
                                                       const SearchOptions& options,
                                                       GenAIModelInstance& model,
                                                       const ToolCallContext& tool_ctx,
                                                       bool use_full_context,
                                                       const std::vector<const ImageItem*>& images);

  std::unique_ptr<OgaGeneratorParams> gen_params_;
  std::unique_ptr<OgaGenerator> generator_;
  std::unique_ptr<OgaTokenizerStream> stream_;
  std::unique_ptr<OgaTokenizerStream> stream_with_special_;  // for tool call token detection
  // Holds the named tensors produced by OgaMultiModalProcessor::ProcessImages
  // for the lifetime of the generator. Generator retains shared_ptr<Tensor>
  // copies internally, but we keep the wrapper alive for symmetry with
  // upstream C# and to guarantee defensive lifetime safety.
  std::unique_ptr<OgaNamedTensors> named_tensors_;
  GenAIModelInstance& model_;  // non-owning reference — model outlives generator
  int prompt_token_count_ = 0;
  std::atomic<bool> cancelled_{false};
};

}  // namespace fl
