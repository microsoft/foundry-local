// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/genai_config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fl {

class EngineHost;
class EngineRequest;
class GenAIModelInstance;
struct MessageItem;
struct SearchOptions;
struct ToolCallContext;

/// Returns whether a request qualifies for the engine-backed text path.
bool ShouldUseEngineChatGenerator(const GenAIConfig& config, bool model_is_multimodal, bool request_has_media);

enum class EngineChatGeneratorMode {
  kStateless,
  kResident,
};

/// Returns the portion of a fully rendered prompt that is not already resident.
///
/// A value is returned only when every resident token is an exact prefix of the complete prompt. The returned span
/// refers to complete_prompt_tokens and may be empty when both sequences end at the same boundary.
std::optional<std::span<const int32_t>> ReconcileResidentTokenSuffix(
    std::span<const int32_t> resident_tokens,
    std::span<const int32_t> complete_prompt_tokens) noexcept;

/// ChatGenerator adapter over a model's shared EngineHost.
///
/// Cancellation is cooperative at EngineHost's serialized Step boundary. ORT GenAI has no public mid-Step
/// cancellation for engine requests, so cancellation waits for the active Step and prevents this request from
/// driving another one. Stateless requests release KV state at completion. Resident requests keep it until an
/// explicit continuation, cancellation, or destruction.
class EngineChatGenerator final : public ChatGenerator {
 public:
  using TokenDecoder = std::function<std::string(int32_t)>;
  using CancellationRequested = std::function<bool()>;

  EngineChatGenerator(std::shared_ptr<EngineHost> host,
                      std::shared_ptr<EngineRequest> request,
                      TokenDecoder decoder,
                      std::vector<int32_t> initial_token_ids,
                      EngineChatGeneratorMode mode = EngineChatGeneratorMode::kStateless,
                      CancellationRequested cancellation_requested = {});
  ~EngineChatGenerator() override;

  bool IsDone() const override;
  void GenerateNextToken() override;
  std::string Decode() override;
  std::optional<int32_t> CurrentTokenId() const override { return current_token_; }
  int TokenCount() const override;
  int PromptTokenCount() const override;
  void Cancel() override;

  /// Resident continuation is valid only after all prior output has been consumed and the turn is complete.
  bool IsReadyForContinuation() const;

  /// Continues from the exact suffix of a fully rendered prompt. Returns false without appending when the request is
  /// not ready, the resident ledger is not an exact prefix, or the suffix is empty.
  bool TryContinue(std::span<const int32_t> complete_prompt_tokens,
                   CancellationRequested cancellation_requested = {});

  EngineChatGeneratorMode Mode() const noexcept { return mode_; }
  std::span<const int32_t> InitialTokenIds() const noexcept;
  std::span<const int32_t> GeneratedTokenIds() const noexcept { return generated_token_ids_; }
  std::span<const int32_t> ResidentTokenIds() const noexcept { return resident_token_ids_; }

  /// Render and tokenize a complete conversation using the canonical chat-template helpers.
  static std::vector<int32_t> EncodeMessages(const std::vector<MessageItem>& messages,
                                             GenAIModelInstance& model,
                                             const std::string& tools_json);

  static std::unique_ptr<EngineChatGenerator> Create(const std::vector<MessageItem>& messages,
                                                     const SearchOptions& options,
                                                     GenAIModelInstance& model,
                                                     const ToolCallContext& tool_ctx,
                                                     EngineChatGeneratorMode mode =
                                                         EngineChatGeneratorMode::kStateless,
                                                     CancellationRequested cancellation_requested = {});

 private:
  void GenerateNextTokenImpl();
  void CloseNoThrow() noexcept;

  std::shared_ptr<EngineHost> host_;
  std::shared_ptr<EngineRequest> request_;
  TokenDecoder decoder_;
  CancellationRequested cancellation_requested_;
  std::optional<int32_t> current_token_;
  std::vector<int32_t> resident_token_ids_;
  std::vector<int32_t> generated_token_ids_;
  std::size_t initial_token_count_;
  int prompt_token_count_;
  EngineChatGeneratorMode mode_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace fl
