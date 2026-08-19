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

#include <utility>

namespace fl {

bool ShouldUseEngineChatGenerator(const GenAIConfig& config,
                                  bool model_is_multimodal,
                                  bool request_has_media) {
  return config.SupportsDynamicBatching() && !model_is_multimodal && !request_has_media;
}

EngineChatGenerator::EngineChatGenerator(std::shared_ptr<EngineHost> host,
                                         std::shared_ptr<EngineRequest> request,
                                         TokenDecoder decoder,
                                         int prompt_token_count,
                                         CancellationRequested cancellation_requested)
    : host_(std::move(host)),
      request_(std::move(request)),
      decoder_(std::move(decoder)),
      cancellation_requested_(std::move(cancellation_requested)),
      prompt_token_count_(prompt_token_count) {
  if (!host_ || !request_ || !decoder_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "engine chat generator dependencies must not be null");
  }
}

EngineChatGenerator::~EngineChatGenerator() {
  try {
    Cancel();
  } catch (...) {
    // Explicit request cancellation and error paths report close failures; destruction cannot throw.
  }
}

bool EngineChatGenerator::IsDone() const {
  if (cancelled_) {
    return true;
  }

  if (current_token_.has_value() || request_->HasGeneratedTokens()) {
    return false;
  }

  return request_->IsTurnComplete();
}

void EngineChatGenerator::GenerateNextToken() {
  if (cancelled_ || current_token_.has_value()) {
    return;
  }

  while (!cancelled_) {
    if (cancellation_requested_ && cancellation_requested_()) {
      Cancel();
      return;
    }

    if (auto token = request_->PopGeneratedToken()) {
      current_token_ = *token;
      ++generated_token_count_;
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
  return prompt_token_count_ + generated_token_count_;
}

int EngineChatGenerator::PromptTokenCount() const {
  return prompt_token_count_;
}

void EngineChatGenerator::Cancel() {
  if (cancelled_) {
    return;
  }

  try {
    request_->Close();
    cancelled_ = true;
  } catch (...) {
    cancelled_ = false;
    throw;
  }
}

std::unique_ptr<EngineChatGenerator> EngineChatGenerator::Create(const std::vector<MessageItem>& messages,
                                                                 const SearchOptions& options,
                                                                 GenAIModelInstance& model,
                                                                 const ToolCallContext& tool_ctx,
                                                                 CancellationRequested cancellation_requested) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  const auto prompt = BuildChatPrompt(messages, model, tool_ctx.tools_json);
  auto sequences = EncodePrompt(prompt, model);
  const auto prompt_token_count = static_cast<int>(sequences->SequenceCount(0));
  const auto prompt_tokens =
      std::span<const int32_t>(sequences->SequenceData(0), sequences->SequenceCount(0));

  auto gen_params = OgaGeneratorParams::Create(model.GetOgaModel());
  ApplySearchOptions(options, prompt_token_count, model.GetGenAIConfig(), *gen_params, model.EP());
  gen_params->SetSearchOption("batch_size", 1.0);
  gen_params->SetSearchOption("num_beams", 1.0);
  ApplyGeneratorGuidance(tool_ctx, *gen_params);

  auto token_decoder = std::make_shared<ChatTokenDecoder>(model);
  TokenDecoder decoder = [token_decoder = std::move(token_decoder)](int32_t token) {
    return token_decoder->Decode(token);
  };

  auto host = model.GetEngineHost();
  auto request = host->Submit(*gen_params, prompt_tokens, EngineRequestMode::kStateless);
  try {
    return std::make_unique<EngineChatGenerator>(
        host, request, std::move(decoder), prompt_token_count, std::move(cancellation_requested));
  } catch (...) {
    request->Close();
    throw;
  }
}

}  // namespace fl
