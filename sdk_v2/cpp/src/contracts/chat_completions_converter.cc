// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/chat_completions_converter.h"

#include "contracts/tool_definitions.h"
#include "inferencing/generative/chat/stop_strings.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"
#include "utils.h"

#include <algorithm>
#include <random>
#include <sstream>
#include <utility>

namespace fl {
namespace chat_completions {

namespace {

/// Core definition for one declared tool, whichever kind it is. A custom tool contributes no
/// schema: the registry synthesizes it, which is what keeps the raw payload out of function
/// argument handling.
ToolDefinition ToCoreDefinition(const ChatCompletionTool& tool) {
  if (tool.IsCustom()) {
    return tools::MakeCustomTool(tool.custom->name, tool.custom->description.value_or(""),
                                 tool.custom->description.has_value());
  }

  return tools::MakeFunctionTool(tool.function.name, tool.function.description.value_or(""),
                                 tool.function.parameters.has_value() ? tool.function.parameters->dump()
                                                                      : std::string{},
                                 tool.function.description.has_value(), tool.function.parameters.has_value(),
                                 tool.function.strict);
}

ToolKind ForcedChoiceKind(const ChatCompletionToolChoice& choice) {
  return choice.kind == ChatCompletionToolChoice::Kind::kCustom ? ToolKind::kCustom : ToolKind::kFunction;
}

}  // namespace

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
    //
    // A custom call contributes its raw text payload, not JSON arguments. The item carries those bytes unchanged and
    // the transcript rewraps them as {"input": ...} for template projection only, using the kind the session's
    // definition snapshot records for the name — so nothing here has to guess what kind a call was.
    for (const auto& call : msg.tool_calls) {
      const auto kind = call.IsCustom() ? ToolKind::kCustom : ToolKind::kFunction;
      session_request.AddOwnedItem(std::make_unique<ToolCallItem>(
          call.id, call.Name(), call.Payload(), /*replayed_from_store=*/false, kind, kind));
    }
  }
}

std::vector<ToolDefinition> ExtractToolDefinitions(const ChatCompletionRequest& req, Request& session_request) {
  std::vector<ToolDefinition> definitions;

  if (req.tools.has_value()) {
    definitions.reserve(req.tools->size());
    for (const auto& tool : *req.tools) {
      definitions.push_back(ToCoreDefinition(tool));
    }
  }

  // tool_choice → controls text_output / tool_output in ChatSession. Unrepresentable choices were
  // already rejected while the request was read, so only valid ones reach here.
  if (req.tool_choice.has_value()) {
    const auto& choice = *req.tool_choice;
    session_request.options["tool_choice"] = choice.ModeString();

    if (choice.IsForced()) {
      tools::NarrowToForcedTool(definitions, choice.name, ForcedChoiceKind(choice));
    }
  }

  if (definitions.empty() && req.tool_choice.has_value() &&
      req.tool_choice->kind == ChatCompletionToolChoice::Kind::kRequired) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "tool_choice 'required' requires at least one declared tool");
  }

  return definitions;
}

ChatCompletionToolCall MakeToolCall(std::string call_id, std::string name, std::string payload, ToolKind kind) {
  if (kind == ToolKind::kCustom) {
    return ChatCompletionToolCall::MakeCustom(std::move(call_id), std::move(name), std::move(payload));
  }

  return ChatCompletionToolCall::MakeFunction(std::move(call_id), std::move(name), std::move(payload));
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
      tool_calls.push_back(MakeToolCall(tc_item.call_id, tc_item.name, tc_item.arguments, tc_item.kind));
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
