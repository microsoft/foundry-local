// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/genai_config.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fl {

class EngineHost;
class EngineRequest;
class GenAIModelInstance;
struct MessageItem;
struct SearchOptions;
struct ToolCallContext;

/// Returns whether a stateless request qualifies for the engine-backed text path.
bool ShouldUseEngineChatGenerator(const GenAIConfig& config, bool model_is_multimodal, bool request_has_media);

/// Stateless ChatGenerator adapter over a model's shared EngineHost.
///
/// Cancellation is cooperative at EngineHost's serialized Step boundary. ORT GenAI has no public mid-Step
/// cancellation for engine requests, so cancellation waits for the active Step and prevents this request from
/// driving another one. The request is always removed and never retains KV state after completion or cancellation.
class EngineChatGenerator final : public ChatGenerator {
 public:
  using TokenDecoder = std::function<std::string(int32_t)>;
  using CancellationRequested = std::function<bool()>;

  EngineChatGenerator(std::shared_ptr<EngineHost> host,
                      std::shared_ptr<EngineRequest> request,
                      TokenDecoder decoder,
                      int prompt_token_count,
                      CancellationRequested cancellation_requested = {});
  ~EngineChatGenerator() override;

  bool IsDone() const override;
  void GenerateNextToken() override;
  std::string Decode() override;
  int TokenCount() const override;
  int PromptTokenCount() const override;
  void Cancel() override;

  static std::unique_ptr<EngineChatGenerator> Create(const std::vector<MessageItem>& messages,
                                                     const SearchOptions& options,
                                                     GenAIModelInstance& model,
                                                     const ToolCallContext& tool_ctx,
                                                     CancellationRequested cancellation_requested = {});

 private:
  std::shared_ptr<EngineHost> host_;
  std::shared_ptr<EngineRequest> request_;
  TokenDecoder decoder_;
  CancellationRequested cancellation_requested_;
  std::optional<int32_t> current_token_;
  int prompt_token_count_;
  int generated_token_count_{0};
  std::atomic<bool> cancelled_{false};
};

}  // namespace fl
