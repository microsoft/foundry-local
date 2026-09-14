// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/chat_template.h"
#include "exception.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "utils.h"

#include <ort_genai.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <unordered_map>

namespace fl {

namespace chat_internal {

namespace {

constexpr std::string_view kAmbiguousPositionalResults =
    "positional tool results must immediately follow a multi-call assistant turn and contain each assistant call id "
    "exactly once";

[[noreturn]] void ThrowAmbiguousPositionalResults() {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, kAmbiguousPositionalResults);
}

}  // namespace

std::optional<size_t> FindUnmatchedPromptSuffix(std::span<const int32_t> resident_tokens,
                                                std::span<const int32_t> full_prompt) noexcept {
  if (resident_tokens.size() > full_prompt.size() ||
      !std::equal(resident_tokens.begin(), resident_tokens.end(), full_prompt.begin())) {
    return std::nullopt;
  }

  return resident_tokens.size();
}

std::vector<TranscriptMessage> ProjectPositionalToolResults(const std::vector<TranscriptMessage>& messages) {
  auto projected = messages;

  for (size_t assistant_index = 0; assistant_index < messages.size(); ++assistant_index) {
    const auto& assistant = messages[assistant_index];
    const auto calls = assistant.ToolCalls();
    if (assistant.role != FOUNDRY_LOCAL_ROLE_ASSISTANT || calls.size() < 2) {
      continue;
    }

    const auto results_begin = assistant_index + 1;
    auto results_end = results_begin;
    while (results_end < messages.size() && messages[results_end].role == FOUNDRY_LOCAL_ROLE_TOOL) {
      ++results_end;
    }

    std::unordered_map<std::string_view, size_t> call_positions;
    for (size_t call_position = 0; call_position < calls.size(); ++call_position) {
      if (!call_positions.emplace(calls[call_position]->call_id, call_position).second) {
        ThrowAmbiguousPositionalResults();
      }
    }

    // An outstanding parallel-call turn is valid only while none of its results appears later. Results for other
    // turns are unrelated and must not make this group ambiguous.
    if (results_end == results_begin) {
      const auto has_noncontiguous_result =
          std::any_of(messages.begin() + results_begin, messages.end(), [&](const auto& message) {
            return message.role == FOUNDRY_LOCAL_ROLE_TOOL &&
                   call_positions.contains(message.tool_call_id);
          });
      if (has_noncontiguous_result) {
        ThrowAmbiguousPositionalResults();
      }

      continue;
    }

    if (results_end - results_begin != calls.size()) {
      ThrowAmbiguousPositionalResults();
    }

    std::vector<bool> matched_calls(calls.size());
    for (auto result_index = results_begin; result_index < results_end; ++result_index) {
      const auto call_position = call_positions.find(messages[result_index].tool_call_id);
      if (call_position == call_positions.end() || matched_calls[call_position->second]) {
        ThrowAmbiguousPositionalResults();
      }

      projected[results_begin + call_position->second] = messages[result_index];
      matched_calls[call_position->second] = true;
    }

    assistant_index = results_end - 1;
  }

  return projected;
}

PreparedChatMessages PrepareChatMessages(std::vector<TranscriptMessage> messages,
                                         bool positional_tool_results) {
  if (positional_tool_results) {
    messages = ProjectPositionalToolResults(messages);
  }

  return PreparedChatMessages(std::move(messages));
}

}  // namespace chat_internal

namespace {

nlohmann::ordered_json BuildToolCallsJson(const TranscriptMessage& message) {
  auto tool_calls = nlohmann::ordered_json::array();

  for (const auto* call : message.ToolCalls()) {
    if (call->generated_encoding == GeneratedCallEncoding::kRawEnvelope) {
      continue;
    }

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

  for (const auto* call : message.ToolCalls()) {
    if (call->generated_encoding == GeneratedCallEncoding::kRawEnvelope) {
      entry["content"] = entry["content"].get<std::string>() + call->arguments;
    }
  }

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
  auto tool_calls = BuildToolCallsJson(message);
  if (!tool_calls.empty()) {
    entry["tool_calls"] = std::move(tool_calls);
  }
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

std::string chat_internal::BuildChatMessagesJsonForModel(const std::vector<TranscriptMessage>& messages,
                                                         bool positional_tool_results) {
  if (!positional_tool_results) {
    return BuildChatMessagesJson(messages);
  }

  return BuildChatMessagesJson(ProjectPositionalToolResults(messages));
}

std::string BuildChatPrompt(const std::vector<TranscriptMessage>& messages,
                            GenAIModelInstance& model,
                            const std::string& tools_json,
                            const std::string& template_kwargs_json) {
  return BuildChatPrompt(
      chat_internal::PrepareChatMessages(messages, model.HasPositionalToolResults()),
      model,
      tools_json,
      template_kwargs_json);
}

std::string BuildChatPrompt(const chat_internal::PreparedChatMessages& messages,
                            GenAIModelInstance& model,
                            const std::string& tools_json,
                            const std::string& template_kwargs_json) {
  if (messages.Empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  std::string messages_str = BuildChatMessagesJson(messages.Messages());
  const char* tools_ptr = tools_json.empty() ? nullptr : tools_json.c_str();
  const char* template_kwargs_ptr = template_kwargs_json.empty() ? nullptr : template_kwargs_json.c_str();

  // ApplyChatTemplate uses the model's built-in template (template_str=nullptr) and appends the assistant
  // turn prefix (add_generation_prompt=true).
  return model.GetPreprocessor().ApplyChatTemplateWithOptions(
      messages_str.c_str(), tools_ptr, template_kwargs_ptr, /*add_generation_prompt=*/true);
}

std::string BuildChatPrompt(const std::vector<MessageItem>& messages,
                            GenAIModelInstance& model,
                            const ToolCallContext& tool_ctx) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  nlohmann::json messages_json = nlohmann::json::array();
  for (const auto& message : messages) {
    messages_json.push_back({
        {"role", Utils::RoleToString(message.role)},
        {"content", RenderMessageForPrompt(message)},
    });
  }

  const char* tools = tool_ctx.tools_json.empty() ? nullptr : tool_ctx.tools_json.c_str();
  const char* template_kwargs =
      tool_ctx.template_kwargs_json.empty() ? nullptr : tool_ctx.template_kwargs_json.c_str();
  return model.GetPreprocessor().ApplyChatTemplateWithOptions(
      messages_json.dump().c_str(), tools, template_kwargs, /*add_generation_prompt=*/true);
}

std::unique_ptr<OgaSequences> EncodePrompt(const std::string& prompt,
                                           GenAIModelInstance& model) {
  return model.GetPreprocessor().Encode(prompt.c_str());
}

}  // namespace fl
