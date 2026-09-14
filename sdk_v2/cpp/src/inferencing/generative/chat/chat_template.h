// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_transcript.h"
#include "items/message_item.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Forward declarations
struct OgaSequences;

namespace fl {

class GenAIModelInstance;
struct ToolCallContext;

namespace chat_internal {

/// A complete transcript after its model-specific message projection has been validated and applied.
///
/// Keeping this as a distinct value prevents retained backends from accidentally projecting again after the
/// session has already crossed its cache-mutation boundary.
class PreparedChatMessages {
 public:
  const std::vector<TranscriptMessage>& Messages() const noexcept { return messages_; }
  bool Empty() const noexcept { return messages_.empty(); }

 private:
  friend PreparedChatMessages PrepareChatMessages(std::vector<TranscriptMessage> messages,
                                                  bool positional_tool_results);

  explicit PreparedChatMessages(std::vector<TranscriptMessage> messages) : messages_(std::move(messages)) {}

  std::vector<TranscriptMessage> messages_;
};

/// Return the first unmatched full-prompt token when the resident sequence is an exact prefix.
std::optional<size_t> FindUnmatchedPromptSuffix(std::span<const int32_t> resident_tokens,
                                                std::span<const int32_t> full_prompt) noexcept;

/// Copy a complete logical conversation and reorder each multi-call assistant turn's immediately following tool
/// results into call order.
///
/// Positional templates discard result IDs, so a multi-call exchange is renderable only when its contiguous result
/// run is an exact bijection to the calls. This projection is deliberately separate from transcript validation:
/// canonical messages and public IDs retain arrival order, and non-positional models never invoke it.
///
/// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT when a multi-call result group is ambiguous.
std::vector<TranscriptMessage> ProjectPositionalToolResults(const std::vector<TranscriptMessage>& messages);

/// Validate and apply the model-specific projection to a complete logical conversation.
PreparedChatMessages PrepareChatMessages(std::vector<TranscriptMessage> messages,
                                         bool positional_tool_results);

/// Select the model-capability-specific projection before rendering. The false branch is the canonical projection
/// verbatim so models whose templates preserve result IDs remain byte-for-byte unchanged.
std::string BuildChatMessagesJsonForModel(const std::vector<TranscriptMessage>& messages,
                                          bool positional_tool_results);

}  // namespace chat_internal

/// Render a MessageItem's content as a plain string suitable for the chat template.
///
/// - Single-text messages return their text directly.
/// - Multi-part messages concatenate their TextItem parts. Non-text parts (images, audio) are skipped.
/// - REASONING-typed TextItem parts are always skipped: chain-of-thought content must NOT be re-injected into the
///   model's prompt as visible content.
///
/// Used by the media path, which feeds MessageItems (and their image / audio parts) straight to the template. The
/// text path builds its template input from the authoritative transcript instead — see BuildChatMessagesJson.
std::string RenderMessageForPrompt(const MessageItem& msg);

/// Project transcript messages into the JSON array handed to ApplyChatTemplate.
///
/// The projection is provider-neutral and canonicalizes each message into the shape chat templates expect, while the
/// transcript keeps the authoritative event order:
///   - `content` — concatenated visible text. Reasoning is never projected, on any message: it is the model's
///     private scratchpad, and a conversation rebuilt from storage cannot reproduce it, so replaying it would make
///     a live session and a rebuilt one send different prompts. Always present, possibly empty.
///   - `tool_calls` — OpenAI-shaped array (`id` / `type` / `function.name` / `function.arguments`) for assistant
///     messages that issued calls. Arguments are the transcript's normalized object form, so a committed
///     conversation always projects; the raw bytes stay on the transcript and the response.
///   - `tool_call_id` — set on role="tool" messages so the template can correlate a result with its call.
///   - `name` — emitted only when the message carries a participant name.
///
/// The projection is total: every committed transcript renders.
///
/// It is also order-faithful, but only because the transcript refuses to hold the one shape this schema cannot
/// express. `content` plus a `tool_calls` array can say "this text, then these calls"; it has no way to say "text,
/// then a call, then more text". Rather than silently reorder such a turn, ChatTranscript rejects it and generation
/// stops at the call — see ValidateRenderableTurn. Within that invariant, what the template receives is the order
/// the events actually happened in.
std::string BuildChatMessagesJson(const std::vector<TranscriptMessage>& messages);

/// Build a chat prompt string from the transcript messages of a conversation.
/// Uses the tokenizer's built-in chat template (via GenAIModelInstance::ApplyChatTemplate).
///
/// @param messages       Ordered transcript messages (system, user, assistant, tool, ...)
/// @param model          Model instance whose shared tokenizer renders the template (thread-safe)
/// @param tools_json     Optional JSON string describing available tools. Pass empty string for none.
/// @param template_kwargs_json Optional JSON object containing additional typed template context values.
/// @returns The formatted prompt string ready for tokenization
std::string BuildChatPrompt(const std::vector<TranscriptMessage>& messages,
                            GenAIModelInstance& model,
                            const std::string& tools_json = "",
                            const std::string& template_kwargs_json = "");

/// Build a chat prompt from the complete retained tool/template context.
std::string BuildChatPrompt(const std::vector<MessageItem>& messages,
                            GenAIModelInstance& model,
                            const ToolCallContext& tool_ctx);

/// Build a prompt from a projection that was already validated at the caller's state-mutation boundary.
std::string BuildChatPrompt(const chat_internal::PreparedChatMessages& messages,
                            GenAIModelInstance& model,
                            const std::string& tools_json = "",
                            const std::string& template_kwargs_json = "");

/// Encode a prompt string into token sequences using the model's shared tokenizer (thread-safe).
/// Returns a unique_ptr to OgaSequences. Caller takes ownership.
///
/// @param prompt     The formatted prompt string (from BuildChatPrompt)
/// @param model      Model instance whose shared tokenizer encodes the prompt
/// @returns Encoded token sequences
std::unique_ptr<OgaSequences> EncodePrompt(const std::string& prompt,
                                           GenAIModelInstance& model);

}  // namespace fl
