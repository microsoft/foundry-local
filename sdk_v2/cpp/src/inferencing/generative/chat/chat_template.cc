// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/chat_template.h"
#include "exception.h"
#include "inferencing/generative/genai_model_instance.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "utils.h"

#include <ort_genai.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace fl {

namespace chat_internal {

std::optional<size_t> FindUnmatchedPromptSuffix(std::span<const int32_t> resident_tokens,
                                                std::span<const int32_t> full_prompt) noexcept {
  if (resident_tokens.size() > full_prompt.size() ||
      !std::equal(resident_tokens.begin(), resident_tokens.end(), full_prompt.begin())) {
    return std::nullopt;
  }

  return resident_tokens.size();
}

}  // namespace chat_internal

namespace {

nlohmann::ordered_json BuildToolCallsJson(const TranscriptMessage& message) {
  auto tool_calls = nlohmann::ordered_json::array();

  for (const auto* call : message.ToolCalls()) {
    nlohmann::ordered_json entry;
    entry["id"] = call->call_id;
    entry["type"] = "function";
    // Always the normalized object: the transcript guarantees it, so projecting a committed conversation cannot fail.
    entry["function"] = nlohmann::ordered_json{{"name", call->name}, {"arguments", call->normalized_arguments}};
    tool_calls.push_back(std::move(entry));
  }

  return tool_calls;
}

nlohmann::ordered_json BuildMessageJson(const TranscriptMessage& message) {
  nlohmann::ordered_json entry;
  entry["role"] = Utils::RoleToString(message.role);
  entry["content"] = message.VisibleText();

  if (!message.name.empty()) {
    entry["name"] = message.name;
  }

  if (message.role == FOUNDRY_LOCAL_ROLE_TOOL) {
    if (!message.tool_call_id.empty()) {
      entry["tool_call_id"] = message.tool_call_id;
    }

    return entry;
  }

  if (!message.HasToolCalls()) {
    return entry;
  }

  // Reasoning is never projected back into a prompt, not even alongside the calls it produced. It is the model's
  // private scratchpad: it is typed, stored, and surfaced to the caller, but a conversation replayed from storage
  // cannot reproduce it, so replaying it here would make a warm session and a rebuilt one send different prompts.
  entry["tool_calls"] = BuildToolCallsJson(message);
  return entry;
}

}  // namespace

std::string RenderMessageForPrompt(const MessageItem& msg) {
  if (msg.IsSimpleText()) {
    const auto& part = static_cast<const TextItem&>(*msg.content.front().view);
    if (part.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
      return {};
    }
    return part.text;
  }

  std::string text;
  for (const auto& part : msg.content) {
    if (!part.view || part.view->type != FOUNDRY_LOCAL_ITEM_TEXT) {
      continue;
    }

    const auto& ti = static_cast<const TextItem&>(*part.view);
    if (ti.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
      continue;
    }

    // No separator between text parts: matches OpenAI content-array semantics where parts are literal fragments and
    // the caller owns any whitespace.
    text += ti.text;
  }

  return text;
}

std::string BuildChatMessagesJson(const std::vector<TranscriptMessage>& messages) {
  auto messages_json = nlohmann::ordered_json::array();
  for (const auto& message : messages) {
    messages_json.push_back(BuildMessageJson(message));
  }

  return messages_json.dump();
}

std::string BuildChatPrompt(const std::vector<TranscriptMessage>& messages,
                            GenAIModelInstance& model,
                            const std::string& tools_json) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  std::string messages_str = BuildChatMessagesJson(messages);
  const char* tools_ptr = tools_json.empty() ? nullptr : tools_json.c_str();

  // ApplyChatTemplate uses the model's built-in template (template_str=nullptr) and appends the assistant
  // turn prefix (add_generation_prompt=true).
  return model.GetPreprocessor().ApplyChatTemplate(messages_str.c_str(), tools_ptr, /*add_generation_prompt=*/true);
}

std::unique_ptr<OgaSequences> EncodePrompt(const std::string& prompt,
                                           GenAIModelInstance& model) {
  return model.GetPreprocessor().Encode(prompt.c_str());
}

}  // namespace fl
