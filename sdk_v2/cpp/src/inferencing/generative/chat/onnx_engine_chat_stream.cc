// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/onnx_engine_chat_stream.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "inferencing/generative/genai_model_instance.h"

#include <ort_genai.h>

#include <algorithm>

namespace fl {

namespace {

std::optional<flFinishReason> MapFinishReason(OgaFinishReason reason) {
  switch (reason) {
    case OgaFinishReason_Eos:
    case OgaFinishReason_StopString:
      return FOUNDRY_LOCAL_FINISH_STOP;
    case OgaFinishReason_MaxGeneratedTokens:
    case OgaFinishReason_MaxSessionTokens:
      return FOUNDRY_LOCAL_FINISH_LENGTH;
    case OgaFinishReason_Cancelled:
      return FOUNDRY_LOCAL_FINISH_NONE;
    case OgaFinishReason_Failed:
      return FOUNDRY_LOCAL_FINISH_ERROR;
    default:
      return std::nullopt;
  }
}

}  // namespace

namespace onnx_engine_chat_stream_internal {

std::optional<BackendTerminationCause> MapTerminationCause(uint32_t reason) {
  switch (static_cast<OgaFinishReason>(reason)) {
    case OgaFinishReason_Eos:
      return BackendTerminationCause::kNaturalEnd;
    case OgaFinishReason_StopString:
      return BackendTerminationCause::kStopSequence;
    case OgaFinishReason_MaxGeneratedTokens:
      return BackendTerminationCause::kOutputTokenLimit;
    case OgaFinishReason_MaxSessionTokens:
      return BackendTerminationCause::kSessionTokenLimit;
    case OgaFinishReason_Cancelled:
      return BackendTerminationCause::kCancellation;
    case OgaFinishReason_Failed:
      return BackendTerminationCause::kFailure;
    default:
      return std::nullopt;
  }
}

}  // namespace onnx_engine_chat_stream_internal

OnnxEngineChatStream::OnnxEngineChatStream(
    OnnxChatEngine& engine,
    std::shared_ptr<OnnxChatEngine::Conversation> conversation,
    std::unique_ptr<OgaTokenizerStream> stream,
    GenAIModelInstance& model,
    int prompt_token_count)
    : engine_(engine),
      conversation_(std::move(conversation)),
      stream_(std::move(stream)),
      model_(model),
      prompt_token_count_(prompt_token_count) {}

OnnxEngineChatStream::~OnnxEngineChatStream() {
  try {
    Close();
  } catch (...) {
  }
}

bool OnnxEngineChatStream::IsDone() const {
  return cancelled_ || engine_.IsTurnFinished(conversation_);
}

void OnnxEngineChatStream::GenerateNextToken() {
  if (cancelled_) {
    return;
  }

  try {
    current_token_ = engine_.WaitForToken(conversation_);
  } catch (const fl::Exception&) {
    throw;
  } catch (const std::runtime_error& e) {
    if (!cancelled_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("Engine token generation failed: ") + e.what());
    }
  }
}

std::string OnnxEngineChatStream::Decode() {
  if (!current_token_) {
    return "";
  }

  const int32_t token_id = *current_token_;
  current_token_.reset();

  const auto& tag_info = model_.GetTagInfo();
  if (tag_info.bot_id.has_value() && token_id == *tag_info.bot_id) {
    stream_->Decode(token_id);
    return tag_info.bot_str;
  }
  if (tag_info.eot_id.has_value() && token_id == *tag_info.eot_id) {
    stream_->Decode(token_id);
    return tag_info.eot_str;
  }
  if (tag_info.bor_id.has_value() && token_id == *tag_info.bor_id) {
    stream_->Decode(token_id);
    return tag_info.bor_str;
  }
  if (tag_info.eor_id.has_value() && token_id == *tag_info.eor_id) {
    stream_->Decode(token_id);
    return tag_info.eor_str;
  }

  const char* token_text = stream_->Decode(token_id);
  return token_text ? std::string(token_text) : "";
}

std::optional<int32_t> OnnxEngineChatStream::CurrentTokenId() const {
  return current_token_;
}

int OnnxEngineChatStream::TokenCount() const {
  return static_cast<int>(engine_.SequenceLength(conversation_));
}

int OnnxEngineChatStream::PromptTokenCount() const {
  return prompt_token_count_;
}

void OnnxEngineChatStream::Cancel() {
  cancelled_ = true;
  engine_.Cancel(conversation_);
}

void OnnxEngineChatStream::Close() {
  if (closed_) {
    return;
  }

  engine_.Close(conversation_);
  closed_ = true;
}

int OnnxEngineChatStream::AppendMessages(const std::vector<TranscriptMessage>& new_messages,
                                         const std::vector<TranscriptMessage>& full_messages,
                                         GenAIModelInstance& model,
                                         const ToolCallContext& tool_ctx,
                                         const SearchOptions& options) {
  return AppendMessages(new_messages,
                        chat_internal::PrepareChatMessages(full_messages, model.HasPositionalToolResults()),
                        model, tool_ctx, options);
}

int OnnxEngineChatStream::AppendMessages(const std::vector<TranscriptMessage>& new_messages,
                                         const chat_internal::PreparedChatMessages& full_messages,
                                         GenAIModelInstance& model,
                                         const ToolCallContext& tool_ctx,
                                         const SearchOptions& options) {
  const auto prepared = PrepareTextChatPrompt(full_messages, model, tool_ctx);
  return AppendPreparedPrompt(new_messages, prepared, model, tool_ctx, options);
}

int OnnxEngineChatStream::AppendPreparedPrompt(const std::vector<TranscriptMessage>& new_messages,
                                               const PreparedChatPrompt& prepared,
                                               GenAIModelInstance&,
                                               const ToolCallContext& tool_ctx,
                                               const SearchOptions& options) {
  if (new_messages.empty() || prepared.token_ids.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "new_messages and full_messages must not be empty");
  }

  const int count = static_cast<int>(prepared.prompt_token_count);
  const std::span<const int32_t> full_prompt(prepared.token_ids);
  const auto resident_tokens = engine_.ResidentTokens(conversation_);
  const auto suffix_start = chat_internal::FindUnmatchedPromptSuffix(resident_tokens, full_prompt);
  const bool prompt_opens_reasoning =
      fl::PromptOpensReasoning(full_prompt, ResolveReasoningMarkers(tool_ctx, model_), prepared.prompt);

  // Keep the previous decoder intact if admission fails. Once admitted, start a fresh stream so partial UTF-8/BPE
  // state from the prior turn cannot affect generated tokens; prompt tokens are never decoded.
  int submitted_tokens = count;
  if (suffix_start.has_value()) {
    const auto suffix = full_prompt.subspan(*suffix_start);
    if (suffix.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "chat template produced no new tokens for a non-empty Engine continuation");
    }

    engine_.BeginTurn(conversation_, suffix, options, tool_ctx, prompt_opens_reasoning);
    submitted_tokens = static_cast<int>(suffix.size());
  } else {
    auto replacement = engine_.CreateConversation(options, tool_ctx, count);
    try {
      engine_.BeginTurn(replacement, full_prompt, options, tool_ctx, prompt_opens_reasoning);
    } catch (...) {
      engine_.Close(replacement);
      throw;
    }

    engine_.Close(conversation_);
    conversation_ = std::move(replacement);
  }

  ResetTurnDecoder();

  // Public chat usage describes the complete logical prompt, not only the suffix admitted to a resident Engine
  // request. The suffix remains an internal KV-reuse optimization.
  prompt_token_count_ = count;
  prompt_opens_reasoning_ = prompt_opens_reasoning;
  cancelled_ = false;
  return submitted_tokens;
}

void OnnxEngineChatStream::ResetTurnDecoder() {
  // OgaTokenizerStream has no reset, and a stream that ended mid-code-point (or mid-BPE-merge) would otherwise
  // corrupt the first chunk of the next turn. Drop any token the previous turn left undecoded for the same reason.
  current_token_.reset();
  stream_ = model_.GetPreprocessor().CreateTokenizerStream();
}

std::optional<ChatTurnUsage> OnnxEngineChatStream::GetTurnUsage() const {
  const auto result = engine_.GetTurnResult(conversation_);
  return ChatTurnUsage{
      prompt_token_count_,
      static_cast<int>(result.generated_tokens),
      MapFinishReason(result.finish_reason),
      onnx_engine_chat_stream_internal::MapTerminationCause(result.finish_reason),
  };
}

std::unique_ptr<OnnxEngineChatStream> OnnxEngineChatStream::Create(
    const std::vector<TranscriptMessage>& messages,
    const SearchOptions& options,
    GenAIModelInstance& model,
    const ToolCallContext& tool_ctx) {
  return Create(chat_internal::PrepareChatMessages(messages, model.HasPositionalToolResults()),
                options, model, tool_ctx);
}

std::unique_ptr<OnnxEngineChatStream> OnnxEngineChatStream::Create(
    const chat_internal::PreparedChatMessages& messages,
    const SearchOptions& options,
    GenAIModelInstance& model,
    const ToolCallContext& tool_ctx) {
  return CreatePrepared(PrepareTextChatPrompt(messages, model, tool_ctx), options, model, tool_ctx);
}

std::unique_ptr<OnnxEngineChatStream> OnnxEngineChatStream::CreatePrepared(
    PreparedChatPrompt prepared,
    const SearchOptions& options,
    GenAIModelInstance& model,
    const ToolCallContext& tool_ctx) {
  if (prepared.token_ids.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "prepared text prompt must not be empty");
  }

  auto* engine = model.GetChatEngine();
  if (!engine) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "model does not own a chat Engine");
  }

  const int prompt_token_count = static_cast<int>(prepared.prompt_token_count);
  const bool prompt_opens_reasoning =
      fl::PromptOpensReasoning(prepared.token_ids, ResolveReasoningMarkers(tool_ctx, model), prepared.prompt);
  auto stream = model.GetPreprocessor().CreateTokenizerStream();
  auto conversation = engine->CreateConversation(options, tool_ctx, prompt_token_count);
  try {
    engine->BeginTurn(conversation, prepared.token_ids, options, tool_ctx, prompt_opens_reasoning);

    auto result = std::unique_ptr<OnnxEngineChatStream>(
        new OnnxEngineChatStream(*engine, std::move(conversation), std::move(stream), model, prompt_token_count));
    result->prompt_opens_reasoning_ = prompt_opens_reasoning;
    return result;
  } catch (...) {
    try {
      engine->Close(conversation);
    } catch (...) {
    }
    throw;
  }
}

}  // namespace fl
