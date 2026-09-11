// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/chat_completions_converter.h"

#include "inferencing/generative/chat/stop_strings.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"
#include "utils.h"

#include <random>
#include <sstream>

namespace fl {
namespace chat_completions {

std::string GenerateCompletionId() {
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

  std::ostringstream ss;
  ss << "chatcmpl-" << std::hex << dist(rng) << dist(rng);
  return ss.str();
}

void ApplyCatalogDefaults(ChatCompletionRequest& req, const KeyValuePairs& model_settings) {
  if (model_settings.empty()) {
    return;
  }

  auto apply_default_float = [&](const char* key, std::optional<float>& field) {
    if (!field.has_value()) {
      const char* val = model_settings.Find(key);
      if (val) {
        field = std::stof(val);
      }
    }
  };

  auto apply_default_int = [&](const char* key, std::optional<int>& field) {
    if (!field.has_value()) {
      const char* val = model_settings.Find(key);
      if (val) {
        field = std::stoi(val);
      }
    }
  };

  apply_default_float("temperature", req.temperature);
  apply_default_float("top_p", req.top_p);
  apply_default_int("max_tokens", req.max_tokens);

  // top_k and random_seed go through metadata (matches C# behavior)
  if (!req.metadata.has_value()) {
    req.metadata.emplace();
  }

  auto apply_metadata = [&](const char* key) {
    const char* val = model_settings.Find(key);
    if (val && req.metadata->find(key) == req.metadata->end()) {
      (*req.metadata)[key] = val;
    }
  };

  apply_metadata("top_k");
  apply_metadata("random_seed");
}

std::string MapFinishReason(flFinishReason reason) {
  switch (reason) {
    case FOUNDRY_LOCAL_FINISH_STOP:
      return "stop";
    case FOUNDRY_LOCAL_FINISH_LENGTH:
      return "length";
    case FOUNDRY_LOCAL_FINISH_TOOL_CALLS:
      return "tool_calls";
    default:
      return "stop";
  }
}

void BuildRequestItems(const ChatCompletionRequest& req, Request& session_request) {
  for (const auto& msg : req.messages) {
    auto role = Utils::StringToRole(msg.role);

    if (role == FOUNDRY_LOCAL_ROLE_TOOL) {
      // A tool result may legitimately be an empty string; the call ID is what correlates it with its call.
      session_request.AddOwnedItem(
          std::make_unique<ToolResultItem>(msg.tool_call_id.value_or(""), msg.content.value_or("")));
      continue;
    }

    const std::string content = msg.content.value_or("");
    if (!content.empty()) {
      session_request.AddOwnedItem(std::make_unique<MessageItem>(role, content, msg.name.value_or("")));
    }

    if (role != FOUNDRY_LOCAL_ROLE_ASSISTANT) {
      continue;
    }

    // A reasoning-only response has no model-visible text to replay, but its assistant role still separates the
    // messages on either side. Carry that boundary as an empty visible text part; reasoning itself remains private.
    if (content.empty() && msg.reasoning_content.has_value() && !msg.reasoning_content->empty()) {
      auto boundary = std::make_unique<MessageItem>();
      boundary->role = role;
      boundary->name = msg.name.value_or("");
      boundary->content.push_back(MessagePart::Own(std::make_unique<TextItem>("")));
      session_request.AddOwnedItem(std::move(boundary));
    }

    // Assistant messages that issue tool calls usually have null content. When such a message also carries a
    // participant name, emit a content-free MessageItem to carry it: the transcript folds the calls below into that
    // message, so the name reaches the template without fabricating a text part the caller never sent.
    if (content.empty() && (!msg.reasoning_content.has_value() || msg.reasoning_content->empty()) &&
        !msg.tool_calls.empty() && msg.name.has_value() && !msg.name->empty()) {
      auto named = std::make_unique<MessageItem>();
      named->role = role;
      named->name = *msg.name;
      session_request.AddOwnedItem(std::move(named));
    }

    // Emit the calls as items directly after any visible text so the transcript keeps them on one assistant message.
    for (const auto& call : msg.tool_calls) {
      session_request.AddOwnedItem(
          std::make_unique<ToolCallItem>(call.id, call.function.name, call.function.arguments));
    }
  }
}

std::string ExtractToolDefinitions(ChatCompletionRequest& req, Request& session_request) {
  std::string tools_json;

  // it's cheaper to re-serialize than to re-parse the full request to get the tools JSON.
  if (req.tools.has_value() && !req.tools->empty()) {
    tools_json = nlohmann::json(*req.tools).dump();
  }

  // Extract tool_choice → controls text_output / tool_output in ChatSession
  if (req.tool_choice.has_value()) {
    const auto& tc = *req.tool_choice;

    if (tc.is_string()) {
      session_request.options["tool_choice"] = tc.get<std::string>();
    } else if (tc.is_object() && tc.contains("type") && tc["type"] == "function") {
      // {"type": "function", "function": {"name": "..."}} → filter to named function + "required"
      session_request.options["tool_choice"] = "required";

      // Filter tools to only the specified function (matches C# SetToolChoice behavior)
      if (tc.contains("function") && tc["function"].contains("name")) {
        std::string target_name = tc["function"]["name"].get<std::string>();

        if (req.tools.has_value()) {
          std::vector<ChatCompletionTool> filtered;
          for (const auto& tool : *req.tools) {
            if (tool.function.name == target_name) {
              filtered.push_back(tool);
            }
          }

          if (!filtered.empty()) {
            tools_json = nlohmann::json(filtered).dump();
          }
        }
      }
    }
  }

  return tools_json;
}

void MapRequestParameters(const ChatCompletionRequest& req, Request& session_request) {
  auto set_float_param = [&](const std::optional<float>& val, const char* key) {
    if (val.has_value()) {
      session_request.options[key] = std::to_string(*val);
    }
  };

  set_float_param(req.temperature, "temperature");
  set_float_param(req.top_p, "top_p");
  if (req.frequency_penalty.value_or(0.0f) != 0.0f) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "nonzero frequency_penalty is not supported; ORT repetition_penalty has different semantics");
  }
  if (req.presence_penalty.value_or(0.0f) != 0.0f) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "nonzero presence_penalty is not supported; ORT diversity_penalty has different semantics");
  }

  if (req.seed.has_value()) {
    session_request.options["seed"] = std::to_string(*req.seed);
  }

  // max_completion_tokens (current) and max_tokens (deprecated) → max_output_tokens
  if (req.max_completion_tokens.has_value()) {
    session_request.options["max_output_tokens"] = std::to_string(*req.max_completion_tokens);
  } else if (req.max_tokens.has_value()) {
    session_request.options["max_output_tokens"] = std::to_string(*req.max_tokens);
  }

  // Extract metadata extensions (matching C# GetTopK/GetRandomSeed)
  if (req.metadata.has_value()) {
    const auto& meta = *req.metadata;

    auto top_k_it = meta.find("top_k");
    if (top_k_it != meta.end() && !top_k_it->second.empty()) {
      session_request.options["top_k"] = top_k_it->second;
    }

    auto seed_it = meta.find("random_seed");
    if (seed_it != meta.end() && !seed_it->second.empty()) {
      session_request.options["seed"] = seed_it->second;
    }
  }
}

void MapGuidance(const ChatCompletionRequest& req, Request& session_request) {
  if (!req.response_format.has_value() || !req.response_format->is_object()) {
    return;
  }

  const auto& rf = *req.response_format;
  std::string rf_type = rf.value("type", "");

  if (rf_type == "lark_grammar") {
    session_request.options["guidance_type"] = "lark_grammar";
    if (rf.contains("lark_grammar") && rf["lark_grammar"].is_string()) {
      session_request.options["guidance_data"] = rf["lark_grammar"].get<std::string>();
    }
  } else if (rf_type == "json_schema") {
    session_request.options["guidance_type"] = "json_schema";
    if (rf.contains("json_schema")) {
      session_request.options["guidance_data"] = rf["json_schema"].dump();
    }
  } else if (rf_type == "json_object") {
    session_request.options["guidance_type"] = "json_schema";
  } else if (rf_type == "text") {
    session_request.options["tool_choice"] = "none";
  }
}

void MapStopSequences(const ChatCompletionRequest& req, Request& session_request) {
  if (!req.stop.has_value()) {
    return;
  }

  StoreStopStringsOption(NormalizeOpenAiStopStrings(*req.stop), session_request.options);
}

ChatCompletionResponse BuildResponse(const Response& response,
                                     const std::string& completion_id,
                                     int64_t created,
                                     const std::string& model_name) {
  // Extract assistant message and tool calls from response items
  std::string response_text;
  std::string reasoning_text;
  std::vector<ChatCompletionToolCall> tool_calls;

  for (const auto& item : response.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      auto& msg_item = static_cast<MessageItem&>(*item);
      if (msg_item.role == FOUNDRY_LOCAL_ROLE_ASSISTANT) {
        // Chat Completions exposes only visible text. Inspect the TextItem type even for a one-part message because a
        // generation truncated inside a reasoning block produces exactly one REASONING part.
        for (const auto& part : msg_item.content) {
          if (!part.view || part.view->type != FOUNDRY_LOCAL_ITEM_TEXT) {
            continue;
          }
          const auto& ti = static_cast<const TextItem&>(*part.view);
          if (ti.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
            response_text += ti.text;
          } else if (ti.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
            reasoning_text += ti.text;
          }
        }
      }
    } else if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
      auto& tc_item = static_cast<ToolCallItem&>(*item);
      ChatCompletionToolCall tc;
      tc.id = tc_item.call_id;
      tc.type = "function";
      tc.function.name = tc_item.name;
      tc.function.arguments = tc_item.arguments;
      tool_calls.push_back(std::move(tc));
    }
  }

  bool has_tool_calls = response.finish_reason == FOUNDRY_LOCAL_FINISH_TOOL_CALLS;

  ChatCompletionChoice choice;
  choice.index = 0;
  choice.finish_reason = MapFinishReason(response.finish_reason);
  choice.message.content = response_text;

  if (!reasoning_text.empty()) {
    choice.message.reasoning_content = std::move(reasoning_text);
  }

  if (has_tool_calls) {
    choice.message.tool_calls = std::move(tool_calls);
  }

  ChatCompletionResponse result;
  result.id = completion_id;
  result.created = created;
  result.model = model_name;
  result.choices.push_back(std::move(choice));

  result.usage.prompt_tokens = static_cast<int>(response.usage.prompt_tokens);
  result.usage.completion_tokens = static_cast<int>(response.usage.completion_tokens);
  result.usage.total_tokens = static_cast<int>(response.usage.total_tokens);
  result.usage.completion_tokens_details.reasoning_tokens =
      static_cast<int>(response.usage.reasoning_tokens);

  return result;
}

std::string FormatStreamingChunk(const std::string& content,
                                 const std::string& completion_id,
                                 int64_t created,
                                 const std::string& model_name) {
  ChatCompletionChunk chunk;
  chunk.id = completion_id;
  chunk.created = created;
  chunk.model = model_name;

  ChatCompletionChunkChoice choice;
  choice.delta.content = content;
  chunk.choices.push_back(std::move(choice));

  return nlohmann::json(chunk).dump();
}

std::string FormatReasoningStreamingChunk(const std::string& reasoning_content,
                                          const std::string& completion_id,
                                          int64_t created,
                                          const std::string& model_name) {
  ChatCompletionChunk chunk;
  chunk.id = completion_id;
  chunk.created = created;
  chunk.model = model_name;

  ChatCompletionChunkChoice choice;
  choice.delta.reasoning_content = reasoning_content;
  chunk.choices.push_back(std::move(choice));

  return nlohmann::json(chunk).dump();
}

std::string FormatInitialStreamingChunk(const std::string& completion_id,
                                        int64_t created,
                                        const std::string& model_name) {
  ChatCompletionChunk chunk;
  chunk.id = completion_id;
  chunk.created = created;
  chunk.model = model_name;

  ChatCompletionChunkChoice choice;
  choice.delta.role = "assistant";
  choice.delta.content = "";
  chunk.choices.push_back(std::move(choice));

  return nlohmann::json(chunk).dump();
}

std::string FormatToolCallStreamingChunk(const std::vector<ChatCompletionToolCall>& tool_calls,
                                         const std::string& completion_id,
                                         int64_t created,
                                         const std::string& model_name) {
  ChatCompletionChunk chunk;
  chunk.id = completion_id;
  chunk.created = created;
  chunk.model = model_name;

  ChatCompletionChunkChoice choice;
  choice.delta.tool_calls = tool_calls;
  chunk.choices.push_back(std::move(choice));

  return nlohmann::json(chunk).dump();
}

std::string FormatFinalStreamingChunk(flFinishReason reason,
                                      const std::string& completion_id,
                                      int64_t created,
                                      const std::string& model_name) {
  ChatCompletionChunk chunk;
  chunk.id = completion_id;
  chunk.created = created;
  chunk.model = model_name;

  ChatCompletionChunkChoice choice;
  choice.finish_reason = MapFinishReason(reason);
  chunk.choices.push_back(std::move(choice));

  return nlohmann::json(chunk).dump();
}

}  // namespace chat_completions
}  // namespace fl
