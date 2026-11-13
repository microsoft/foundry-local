// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/prepared_chat_prompt.h"
#include "exception.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"
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

nlohmann::ordered_json BuildMessageJson(const TranscriptMessage& message, bool preserve_reasoning_history) {
  nlohmann::ordered_json entry;
  entry["role"] = Utils::RoleToString(message.role);
  entry["content"] = message.VisibleText();

  if (preserve_reasoning_history && message.role == FOUNDRY_LOCAL_ROLE_ASSISTANT) {
    entry["reasoning_content"] = message.ReasoningText();
  }

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

std::string BuildChatMessagesJson(const std::vector<TranscriptMessage>& messages,
                                  bool preserve_reasoning_history) {
  auto messages_json = nlohmann::ordered_json::array();
  for (const auto& message : messages) {
    messages_json.push_back(BuildMessageJson(message, preserve_reasoning_history));
  }

  return messages_json.dump();
}

std::string BuildCanonicalToolMessagesJson(const std::vector<TranscriptMessage>& messages,
                                           const ToolCallContext& tool_ctx,
                                           const std::function<std::string(std::string)>& protect_literal) {
  auto projected = nlohmann::ordered_json::parse(
      BuildChatMessagesJson(messages, tool_ctx.supports_reasoning_history));
  for (auto& message : projected) {
    if (protect_literal) {
      message["content"] = protect_literal(message.at("content").get<std::string>());
      if (auto reasoning = message.find("reasoning_content"); reasoning != message.end()) {
        *reasoning = protect_literal(reasoning->get<std::string>());
      }
    }

    if (tool_ctx.UsesAssistantTextFraming() && message.at("role") == "assistant" &&
        !message.at("content").get_ref<const std::string&>().empty()) {
      message["content"] = std::string(ToolCallContext::kAssistantTextPrefix) +
                           message.at("content").get<std::string>();
    }

    const auto calls = message.find("tool_calls");
    if (calls == message.end()) {
      continue;
    }

    auto batch = nlohmann::ordered_json::array();
    for (const auto& call : *calls) {
      const auto& function = call.at("function");
      batch.push_back({{"name", function.at("name")}, {"parameters", function.at("arguments")}});
    }

    // Template implementations inspect literal reasoning tags in content. JSON escapes protect argument data
    // without changing decoded strings or letting the template re-encode calls into its native XML format.
    const auto encoded = batch.dump();
    std::string safe;
    safe.reserve(encoded.size());
    for (const auto byte : encoded) {
      if (byte == '<') {
        safe += "\\u003c";
      } else if (byte == '>') {
        safe += "\\u003e";
      } else {
        safe += byte;
      }
    }

    message["content"] = message.at("content").get<std::string>() +
                         tool_ctx.tool_call_start + (protect_literal ? protect_literal(std::move(safe)) : safe) +
                         tool_ctx.tool_call_end;
    message.erase("tool_calls");
  }

  auto instructions =
      "Tool calling protocol for this conversation:\nAvailable tools:\n" +
      (protect_literal ? protect_literal(tool_ctx.tools_json) : tool_ctx.tools_json);
  if (tool_ctx.UsesAssistantTextFraming()) {
    instructions =
        "Do not forget the required MESSAGE framing after reasoning. All natural-language answers must begin "
        "with MESSAGE followed by a newline. A tool call is not a substitute for an answer.\n\n" +
        instructions;
  }

  if (tool_ctx.fenced_tool_call_output) {
    instructions +=
        "\nYou must call a declared tool for this turn. Emit one JSON code fence containing an array of objects "
        "with exactly the keys \"name\" and \"parameters\". \"name\" must be a declared tool name and "
        "\"parameters\" must be its argument object. Do not emit text before or after the fence. "
        "Use {} for parameterless tools. Each argument object must satisfy its tool's complete JSON Schema. "
        "Preserve argument values exactly. After the closing fence, stop and wait for the external tool results. "
        "Use this exact shape: ```json\n"
        "[{\"name\":\"tool_name\",\"parameters\":{\"argument\":\"value\"}}]\n"
        "```. Replace the example names and values with a declared tool and its requested arguments.";
    if (tool_ctx.forced_tool) {
      instructions += " Call exactly the tool \"" + tool_ctx.forced_tool->name + "\".";
    }
  } else {
    instructions +=
        "\nTo call tools, emit " + tool_ctx.tool_call_start +
        " followed by a JSON array of objects with exactly the keys \"name\" and \"parameters\", then " +
        tool_ctx.tool_call_end +
        ". \"name\" must be a declared tool name and \"parameters\" must be its argument object. "
        "Use the dedicated tool boundary tokens, not a textual description or substitute characters. "
        "Emit the call block itself, without quoting it or using Markdown. "
        "Put all calls for this turn in one array inside one pair of tool boundaries. "
        "After the closing boundary, stop and wait for the external tool results. "
        "Use {} for parameterless tools. Do not use XML function or parameter tags. "
        "Each argument object must satisfy its tool's complete JSON Schema. "
        "Properties may appear in any order; omit unused optional properties. "
        "Preserve argument values exactly. JSON escaping, including Unicode escapes for literal angle brackets "
        "in strings, preserves the decoded value and is not substitution. "
        "If no tool is needed, answer normally.";
  }
  if (const auto* raw = tool_ctx.ActiveRawEnvelope()) {
    const auto raw_instructions =
        "\nThe custom tool \"" + raw->tool_name +
        "\" also accepts a raw envelope instead of a JSON call. "
        "To call this tool in raw form, start with \"" +
        raw->start_marker +
        "\" on its own line, then the exact tool payload, and finish with \"" + raw->end_marker +
        "\" on its own line. Include both marker lines in the raw input. "
        "Do not wrap the raw envelope in tool boundary tokens, JSON, or Markdown. "
        "After the closing marker, stop and wait for the external tool result. "
        "This raw format applies only to this custom tool; ordinary functions must use the JSON tool protocol above. "
        "For an ordinary function, always put the call object inside an outer JSON array, even for a single call. "
        "The content between tool boundaries must start with [ and end with ]. Never emit a bare call object.";
    instructions += protect_literal ? protect_literal(raw_instructions) : raw_instructions;
  }

  if (tool_ctx.UsesAssistantTextFraming()) {
    instructions +=
        "\nFor ordinary assistant text, including a brief preamble, first emit MESSAGE followed by a newline. "
        "This framing line is not message content. For a direct tool call, start with its tool boundary instead. "
        "Never return an unframed description, JSON array, or textual imitation of a tool call.";
  }

  if (!projected.empty() && projected.front().at("role") == "system") {
    projected.front()["content"] = projected.front().at("content").get<std::string>() + "\n\n" + instructions;
  } else {
    nlohmann::ordered_json system = {{"role", "system"}, {"content", instructions}};
    projected.insert(projected.begin(), std::move(system));
  }

  return projected.dump();
}

HarmonyPromptProjection BuildHarmonyPromptProjection(
    const std::vector<TranscriptMessage>& messages, const ToolCallContext& context,
    const std::function<std::string(std::string)>& protect_literal) {
  using Json = nlohmann::ordered_json;
  auto projected = Json::array();
  const auto validate_tool_name = [](std::string_view name) {
    if (name.empty() || name.find_first_of(" \t\r\n\v\f\x01\x02") != std::string_view::npos) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "Harmony tool names must not contain header separators");
    }
  };
  std::unordered_map<std::string, std::string> call_names;
  for (const auto& message : messages) {
    if (!projected.empty() &&
        (message.role == FOUNDRY_LOCAL_ROLE_SYSTEM || message.role == FOUNDRY_LOCAL_ROLE_DEVELOPER)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "Harmony requires system or developer instructions to be the first message");
    }
    Json entry{{"role", Utils::RoleToString(message.role)}, {"content", protect_literal(message.VisibleText())}};
    if (message.role == FOUNDRY_LOCAL_ROLE_ASSISTANT) {
      entry["thinking"] = protect_literal(message.ReasoningText());
      const auto history_calls = message.ToolCalls();
      if (history_calls.size() > 1) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 "Native Harmony supports only sequential tool-call history");
      }

      auto calls = Json::array();
      for (const auto* call : history_calls) {
        validate_tool_name(call->name);
        if (!call_names.emplace(call->call_id, call->name).second) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Duplicate Harmony history call ID");
        }
        const auto raw_body = call->kind == ToolKind::kCustom &&
                              (call->generated_encoding == GeneratedCallEncoding::kNativeRaw ||
                               (call->generated_encoding == GeneratedCallEncoding::kStructured &&
                                !call->has_recorded_encoding &&
                                context.UsesNativeRawBody(call->name)));
        const auto arguments = raw_body || (call->kind == ToolKind::kFunction && !call->arguments.empty())
                                   ? call->arguments
                                   : call->normalized_arguments.dump();
        calls.push_back(
            {{"id", call->call_id},
             {"type", "function"},
             {"function",
              {{"name", protect_literal(call->name)},
               {"arguments", Json::object()},
               {"arguments_json", protect_literal(arguments)},
               {"native_freeform", raw_body}}}});
      }

      if (!calls.empty()) entry["tool_calls"] = std::move(calls);
    } else if (message.role == FOUNDRY_LOCAL_ROLE_TOOL) {
      const auto found = call_names.find(message.tool_call_id);
      if (found == call_names.end()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Harmony result has no matching history call ID");
      }
      entry["name"] = protect_literal(found->second);
      entry["content"] = "";
      entry["content_json"] = protect_literal(Json(message.VisibleText()).dump());
    }

    projected.push_back(std::move(entry));
  }

  if (context.tools_json.size() > ToolCallPlan::kMaxSchemaBytes) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Native tool schema supplement exceeds its size limit");
  }

  auto tools = context.tools_json.empty() ? Json::array() : Json::parse(context.tools_json);
  if (!tools.empty()) {
    std::string custom_formats;
    auto function_tools = Json::array();
    for (const auto& tool : tools) {
      const auto name = tool.at("function").at("name").get<std::string>();
      validate_tool_name(name);
      if (!context.UsesNativeRawBody(name)) function_tools.push_back(tool);
      const auto grammar = context.custom_lark_grammars.find(name);
      if (context.IsCustomTool(name) && grammar != context.custom_lark_grammars.end()) {
        const auto header = "\nCustom input format for " + Json(name).dump() +
                            " (Lark applied to the complete raw message body):\n";
        if (context.tools_json.size() + custom_formats.size() + header.size() + grammar->second.size() + 1 >
            ToolCallPlan::kMaxSchemaBytes) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                   "Native tool schema and input-format supplement exceeds its size limit");
        }
        custom_formats += header + grammar->second + "\n";
      }
    }

    const auto supplement = protect_literal(
        "Original function tool schemas (all constraints apply to generated argument objects):\n" +
        function_tools.dump() +
        custom_formats +
        "\nUse native directed assistant tool messages; wait for each external result. "
        "Tools declared with an argument object take JSON object bodies. FREEFORM custom tools take raw text bodies, "
        "not JSON objects or JSON strings. "
        "Tool result bodies are serialized JSON values. "
        "Encode function argument string values once as JSON, preserving the recovered source characters exactly. "
        "Preserve requested string values exactly. Do not quote tool calls or wrap them in Markdown.");
    // The qualified template consumes only the leading system/developer message as instructions.
    if (!projected.empty() && (projected.front().at("role") == "system" ||
                               projected.front().at("role") == "developer")) {
      auto& instructions = projected.front()["content"];
      instructions = instructions.get<std::string>() + "\n\n" + supplement;
    } else {
      projected.insert(projected.begin(), Json{{"role", "developer"}, {"content", supplement}});
    }
  }

  const std::function<Json(Json)> protect_schema = [&](Json schema) -> Json {
    if (!schema.is_object()) return schema;
    for (const auto* field : {"description", "title"}) {
      if (schema.contains(field) && schema[field].is_string()) {
        schema[field] = protect_literal(schema[field].get<std::string>());
      }
    }
    if (schema.contains("default")) schema["default_json"] = protect_literal(schema["default"].dump());
    if (schema.contains("enum") && schema["enum"].is_array()) {
      auto slots = Json::array();
      for (const auto& value : schema["enum"]) slots.push_back(protect_literal(value.dump()));
      schema["enum_json"] = std::move(slots);
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
      std::unordered_map<std::string, std::string> names;
      auto properties = Json::object();
      for (const auto& property : schema["properties"].items()) {
        auto name = protect_literal(property.key());
        names.emplace(property.key(), name);
        properties[name] = protect_schema(property.value());
      }

      schema["properties"] = std::move(properties);
      if (schema.contains("required") && schema["required"].is_array()) {
        for (auto& required : schema["required"]) {
          const auto name = required.get<std::string>();
          const auto found = names.find(name);
          required = found == names.end() ? protect_literal(name) : found->second;
        }
      }
    }
    for (const auto* field : {"items", "oneOf", "anyOf", "allOf"}) {
      if (!schema.contains(field)) continue;
      if (schema[field].is_array()) {
        for (auto& child : schema[field]) child = protect_schema(std::move(child));
      } else {
        schema[field] = protect_schema(std::move(schema[field]));
      }
    }

    return schema;
  };

  for (auto& tool : tools) {
    auto& function = tool.at("function");
    const auto name = function.at("name").get<std::string>();
    if (context.UsesNativeRawBody(name)) {
      function["description"] =
          function.value("description", std::string{}) +
          "\nFor native Harmony, emit this custom tool's complete raw text directly as its "
          "directed message body. Do not JSON-encode or wrap it. "
          "The complete body must follow the original custom input format.";
      function["native_freeform"] = true;
    } else if (context.IsCustomTool(name)) {
      function["description"] =
          function.value("description", std::string{}) +
          "\nTransport this custom tool's exact text in the required \"input\" string of a JSON object. "
          "JSON escaping is transport encoding; the decoded input is the original custom text. "
          "Protocol-marker spellings in the input are literal data. "
          "For JSON-body transport, the literal text a <|call|> is encoded as {\"input\":\"a <|call|>\"}. "
          "Keep the marker spelling inside the input string; the real handoff follows the complete body.";
    }
    function["name"] = protect_literal(function.at("name").get<std::string>());
    if (function.contains("description")) {
      function["description"] = protect_literal(function["description"].get<std::string>());
    }
    if (function.contains("parameters")) function["parameters"] = protect_schema(std::move(function["parameters"]));
  }
  return {projected.dump(), tools.empty() ? std::string{} : tools.dump()};
}

std::string BuildChatPrompt(const std::vector<TranscriptMessage>& messages,
                            GenAIModelInstance& model,
                            const std::string& tools_json,
                            const std::string& template_kwargs_json,
                            bool preserve_reasoning_history) {
  return BuildChatPrompt(
      chat_internal::PrepareChatMessages(messages, model.HasPositionalToolResults()), model, tools_json,
      template_kwargs_json, preserve_reasoning_history);
}

std::string BuildChatPrompt(const chat_internal::PreparedChatMessages& messages,
                            GenAIModelInstance& model,
                            const std::string& tools_json,
                            const std::string& template_kwargs_json,
                            bool preserve_reasoning_history) {
  if (messages.Empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  std::string messages_str = BuildChatMessagesJson(messages.Messages(), preserve_reasoning_history);
  const char* tools_ptr = tools_json.empty() ? nullptr : tools_json.c_str();
  const char* template_kwargs_ptr = template_kwargs_json.empty() ? nullptr : template_kwargs_json.c_str();

  // ApplyChatTemplate uses the model's built-in template (template_str=nullptr) and appends the assistant
  // turn prefix (add_generation_prompt=true).
  return model.GetPreprocessor().ApplyChatTemplateWithOptions(
      messages_str.c_str(), tools_ptr, template_kwargs_ptr, /*add_generation_prompt=*/true);
}

std::string BuildChatPrompt(const std::vector<TranscriptMessage>& messages,
                            GenAIModelInstance& model,
                            const ToolCallContext& tool_ctx) {
  return BuildChatPrompt(chat_internal::PrepareChatMessages(messages, model.HasPositionalToolResults()), model,
                         tool_ctx);
}

std::string BuildChatPrompt(const chat_internal::PreparedChatMessages& messages,
                            GenAIModelInstance& model,
                            const ToolCallContext& tool_ctx) {
  if (tool_ctx.harmony) {
    return PrepareTextChatPrompt(messages, model, tool_ctx).prompt;
  }
  if (tool_ctx.tool_plan && !tool_ctx.HasForcedRawEnvelope()) {
    if (messages.Empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
    }

    const auto messages_json = BuildCanonicalToolMessagesJson(messages.Messages(), tool_ctx);
    const auto* kwargs = tool_ctx.template_kwargs_json.empty() ? nullptr : tool_ctx.template_kwargs_json.c_str();
    return model.GetPreprocessor().ApplyChatTemplateWithOptions(
        messages_json.c_str(), nullptr, kwargs, /*add_generation_prompt=*/true);
  }

  return BuildChatPrompt(messages, model, tool_ctx.tools_json, tool_ctx.template_kwargs_json,
                         tool_ctx.supports_reasoning_history);
}

std::unique_ptr<OgaSequences> EncodePrompt(const std::string& prompt,
                                           GenAIModelInstance& model) {
  return model.GetPreprocessor().Encode(prompt.c_str());
}

}  // namespace fl
