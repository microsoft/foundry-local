// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/chat/onnx_chat_engine.h"
#include "inferencing/generative/chat/prepared_chat_prompt.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"

#include <atomic>
#include <memory>
#include <optional>

struct OgaTokenizerStream;

namespace fl {

class GenAIModelInstance;

namespace onnx_engine_chat_stream_internal {

std::optional<BackendTerminationCause> MapTerminationCause(uint32_t reason);
ChatTurnUsage BuildTurnUsage(int prompt_tokens, const OnnxChatEngine::TurnResult& result);

}  // namespace onnx_engine_chat_stream_internal

/// ChatGenerator stream for a conversation scheduled by a model-owned ORT GenAI Engine.
class OnnxEngineChatStream final : public ChatGenerator {
 public:
  ~OnnxEngineChatStream() override;

  bool IsDone() const override;
  void GenerateNextToken() override;
  std::string Decode() override;
  std::optional<int32_t> CurrentTokenId() const override;
  int TokenCount() const override;
  int PromptTokenCount() const override;
  void Cancel() override;
  void Close() override;
  int AppendMessages(const std::vector<TranscriptMessage>& new_messages,
                     const chat_internal::PreparedChatMessages& full_messages,
                     GenAIModelInstance& model,
                     const ToolCallContext& tool_ctx,
                     const SearchOptions& options) override;
  int AppendPreparedPrompt(const std::vector<TranscriptMessage>& new_messages,
                           const PreparedChatPrompt& prompt,
                           GenAIModelInstance& model,
                           const ToolCallContext& tool_ctx,
                           const SearchOptions& options) override;
  int AppendMessages(const std::vector<TranscriptMessage>& new_messages,
                     const std::vector<TranscriptMessage>& full_messages,
                     GenAIModelInstance& model,
                     const ToolCallContext& tool_ctx,
                     const SearchOptions& options);
  bool PromptOpensReasoning() const override { return prompt_opens_reasoning_; }
  std::optional<ChatTurnUsage> GetTurnUsage() const override;

  static std::unique_ptr<OnnxEngineChatStream> Create(
      const std::vector<TranscriptMessage>& messages,
      const SearchOptions& options,
      GenAIModelInstance& model,
      const ToolCallContext& tool_ctx);
  static std::unique_ptr<OnnxEngineChatStream> CreatePrepared(PreparedChatPrompt prepared,
                                                              const SearchOptions& options,
                                                              GenAIModelInstance& model,
                                                              const ToolCallContext& tool_ctx);
  static std::unique_ptr<OnnxEngineChatStream> Create(
      const chat_internal::PreparedChatMessages& messages,
      const SearchOptions& options,
      GenAIModelInstance& model,
      const ToolCallContext& tool_ctx);

 private:
  OnnxEngineChatStream(OnnxChatEngine& engine,
                       std::shared_ptr<OnnxChatEngine::Conversation> conversation,
                       std::unique_ptr<OgaTokenizerStream> stream,
                       GenAIModelInstance& model,
                       int prompt_token_count);

  /// Replace this turn's token decoder. Called only once the Engine has admitted a turn.
  void ResetTurnDecoder();

  OnnxChatEngine& engine_;
  std::shared_ptr<OnnxChatEngine::Conversation> conversation_;
  std::unique_ptr<OgaTokenizerStream> stream_;
  GenAIModelInstance& model_;
  int prompt_token_count_ = 0;
  bool prompt_opens_reasoning_ = false;
  bool closed_ = false;
  std::optional<int32_t> current_token_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace fl
