// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/chat/onnx_chat_engine.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"

#include <atomic>
#include <memory>
#include <optional>

struct OgaTokenizerStream;

namespace fl {

class GenAIModelInstance;

/// ChatGenerator adapter for a conversation scheduled by a model-owned ORT GenAI Engine.
class OnnxEngineChatGenerator final : public ChatGenerator {
 public:
  ~OnnxEngineChatGenerator() override;

  bool IsDone() const override;
  void GenerateNextToken() override;
  std::string Decode() override;
  int TokenCount() const override;
  int PromptTokenCount() const override;
  void Cancel() override;
  int AppendMessages(const std::vector<MessageItem>& new_messages,
                     GenAIModelInstance& model,
                     const std::string& tools_json,
                     const SearchOptions& options) override;
  bool CanRewind() const override { return false; }
  void RewindTo(int token_count) override;
  std::optional<ChatTurnUsage> GetTurnUsage() const override;

  static std::unique_ptr<OnnxEngineChatGenerator> Create(
      const std::vector<MessageItem>& messages,
      const SearchOptions& options,
      GenAIModelInstance& model,
      const ToolCallContext& tool_ctx);

 private:
  OnnxEngineChatGenerator(OnnxChatEngine& engine,
                          std::shared_ptr<OnnxChatEngine::Conversation> conversation,
                          std::unique_ptr<OgaTokenizerStream> stream,
                          std::unique_ptr<OgaTokenizerStream> stream_with_special,
                          GenAIModelInstance& model,
                          int prompt_token_count);

  OnnxChatEngine& engine_;
  std::shared_ptr<OnnxChatEngine::Conversation> conversation_;
  std::unique_ptr<OgaTokenizerStream> stream_;
  std::unique_ptr<OgaTokenizerStream> stream_with_special_;
  GenAIModelInstance& model_;
  int prompt_token_count_ = 0;
  std::optional<int32_t> current_token_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace fl
