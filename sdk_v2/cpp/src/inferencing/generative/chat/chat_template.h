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

namespace chat_internal {

/// Return the first unmatched full-prompt token when the resident sequence is an exact prefix.
std::optional<size_t> FindUnmatchedPromptSuffix(std::span<const int32_t> resident_tokens,
                                                std::span<const int32_t> full_prompt) noexcept;

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
/// @returns The formatted prompt string ready for tokenization
std::string BuildChatPrompt(const std::vector<TranscriptMessage>& messages,
                            GenAIModelInstance& model,
                            const std::string& tools_json = "");

/// Encode a prompt string into token sequences using the model's shared tokenizer (thread-safe).
/// Returns a unique_ptr to OgaSequences. Caller takes ownership.
///
/// @param prompt     The formatted prompt string (from BuildChatPrompt)
/// @param model      Model instance whose shared tokenizer encodes the prompt
/// @returns Encoded token sequences
std::unique_ptr<OgaSequences> EncodePrompt(const std::string& prompt,
                                           GenAIModelInstance& model);

}  // namespace fl
