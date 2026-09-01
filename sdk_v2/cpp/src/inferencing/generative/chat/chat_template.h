// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/message_item.h"

#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct OgaSequences;

namespace fl {

class GenAIModelInstance;

/// Render a MessageItem's content as a plain string suitable for the chat template.
///
/// - Single-text messages return their text directly.
/// - Multi-part messages concatenate their TextItem parts. Non-text parts (images, audio) are skipped.
/// - REASONING-typed TextItem parts are always skipped: chain-of-thought content is preserved on the assistant
///   message in `history_` (useful for caller inspection) but must NOT be re-injected into the model's prompt.
///   Reasoning models (qwen3, etc.) are trained to never see prior reasoning in the conversation.
///
/// This is the single canonical "message → prompt string" entry point. Any new code path that builds prompts from
/// MessageItems should call this rather than iterating `msg.content` directly, so the REASONING-skip policy stays
/// in one place.
std::string RenderMessageForPrompt(const MessageItem& msg);

/// Serialize messages into the JSON shape consumed by the model chat template.
std::string BuildChatMessagesJson(const std::vector<MessageItem>& messages);

/// Build a chat prompt string from a list of messages.
/// Uses the tokenizer's built-in chat template (via GenAIModelInstance::ApplyChatTemplate).
///
/// @param messages       Ordered list of chat messages (system, user, assistant, tool, etc.)
/// @param model          Model instance whose shared tokenizer renders the template (thread-safe)
/// @param tools_json     Optional JSON string describing available tools. Pass empty string for none.
/// @returns The formatted prompt string ready for tokenization
std::string BuildChatPrompt(const std::vector<MessageItem>& messages,
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
