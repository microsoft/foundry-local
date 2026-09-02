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

namespace fl {

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

std::string BuildChatPrompt(const std::vector<MessageItem>& messages,
                            GenAIModelInstance& model,
                            const std::string& tools_json) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  // Build messages JSON array matching the format expected by the chat template.
  // Format: [{"role": "system", "content": "..."}, {"role": "user", "content": "..."}, ...]
  nlohmann::json messages_json = nlohmann::json::array();
  for (const auto& msg : messages) {
    messages_json.push_back({{"role", Utils::RoleToString(msg.role)}, {"content", RenderMessageForPrompt(msg)}});
  }

  std::string messages_str = messages_json.dump();
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
