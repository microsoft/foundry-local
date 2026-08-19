// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/engine_chat_generator.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_token_decoder.h"
#include "inferencing/generative/chat/generator_guidance.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/engine/engine_host.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "items/message_item.h"

#include <ort_genai.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace fl {

bool ShouldUseEngineChatGenerator(const GenAIConfig& config,
                                  bool model_is_multimodal,
                                  bool request_has_media) {
  return config.SupportsDynamicBatching() && !model_is_multimodal && !request_has_media;
}

std::optional<std::span<const int32_t>> ReconcileResidentTokenSuffix(
    std::span<const int32_t> resident_tokens,
    std::span<const int32_t> complete_prompt_tokens) noexcept {
  if (resident_tokens.size() > complete_prompt_tokens.size()) {
    return std::nullopt;
  }

  if (!std::equal(resident_tokens.begin(), resident_tokens.end(), complete_prompt_tokens.begin())) {
    return std::nullopt;
  }

  return complete_prompt_tokens.subspan(resident_tokens.size());
}

EngineChatGenerator::EngineChatGenerator(std::shared_ptr<EngineHost> host,
                                         std::shared_ptr<EngineRequest> request,
                                         TokenDecoder decoder,
                                         std::vector<int32_t> initial_token_ids,
                                         EngineChatGeneratorMode mode,
                                         CancellationRequested cancellation_requested)
    : host_(std::move(host)),
      request_(std::move(request)),
      decoder_(std::move(decoder)),
      cancellation_requested_(std::move(cancellation_requested)),
      resident_token_ids_(std::move(initial_token_ids)),
      initial_token_count_(resident_token_ids_.size()),
      prompt_token_count_(0),
      mode_(mode) {
  if (!host_ || !request_ || !decoder_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "engine chat generator dependencies must not be null");
  }

  if (resident_token_ids_.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "engine chat prompt has too many tokens");
  }

  prompt_token_count_ = static_cast<int>(resident_token_ids_.size());
}

EngineChatGenerator::~EngineChatGenerator() {
  CloseNoThrow();
}

bool EngineChatGenerator::IsDone() const {
  if (cancelled_.load(std::memory_order_relaxed)) {
    return true;
  }

  if (current_token_.has_value() || request_->HasGeneratedTokens()) {
    return false;
  }

  return request_->IsTurnComplete();
}

void EngineChatGenerator::GenerateNextToken() {
  try {
    GenerateNextTokenImpl();
  } catch (...) {
    CloseNoThrow();
    throw;
  }
}

void EngineChatGenerator::GenerateNextTokenImpl() {
  if (cancelled_.load(std::memory_order_relaxed) || current_token_.has_value()) {
    return;
  }

  while (!cancelled_.load(std::memory_order_relaxed)) {
    if (cancellation_requested_ && cancellation_requested_()) {
      Cancel();
      return;
    }

    if (auto token = request_->PopGeneratedToken()) {
      current_token_ = *token;
      generated_token_ids_.push_back(*token);
      resident_token_ids_.push_back(*token);
      return;
    }

    if (request_->IsTurnComplete()) {
      return;
    }

    const auto progressed_request = host_->Step();
    if (cancellation_requested_ && cancellation_requested_()) {
      // Step is serialized by EngineHost. Closing here prevents this request from driving another shared Step.
      Cancel();
      return;
    }

    if (!progressed_request && !request_->IsTurnComplete()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "engine made no progress for a pending chat request");
    }
  }
}

std::string EngineChatGenerator::Decode() {
  if (!current_token_) {
    return "";
  }

  const auto token = *current_token_;
  current_token_.reset();
  return decoder_(token);
}

int EngineChatGenerator::TokenCount() const {
  return prompt_token_count_ + static_cast<int>(generated_token_ids_.size());
}

int EngineChatGenerator::PromptTokenCount() const {
  return prompt_token_count_;
}

void EngineChatGenerator::Cancel() {
  if (cancelled_.load(std::memory_order_relaxed)) {
    return;
  }

  try {
    request_->Close();
    cancelled_.store(true, std::memory_order_relaxed);
  } catch (...) {
    cancelled_.store(false, std::memory_order_relaxed);
    throw;
  }
}

void EngineChatGenerator::CloseNoThrow() noexcept {
  try {
    Cancel();
  } catch (...) {
    // Explicit cancellation reports close failures. Destruction and error cleanup cannot replace the primary error.
  }
}

bool EngineChatGenerator::IsReadyForContinuation() const {
  if (mode_ != EngineChatGeneratorMode::kResident ||
      cancelled_.load(std::memory_order_relaxed) ||
      request_->IsClosed() ||
      current_token_.has_value() ||
      request_->HasGeneratedTokens()) {
    return false;
  }

  return request_->IsTurnComplete();
}

bool EngineChatGenerator::TryContinue(std::span<const int32_t> complete_prompt_tokens,
                                      CancellationRequested cancellation_requested) {
  if (!IsReadyForContinuation()) {
    return false;
  }

  if (complete_prompt_tokens.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "engine chat prompt has too many tokens");
  }

  const auto suffix = ReconcileResidentTokenSuffix(resident_token_ids_, complete_prompt_tokens);
  if (!suffix.has_value() || suffix->empty()) {
    return false;
  }

  auto continued_tokens = resident_token_ids_;
  continued_tokens.insert(continued_tokens.end(), suffix->begin(), suffix->end());

  request_->Continue(*suffix);

  resident_token_ids_.swap(continued_tokens);
  generated_token_ids_.clear();
  prompt_token_count_ = static_cast<int>(complete_prompt_tokens.size());
  cancellation_requested_ = std::move(cancellation_requested);
  return true;
}

std::span<const int32_t> EngineChatGenerator::InitialTokenIds() const noexcept {
  return std::span<const int32_t>(resident_token_ids_).first(initial_token_count_);
}

std::vector<int32_t> EngineChatGenerator::EncodeMessages(const std::vector<MessageItem>& messages,
                                                         GenAIModelInstance& model,
                                                         const std::string& tools_json) {
  const auto prompt = BuildChatPrompt(messages, model, tools_json);
  return model.GetPreprocessor().EncodeTokenIds(prompt);
}

std::unique_ptr<EngineChatGenerator> EngineChatGenerator::Create(const std::vector<MessageItem>& messages,
                                                                 const SearchOptions& options,
                                                                 GenAIModelInstance& model,
                                                                 const ToolCallContext& tool_ctx,
                                                                 EngineChatGeneratorMode mode,
                                                                 CancellationRequested cancellation_requested) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  auto prompt_token_ids = EncodeMessages(messages, model, tool_ctx.tools_json);
  if (prompt_token_ids.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "engine chat prompt has too many tokens");
  }

  const auto prompt_tokens = std::span<const int32_t>(prompt_token_ids);
  const auto prompt_token_count = static_cast<int>(prompt_tokens.size());

  auto gen_params = OgaGeneratorParams::Create(model.GetOgaModel());
  const auto resident = mode == EngineChatGeneratorMode::kResident;
  ApplySearchOptions(options, prompt_token_count, model.GetGenAIConfig(), *gen_params, model.EP(), resident);
  gen_params->SetSearchOption("batch_size", 1.0);
  gen_params->SetSearchOption("num_beams", 1.0);
  ApplyGeneratorGuidance(tool_ctx, *gen_params);

  auto token_decoder = std::make_shared<ChatTokenDecoder>(model);
  TokenDecoder decoder = [token_decoder = std::move(token_decoder)](int32_t token) {
    return token_decoder->Decode(token);
  };

  auto host = model.GetEngineHost();
  const auto request_mode = resident ? EngineRequestMode::kResident : EngineRequestMode::kStateless;
  auto request = host->Submit(*gen_params, prompt_tokens, request_mode);
  try {
    return std::make_unique<EngineChatGenerator>(
        host, request, std::move(decoder), std::move(prompt_token_ids),
        mode, std::move(cancellation_requested));
  } catch (...) {
    request->Close();
    throw;
  }
}

}  // namespace fl
