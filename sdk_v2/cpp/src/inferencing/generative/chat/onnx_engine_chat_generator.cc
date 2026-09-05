// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/onnx_engine_chat_generator.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/genai_model_instance.h"

#include <ort_genai.h>

#include <algorithm>

namespace fl {

namespace {

std::optional<flFinishReason> MapFinishReason(OgaFinishReason reason) {
  switch (reason) {
    case OgaFinishReason_Eos:
#if defined(OgaFinishReason_StopSequence)
    case OgaFinishReason_StopSequence:
#elif defined(OgaFinishReason_StopString)
    case OgaFinishReason_StopString:
#endif
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

OnnxEngineChatGenerator::OnnxEngineChatGenerator(
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

OnnxEngineChatGenerator::~OnnxEngineChatGenerator() {
  try {
    engine_.Close(conversation_);
  } catch (...) {
  }
}

bool OnnxEngineChatGenerator::IsDone() const {
  return cancelled_ || engine_.IsTurnFinished(conversation_);
}

void OnnxEngineChatGenerator::GenerateNextToken() {
  if (cancelled_) {
    return;
  }

  try {
    current_token_ = engine_.WaitForToken(conversation_);
  } catch (const std::runtime_error& e) {
    if (!cancelled_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("Engine token generation failed: ") + e.what());
    }
  }
}

std::string OnnxEngineChatGenerator::Decode() {
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

int OnnxEngineChatGenerator::TokenCount() const {
  return static_cast<int>(engine_.SequenceLength(conversation_));
}

int OnnxEngineChatGenerator::PromptTokenCount() const {
  return prompt_token_count_;
}

void OnnxEngineChatGenerator::Cancel() {
  cancelled_ = true;
  engine_.Cancel(conversation_);
}

int OnnxEngineChatGenerator::AppendMessages(const std::vector<MessageItem>& new_messages,
                                            GenAIModelInstance& model,
                                            const std::string& tools_json,
                                            const SearchOptions& options) {
  if (new_messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "new_messages must not be empty");
  }

  auto prompt = BuildChatContinuationPrompt(new_messages, model, tools_json);
  auto sequences = EncodePrompt(prompt, model);
  const int count = static_cast<int>(sequences->SequenceCount(0));
  const auto* data = sequences->SequenceData(0);
  engine_.BeginTurn(conversation_, std::span<const int32_t>(data, static_cast<size_t>(count)),
                    ResolveMaxOutputTokens(options));
  prompt_token_count_ = count;
  cancelled_ = false;
  return count;
}

void OnnxEngineChatGenerator::RewindTo(int /*token_count*/) {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
           "Engine request rewind is unavailable; recreate the request from retained conversation history");
}

std::optional<ChatTurnUsage> OnnxEngineChatGenerator::GetTurnUsage() const {
  const auto result = engine_.GetTurnResult(conversation_);
  return ChatTurnUsage{
      static_cast<int>(result.prompt_tokens + result.cached_prompt_tokens),
      static_cast<int>(result.generated_tokens),
      MapFinishReason(result.finish_reason),
  };
}

std::unique_ptr<OnnxEngineChatGenerator> OnnxEngineChatGenerator::Create(
    const std::vector<MessageItem>& messages,
    const SearchOptions& options,
    GenAIModelInstance& model,
    const ToolCallContext& tool_ctx) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  auto* engine = model.GetChatEngine();
  if (!engine) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "model does not own a chat Engine");
  }

  auto prompt = BuildChatPrompt(messages, model, tool_ctx.tools_json);
  auto sequences = EncodePrompt(prompt, model);
  const int prompt_token_count = static_cast<int>(sequences->SequenceCount(0));
  auto stream = model.GetPreprocessor().CreateTokenizerStream();
  auto conversation = engine->CreateConversation(options, tool_ctx, prompt_token_count);
  try {
    const auto* data = sequences->SequenceData(0);
    engine->BeginTurn(conversation, std::span<const int32_t>(data, static_cast<size_t>(prompt_token_count)),
                      ResolveMaxOutputTokens(options));

    return std::unique_ptr<OnnxEngineChatGenerator>(
        new OnnxEngineChatGenerator(*engine, std::move(conversation), std::move(stream), model,
                                    prompt_token_count));
  } catch (...) {
    try {
      engine->Close(conversation);
    } catch (...) {
    }
    throw;
  }
}

}  // namespace fl
