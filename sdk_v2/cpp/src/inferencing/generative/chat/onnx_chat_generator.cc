// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "exception.h"
#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "utils.h"

#include <nlohmann/json.hpp>
#include <ort_genai.h>

namespace fl {

namespace {

/// Probe whether the rendered prompt leaves a reasoning block open. Uses the encoded prompt token IDs when they are
/// available (the text path) and falls back to the rendered text for the media path, which has no encoded sequence
/// of its own.
bool DetectPromptOpensReasoning(const std::string& prompt,
                                const OgaSequences* sequences,
                                const ReasoningMarkers& markers) {
  std::span<const int32_t> prompt_token_ids;
  if (sequences != nullptr && sequences->Count() > 0) {
    prompt_token_ids = {sequences->SequenceData(0), sequences->SequenceCount(0)};
  }

  return PromptOpensReasoning(prompt_token_ids, markers, prompt);
}

}  // namespace

ReasoningMarkers ResolveReasoningMarkers(const ToolCallContext& tool_ctx, GenAIModelInstance& model) {
  const auto& tag_info = model.GetTagInfo();

  ReasoningMarkers markers;
  markers.start = tool_ctx.reasoning_start.empty() ? tag_info.bor_str : tool_ctx.reasoning_start;
  markers.end = tool_ctx.reasoning_end.empty() ? tag_info.eor_str : tool_ctx.reasoning_end;

  // A request may override the marker strings. The published IDs describe the model's own markers, so they are
  // reused only when they decode to exactly the marker in effect; anything else is encoded with the model's
  // tokenizer, which also covers overrides that are several tokens long.
  auto encode = [&model](const std::string& text) { return model.EncodeText(text); };
  const PublishedMarker published_start{tag_info.bor_id, tag_info.bor_str};
  markers.start_token_is_published = UsesPublishedToken(markers.start, published_start);
  markers.start_token_ids = ResolveMarkerTokenIds(markers.start, published_start, encode);
  markers.end_token_ids = ResolveMarkerTokenIds(markers.end, {tag_info.eor_id, tag_info.eor_str}, encode);

  return markers;
}

OnnxChatGenerator::~OnnxChatGenerator() = default;

// ---------------------------------------------------------------------------
// Private constructor
// ---------------------------------------------------------------------------

OnnxChatGenerator::OnnxChatGenerator(std::unique_ptr<OgaGeneratorParams> gen_params,
                                     std::unique_ptr<OgaGenerator> generator,
                                     std::unique_ptr<OgaTokenizerStream> stream,
                                     GenAIModelInstance& model,
                                     int prompt_token_count,
                                     ReasoningMarkers reasoning_markers,
                                     bool prompt_opens_reasoning,
                                     std::unique_ptr<OgaNamedTensors> named_tensors)
    : gen_params_(std::move(gen_params)),
      generator_(std::move(generator)),
      stream_(std::move(stream)),
      named_tensors_(std::move(named_tensors)),
      model_(model),
      prompt_token_count_(prompt_token_count),
      reasoning_markers_(std::move(reasoning_markers)),
      prompt_opens_reasoning_(prompt_opens_reasoning) {}

// ---------------------------------------------------------------------------
// ChatGenerator interface
// ---------------------------------------------------------------------------

bool OnnxChatGenerator::IsDone() const {
  if (cancelled_) {
    return true;
  }

  // OgaGenerator::IsDone() is non-const in the ORT GenAI API, so we need const_cast.
  // This is safe because IsDone only reads state.
  auto* gen = const_cast<OgaGenerator*>(generator_.get());
  return gen->IsDone() || gen->IsSessionTerminated();
}

void OnnxChatGenerator::GenerateNextToken() {
  if (cancelled_) {
    current_token_.reset();
    return;
  }

  current_token_.reset();

  try {
    generator_->GenerateNextToken();

    // GetNextTokens returns the batch of next tokens; chat generation always uses batch size 1.
    const auto next_tokens = generator_->GetNextTokens();
    if (!next_tokens.empty()) {
      current_token_ = next_tokens[0];
    }
  } catch (const std::runtime_error& e) {
    // If cancelled while generating, the OGA engine throws when the session is terminated.
    // This is expected — not an error.
    if (cancelled_) {
      return;
    }

    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("token generation failed: ") + e.what());
  }
}

std::string OnnxChatGenerator::Decode() {
  if (cancelled_ || !current_token_.has_value()) {
    return "";
  }

  const auto token_id = *current_token_;
  current_token_.reset();

  // Fast path: if this token matches a known tag ID, return the pre-decoded string.
  // Decode is always single-stream for normal tokens.
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

  // Single decode for all non-tag tokens.
  const char* token_text = stream_->Decode(token_id);
  return token_text ? std::string(token_text) : "";
}

std::optional<int32_t> OnnxChatGenerator::CurrentTokenId() const {
  return current_token_;
}

int OnnxChatGenerator::TokenCount() const {
  return static_cast<int>(generator_->GetSequenceCount(0));
}

int OnnxChatGenerator::PromptTokenCount() const {
  return prompt_token_count_;
}

void OnnxChatGenerator::Cancel() {
  cancelled_ = true;

  // Use the ORT GenAI engine-level termination to interrupt mid-compute
  // (e.g. during a long prefill), not just between token boundaries.
  try {
    generator_->SetRuntimeOption("terminate_session", "1");
  } catch (...) {
    // SetRuntimeOption may not be supported by all ORT GenAI builds
  }
}

// ---------------------------------------------------------------------------
// Continuous decoding: append new messages / rewind
// ---------------------------------------------------------------------------

int OnnxChatGenerator::AppendMessages(const std::vector<TranscriptMessage>& new_messages,
                                      const std::vector<TranscriptMessage>& full_messages,
                                      GenAIModelInstance& model,
                                      const ToolCallContext& tool_ctx,
                                      const SearchOptions& /*options*/) {
  if (new_messages.empty() || full_messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "new_messages and full_messages must not be empty");
  }

  auto prepared = PrepareTextChatPrompt(full_messages, model, tool_ctx);
  return AppendPreparedPrompt(new_messages, prepared, model, tool_ctx, SearchOptions{});
}

int OnnxChatGenerator::AppendPreparedPrompt(const std::vector<TranscriptMessage>& new_messages,
                                            const PreparedChatPrompt& prepared,
                                            GenAIModelInstance& /*model*/,
                                            const ToolCallContext& /*tool_ctx*/,
                                            const SearchOptions& /*options*/) {
  if (new_messages.empty() || prepared.token_ids.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "new_messages and prepared prompt must not be empty");
  }

  const std::span<const int32_t> full_prompt(prepared.token_ids);
  const std::span<const int32_t> resident(generator_->GetSequenceData(0), generator_->GetSequenceCount(0));
  const auto suffix_start = chat_internal::FindUnmatchedPromptSuffix(resident, full_prompt);
  if (!suffix_start.has_value()) {
    throw RetainedPromptMismatchError();
  }

  const auto suffix = full_prompt.subspan(*suffix_start);
  if (suffix.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "chat template produced no new tokens for a non-empty Generator continuation");
  }

  auto suffix_sequences = OgaSequences::Create();
  suffix_sequences->Append(suffix.data(), suffix.size());

  try {
    generator_->AppendTokenSequences(*suffix_sequences);
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("failed to append token sequences: ") + e.what());
  }

  // Re-probe: the appended segment ends with this turn's assistant generation prefix, so it — not the original
  // prompt — determines whether generation resumes inside a template-opened reasoning block.
  auto full_sequences = OgaSequences::Create();
  full_sequences->Append(prepared.token_ids.data(), prepared.token_ids.size());
  prompt_opens_reasoning_ =
      DetectPromptOpensReasoning(prepared.prompt, full_sequences.get(), reasoning_markers_);

  return static_cast<int>(suffix.size());
}

void OnnxChatGenerator::RewindTo(int token_count) {
  try {
    generator_->RewindTo(token_count);
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("failed to rewind generator: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Media helpers
// ---------------------------------------------------------------------------

std::string OnnxChatGenerator::TransformMessagesForMedia(const std::vector<MessageItem>& messages) {
  // Find the index of the last user message so we can rewrite only that one
  // into structured media markers followed by its text content.
  // Other messages are emitted in plain `{"role","content"}` form so the chat
  // template renders them the same way it does for text-only requests.
  size_t last_user_idx = messages.size();
  for (size_t i = messages.size(); i-- > 0;) {
    if (messages[i].role == FOUNDRY_LOCAL_ROLE_USER) {
      last_user_idx = i;
      break;
    }
  }

  nlohmann::json arr = nlohmann::json::array();
  for (size_t i = 0; i < messages.size(); ++i) {
    const auto& msg = messages[i];
    nlohmann::json entry;
    entry["role"] = Utils::RoleToString(msg.role);

    if (i == last_user_idx) {
      auto content = nlohmann::json::array();
      for (const auto& part : msg.content) {
        if (!part.view) {
          continue;
        }
        if (part.view->type == FOUNDRY_LOCAL_ITEM_IMAGE) {
          content.push_back(nlohmann::json{{"type", "image"}});
        } else if (part.view->type == FOUNDRY_LOCAL_ITEM_AUDIO) {
          content.push_back(nlohmann::json{{"type", "audio"}});
        }
      }
      content.push_back(nlohmann::json{{"type", "text"}, {"text", RenderMessageForPrompt(msg)}});
      entry["content"] = std::move(content);
    } else {
      for (const auto& part : msg.content) {
        if (part.view &&
            (part.view->type == FOUNDRY_LOCAL_ITEM_IMAGE || part.view->type == FOUNDRY_LOCAL_ITEM_AUDIO)) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                   "media input must belong to the final user message");
        }
      }
      // Non-final message: render visible text parts only via the canonical helper.
      entry["content"] = RenderMessageForPrompt(msg);
    }

    arr.push_back(std::move(entry));
  }

  return arr.dump();
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<OnnxChatGenerator> OnnxChatGenerator::Create(const std::vector<TranscriptMessage>& messages,
                                                             const SearchOptions& options,
                                                             GenAIModelInstance& model,
                                                             const ToolCallContext& tool_ctx,
                                                             bool use_full_context) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  return CreatePrepared(PrepareTextChatPrompt(messages, model, tool_ctx), options, model, tool_ctx, use_full_context);
}

std::unique_ptr<OnnxChatGenerator> OnnxChatGenerator::CreateWithMedia(
    const std::vector<MessageItem>& messages,
    const SearchOptions& options,
    GenAIModelInstance& model,
    const std::vector<const ImageItem*>& images,
    const std::vector<const AudioItem*>& audios,
    const ToolCallContext& tool_ctx,
    bool use_full_context) {
  return CreatePrepared(PrepareMediaChatPrompt(messages, model, images, audios, tool_ctx), options, model, tool_ctx,
                        use_full_context);
}

std::unique_ptr<OnnxChatGenerator> OnnxChatGenerator::CreatePrepared(PreparedChatPrompt prepared,
                                                                     const SearchOptions& options,
                                                                     GenAIModelInstance& model,
                                                                     const ToolCallContext& tool_ctx,
                                                                     bool use_full_context) {
  return CreateImpl(std::move(prepared), options, model, tool_ctx, use_full_context);
}

std::unique_ptr<OnnxChatGenerator> OnnxChatGenerator::CreateImpl(PreparedChatPrompt prepared,
                                                                 const SearchOptions& options,
                                                                 GenAIModelInstance& model,
                                                                 const ToolCallContext& tool_ctx,
                                                                 bool use_full_context) {
  const bool media_branch = prepared.HasMedia();
  std::unique_ptr<OgaSequences> sequences;
  if (!media_branch) {
    sequences = OgaSequences::Create();
    sequences->Append(prepared.token_ids.data(), prepared.token_ids.size());
  }

  auto gen_params = OgaGeneratorParams::Create(model.GetOgaModel());

  ApplySearchOptions(options, prepared.prompt_token_count, model.GetGenAIConfig(), *gen_params, model.EP(),
                     use_full_context, media_branch);

  auto reasoning_markers = ResolveReasoningMarkers(tool_ctx, model);
  const bool prompt_opens_reasoning =
      DetectPromptOpensReasoning(prepared.prompt, sequences.get(), reasoning_markers);
  ApplyGuidanceOptions(tool_ctx, prompt_opens_reasoning, *gen_params);

  std::unique_ptr<OgaGenerator> generator;
  try {
    generator = OgaGenerator::Create(model.GetOgaModel(), *gen_params);

    if (media_branch) {
      generator->SetInputs(*prepared.media_tensors);
    } else {
      generator->AppendTokenSequences(*sequences);
    }
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("failed to create generator: ") + e.what());
  }

  auto stream = model.GetPreprocessor().CreateTokenizerStream();

  // `std::make_unique` constructs inside the library helper, which does not have
  // access to this class's private constructor.
  return std::unique_ptr<OnnxChatGenerator>(new OnnxChatGenerator(std::move(gen_params),
                                                                  std::move(generator),
                                                                  std::move(stream),
                                                                  model,
                                                                  static_cast<int>(prepared.prompt_token_count),
                                                                  std::move(reasoning_markers),
                                                                  prompt_opens_reasoning,
                                                                  std::move(prepared.media_tensors)));
}

}  // namespace fl
