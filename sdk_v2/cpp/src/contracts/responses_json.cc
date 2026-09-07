// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contracts/responses.h"

#include "contracts/tool_definitions.h"
#include "exception.h"
#include "util/json_helpers.h"

#include <type_traits>

namespace fl {
namespace responses {

namespace {

std::string RequiredNonEmptyString(const nlohmann::json& j, const char* key, const char* owner) {
  const auto value = j.find(key);
  if (value == j.end() || !value->is_string() || value->get_ref<const std::string&>().empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, owner, " must contain a non-empty string '", key, "'");
  }

  return value->get<std::string>();
}

}  // namespace

// ========================================================================
// Enum string helpers
// ========================================================================

std::string ResponseStatusToString(ResponseStatus status) {
  switch (status) {
    case ResponseStatus::kInProgress:
      return "in_progress";
    case ResponseStatus::kCompleted:
      return "completed";
    case ResponseStatus::kFailed:
      return "failed";
    case ResponseStatus::kCancelled:
      return "cancelled";
    case ResponseStatus::kIncomplete:
      return "incomplete";
    default:
      return "in_progress";
  }
}

ResponseStatus ResponseStatusFromString(const std::string& s) {
  if (s == "completed") {
    return ResponseStatus::kCompleted;
  }

  if (s == "failed") {
    return ResponseStatus::kFailed;
  }

  if (s == "cancelled") {
    return ResponseStatus::kCancelled;
  }

  if (s == "incomplete") {
    return ResponseStatus::kIncomplete;
  }

  return ResponseStatus::kInProgress;
}

std::string StreamEventTypeToString(StreamEventType type) {
  switch (type) {
    case StreamEventType::kResponseCreated:
      return "response.created";
    case StreamEventType::kResponseInProgress:
      return "response.in_progress";
    case StreamEventType::kResponseCompleted:
      return "response.completed";
    case StreamEventType::kResponseFailed:
      return "response.failed";
    case StreamEventType::kResponseIncomplete:
      return "response.incomplete";
    case StreamEventType::kOutputItemAdded:
      return "response.output_item.added";
    case StreamEventType::kOutputItemDone:
      return "response.output_item.done";
    case StreamEventType::kContentPartAdded:
      return "response.content_part.added";
    case StreamEventType::kContentPartDone:
      return "response.content_part.done";
    case StreamEventType::kTextDelta:
      return "response.output_text.delta";
    case StreamEventType::kTextDone:
      return "response.output_text.done";
    case StreamEventType::kRefusalDelta:
      return "response.refusal.delta";
    case StreamEventType::kRefusalDone:
      return "response.refusal.done";
    case StreamEventType::kAudioDelta:
      return "response.audio.delta";
    case StreamEventType::kAudioDone:
      return "response.audio.done";
    case StreamEventType::kAudioTranscriptDelta:
      return "response.audio_transcript.delta";
    case StreamEventType::kAudioTranscriptDone:
      return "response.audio_transcript.done";
    case StreamEventType::kFunctionCallArgumentsDelta:
      return "response.function_call_arguments.delta";
    case StreamEventType::kFunctionCallArgumentsDone:
      return "response.function_call_arguments.done";
    case StreamEventType::kCustomToolCallInputDelta:
      return "response.custom_tool_call_input.delta";
    case StreamEventType::kCustomToolCallInputDone:
      return "response.custom_tool_call_input.done";
    case StreamEventType::kReasoningDelta:
      return "response.reasoning.delta";
    case StreamEventType::kReasoningDone:
      return "response.reasoning.done";
    case StreamEventType::kError:
      return "error";
    default:
      return "error";
  }
}

// ========================================================================
// Input content from_json (request deserialization)
// ========================================================================

void from_json(const nlohmann::json& j, InputTextContent& c) {
  c.text = j.at("text").get<std::string>();
}

void from_json(const nlohmann::json& j, InputImageContent& c) {
  c.detail = j.value("detail", "auto");
  opt_str(j, "image_url", c.image_url);
  opt_str(j, "image_data", c.image_data);
  opt_str(j, "file_id", c.file_id);
  opt_str(j, "media_type", c.media_type);
}

void from_json(const nlohmann::json& j, InputFileContent& c) {
  opt_str(j, "file_id", c.file_id);
  opt_str(j, "file_data", c.file_data);
  opt_str(j, "filename", c.filename);
}

void from_json(const nlohmann::json& j, InputAudioContent& c) {
  c.data = j.at("data").get<std::string>();
  c.format = j.at("format").get<std::string>();
}

void from_json(const nlohmann::json& j, InputMessage& m) {
  m.role = j.at("role").get<std::string>();

  if (j.contains("content")) {
    const auto& content = j["content"];

    if (content.is_string()) {
      // Simple string content -> single InputTextContent
      InputTextContent tc;
      tc.text = content.get<std::string>();
      m.content.push_back(std::move(tc));
    } else if (content.is_array()) {
      for (const auto& part : content) {
        std::string type = part.value("type", "");

        // `output_text` is the shape the Responses API emits for assistant messages, so a caller replaying a
        // conversation statelessly echoes it back verbatim; `text` is the shape used by stored items. Both carry a
        // `text` field and are accepted as input text, matching what chain reconstruction already replays — without
        // them, typed replay would silently drop every assistant turn's visible answer.
        if (type == "input_text" || type == "output_text" || type == "text") {
          m.content.push_back(part.get<InputTextContent>());
        } else if (type == "input_image") {
          m.content.push_back(part.get<InputImageContent>());
        } else if (type == "input_file") {
          m.content.push_back(part.get<InputFileContent>());
        } else if (type == "input_audio") {
          m.content.push_back(part.get<InputAudioContent>());
        }
        // Unknown types silently skipped
      }
    }
  }
}

void from_json(const nlohmann::json& j, FunctionCallInputItem& f) {
  f.type = j.value("type", "function_call");
  f.call_id = RequiredNonEmptyString(j, "call_id", "function_call");
  f.name = RequiredNonEmptyString(j, "name", "function_call");

  // Arguments are a JSON string on the wire. Objects are accepted too and re-serialized to their canonical bytes,
  // matching the Chat Completions path so the same tool-call payload works on either API.
  if (auto arguments = j.find("arguments"); arguments != j.end() && !arguments->is_null()) {
    if (arguments->is_string()) {
      f.arguments = arguments->get<std::string>();
    } else if (arguments->is_object()) {
      f.arguments = arguments->dump();
    } else {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "function_call arguments must be a JSON string or object");
    }
  }
}

void from_json(const nlohmann::json& j, FunctionCallResultInputItem& f) {
  f.type = j.value("type", "function_call_output");
  f.call_id = RequiredNonEmptyString(j, "call_id", "function_call_output");
  f.output = j.at("output").get<std::string>();
}

void from_json(const nlohmann::json& j, CustomToolCallResultInputItem& c) {
  c.type = j.value("type", "custom_tool_call_output");
  c.call_id = RequiredNonEmptyString(j, "call_id", "custom_tool_call_output");
  c.output = j.at("output").get<std::string>();
}

void from_json(const nlohmann::json& j, CustomToolCallInputItem& c) {
  c.type = j.value("type", "custom_tool_call");
  c.call_id = RequiredNonEmptyString(j, "call_id", "custom_tool_call");
  c.name = RequiredNonEmptyString(j, "name", "custom_tool_call");

  // Empty input is valid raw text, but absence and null cannot represent a custom call payload.
  const auto input = j.find("input");
  if (input == j.end() || !input->is_string()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "custom_tool_call input is required and must be a string");
  }

  c.input = input->get<std::string>();
}

// ========================================================================
// Tool types from_json
// ========================================================================

void from_json(const nlohmann::json& j, FunctionDefinition& f) {
  if (!j.is_object() || !j.contains("name") || !j["name"].is_string() ||
      j["name"].get_ref<const std::string&>().empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "function tool must contain a non-empty string 'name'");
  }

  f.name = j["name"].get<std::string>();
  opt_str(j, "description", f.description);

  // AD-007: store parameters as JSON string, not nlohmann::json
  if (auto parameters = j.find("parameters"); parameters != j.end()) {
    f.parameters_present = true;
    if (!parameters->is_null() && !parameters->is_object()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "function tool 'parameters' must be an object or null");
    }

    if (parameters->is_object()) {
      f.parameters_json = parameters->dump();
    }
  }

  if (auto strict = j.find("strict"); strict != j.end()) {
    f.strict_present = true;
    f.strict = tools::ParseFunctionStrict(*strict);
  }
}

void from_json(const nlohmann::json& j, CustomToolDefinition& c) {
  if (!j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "custom tool must contain a non-empty string 'name'");
  }

  c.name = j["name"].get<std::string>();

  for (const auto* forbidden : {"custom", "function", "parameters", "strict"}) {
    if (j.contains(forbidden)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "custom tool must not contain '", forbidden, "'");
    }
  }

  opt_str(j, "description", c.description);

  auto format = j.find("format");
  c.format = tools::ParseCustomToolFormat(format == j.end() ? nlohmann::json() : *format, c.name);
}

void from_json(const nlohmann::json& j, ToolDefinition& t) {
  // Tool entries are polymorphic and only the member matching `type` is meaningful, so an unrecognized type is
  // rejected rather than falling through to the function branch. Reading it as a function would offer the model a
  // tool the runtime cannot honour — a nameless one if the entry nests its declaration — instead of telling the
  // caller the tool is unsupported. Symmetric with the Chat Completions surface.
  if (auto type = j.find("type"); type != j.end() && !type->is_null()) {
    if (!type->is_string()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool 'type' must be a string");
    }

    t.type = type->get<std::string>();
  } else {
    t.type = "function";
  }

  if (t.type != "function" && t.type != "custom") {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported tool type '", t.type,
             "'; expected 'function' or 'custom'");
  }

  // A custom tool declares itself inline and has no `parameters` — reading it as a function would
  // silently produce a nameless function tool.
  if (t.type == "custom") {
    t.custom = j.get<CustomToolDefinition>();
    return;
  }

  if (j.contains("custom")) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "function tool must not contain 'custom'");
  }

  // Responses API uses flat format: name/description/parameters at tool level
  if (j.contains("name")) {
    if (j.contains("function")) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "flat function tool must not contain nested 'function'");
    }

    t.function = j.get<FunctionDefinition>();
  } else if (j.contains("function")) {
    // Fallback: Chat Completions nested format
    if (!j["function"].is_object()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "nested function tool 'function' must be an object");
    }

    t.function = j["function"].get<FunctionDefinition>();
  } else {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "function tool must contain a non-empty string 'name'");
  }
}

void from_json(const nlohmann::json& j, ForcedFunction& f) {
  f.name = j.at("name").get<std::string>();
}

void from_json(const nlohmann::json& j, ForcedCustomTool& f) {
  f.name = j.at("name").get<std::string>();
}

void from_json(const nlohmann::json& j, AllowedToolReference& r) {
  if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "allowed_tools entries must contain a string 'type'");
  }

  r.type = j["type"].get<std::string>();
  if (r.type != "function" && r.type != "custom") {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "allowed_tools entry type must be 'function' or 'custom'");
  }

  if (!j.contains("name") || !j["name"].is_string() ||
      j["name"].get_ref<const std::string&>().empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "allowed_tools entries must contain a non-empty string 'name'");
  }

  r.name = j["name"].get<std::string>();
}

void from_json(const nlohmann::json& j, AllowedToolsChoice& c) {
  if (!j.contains("mode") || !j["mode"].is_string()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "allowed_tools tool_choice must contain a string 'mode'");
  }

  c.mode = j["mode"].get<std::string>();
  if (c.mode != "auto" && c.mode != "required") {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "allowed_tools mode must be 'auto' or 'required'");
  }

  if (!j.contains("tools") || !j["tools"].is_array()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "allowed_tools tool_choice must contain a 'tools' array");
  }

  c.tools = j["tools"].get<std::vector<AllowedToolReference>>();
  if (c.tools.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "allowed_tools tool_choice must contain at least one tool");
  }
}

// ========================================================================
// Request from_json
// ========================================================================

void from_json(const nlohmann::json& j, ResponseTextConfig& c) {
  if (j.contains("format") && j["format"].is_object()) {
    const auto& fmt = j["format"];
    c.format = fmt.value("type", "text");

    if (fmt.contains("schema") && !fmt["schema"].is_null()) {
      c.json_schema = fmt["schema"].dump();
    }

    if (fmt.contains("grammar") && fmt["grammar"].is_string()) {
      c.lark_grammar = fmt["grammar"].get<std::string>();
    }
  } else {
    c.format = "text";
  }
}

void from_json(const nlohmann::json& j, ReasoningConfig& c) {
  opt_str(j, "effort", c.effort);
  opt_bool(j, "generate_summary", c.generate_summary);
}

void from_json(const nlohmann::json& j, ResponseCreateParams& p) {
  // Required
  p.model = j.at("model").get<std::string>();

  // input: string or array of InputItem
  if (j.contains("input")) {
    const auto& input = j["input"];

    if (input.is_string()) {
      p.input = input.get<std::string>();
    } else if (input.is_array()) {
      std::vector<InputItem> items;

      for (const auto& entry : input) {
        std::string type = entry.value("type", "");

        if (type == "function_call_output") {
          items.push_back(entry.get<FunctionCallResultInputItem>());
        } else if (type == "function_call") {
          // Assistant tool calls are replayed as input when the caller chains turns without server-side storage.
          items.push_back(entry.get<FunctionCallInputItem>());
        } else if (type == "custom_tool_call_output") {
          items.push_back(entry.get<CustomToolCallResultInputItem>());
        } else if (type == "custom_tool_call") {
          items.push_back(entry.get<CustomToolCallInputItem>());
        } else if (type == "reasoning") {
          // Reasoning text is private and is never fed back to the model. The assistant turn that produced it still
          // happened, so it replays as an empty assistant message: dropping the item outright would leave two user
          // turns next to each other and a different prompt than the live session builds. When the same turn also
          // carried visible text or a call, this boundary merges into that assistant message and changes nothing.
          InputMessage boundary;
          boundary.role = "assistant";
          items.push_back(std::move(boundary));
        } else {
          // Default: message item
          items.push_back(entry.get<InputMessage>());
        }
      }

      p.input = std::move(items);
    }
  }

  // Optional scalar fields
  opt_str(j, "instructions", p.instructions);
  opt_str(j, "previous_response_id", p.previous_response_id);
  opt_float(j, "temperature", p.temperature);
  opt_int(j, "max_output_tokens", p.max_output_tokens);
  opt_float(j, "top_p", p.top_p);
  opt_float(j, "presence_penalty", p.presence_penalty);
  opt_float(j, "frequency_penalty", p.frequency_penalty);
  opt_int(j, "seed", p.seed);

  if (j.contains("stream") && j["stream"].is_boolean()) {
    p.stream = j["stream"].get<bool>();
  }

  if (j.contains("store") && j["store"].is_boolean()) {
    p.store = j["store"].get<bool>();
  }

  // Text config
  if (j.contains("text") && j["text"].is_object()) {
    p.text = j["text"].get<ResponseTextConfig>();
  }

  // Tools
  if (j.contains("tools") && j["tools"].is_array()) {
    p.tools = j["tools"].get<std::vector<ToolDefinition>>();
  }

  // tool_choice: a mode string ("auto"/"none"/"required") or a forced tool:
  //   {"type":"function","name":".."} / {"type":"custom","name":".."}
  if (j.contains("tool_choice") && !j["tool_choice"].is_null()) {
    const auto& tc = j["tool_choice"];

    if (tc.is_string()) {
      const auto mode = tc.get<std::string>();
      if (mode != "auto" && mode != "none" && mode != "required") {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid tool_choice '", mode,
                 "'; expected 'auto', 'none' or 'required'");
      }

      p.tool_choice = mode;
    } else if (tc.is_object()) {
      const auto type = tc.value("type", "function");
      if (type == "allowed_tools") {
        p.tool_choice = tc.get<AllowedToolsChoice>();
      } else {
        if (!tc.contains("name") || !tc["name"].is_string() ||
            tc["name"].get<std::string>().empty()) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                   "a forced tool_choice must contain a non-empty string 'name'");
        }

        if (type == "custom") {
          p.tool_choice = tc.get<ForcedCustomTool>();
        } else if (type == "function") {
          p.tool_choice = tc.get<ForcedFunction>();
        } else {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported tool_choice type '", type,
                   "'; expected 'function', 'custom' or 'allowed_tools'");
        }
      }
    } else {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_choice must be a string or an object");
    }
  }

  // allowed_tools
  if (j.contains("allowed_tools") && j["allowed_tools"].is_array()) {
    p.allowed_tools = j["allowed_tools"].get<std::vector<std::string>>();
  }

  opt_bool(j, "parallel_tool_calls", p.parallel_tool_calls);

  // Reasoning
  if (j.contains("reasoning") && j["reasoning"].is_object()) {
    p.reasoning = j["reasoning"].get<ReasoningConfig>();
  }

  // Metadata
  if (j.contains("metadata") && j["metadata"].is_object()) {
    for (const auto& [key, value] : j["metadata"].items()) {
      if (value.is_string()) {
        p.metadata[key] = value.get<std::string>();
      }
    }
  }

  opt_str(j, "user", p.user);

  // extra_json: preserve any unrecognized fields
  if (j.contains("extra") && !j["extra"].is_null()) {
    p.extra_json = j["extra"].dump();
  }
}

// ========================================================================
// Config types to_json (for echoing in response)
// ========================================================================

void to_json(nlohmann::json& j, const ResponseTextConfig& c) {
  nlohmann::json fmt;
  fmt["type"] = c.format;

  if (c.json_schema.has_value()) {
    fmt["schema"] = nlohmann::json::parse(*c.json_schema);
  }

  if (c.lark_grammar.has_value()) {
    fmt["grammar"] = *c.lark_grammar;
  }

  j = nlohmann::json{{"format", fmt}};
}

void to_json(nlohmann::json& j, const ReasoningConfig& c) {
  j = nlohmann::json::object();

  if (c.effort.has_value()) {
    j["effort"] = *c.effort;
  }

  if (c.generate_summary.has_value()) {
    j["generate_summary"] = *c.generate_summary;
  }
}

// ========================================================================
// Output to_json (response serialization)
// ========================================================================

void to_json(nlohmann::json& j, const OutputTextContent& c) {
  j = nlohmann::json{
      {"type", "output_text"},
      {"text", c.text},
  };
}

void to_json(nlohmann::json& j, const OutputRefusalContent& c) {
  j = nlohmann::json{
      {"type", "refusal"},
      {"refusal", c.refusal},
  };
}

void to_json(nlohmann::json& j, const OutputAudioContent& c) {
  j = nlohmann::json{
      {"type", "output_audio"},
      {"data", c.data},
      {"transcript", c.transcript},
  };
}

void to_json(nlohmann::json& j, const ResponseOutputMessage& m) {
  j = nlohmann::json{
      {"type", "message"},
      {"id", m.id},
      {"role", m.role},
      {"status", ResponseStatusToString(m.status)},
  };

  nlohmann::json content_arr = nlohmann::json::array();
  for (const auto& c : m.content) {
    nlohmann::json cj;
    std::visit([&](const auto& v) { to_json(cj, v); }, c);
    content_arr.push_back(std::move(cj));
  }

  j["content"] = content_arr;
}

void to_json(nlohmann::json& j, const FunctionCallOutputItem& f) {
  j = nlohmann::json{
      {"type", f.type},
      {"id", f.id},
      {"call_id", f.call_id},
      {"name", f.name},
      {"arguments", f.arguments},
      {"status", ResponseStatusToString(f.status)},
  };
}

void to_json(nlohmann::json& j, const CustomToolCallOutputItem& c) {
  j = nlohmann::json{
      {"type", c.type},
      {"id", c.id},
      {"call_id", c.call_id},
      {"name", c.name},
      {"input", c.input},
  };
}

void to_json(nlohmann::json& j, const ReasoningSummaryText& s) {
  j = nlohmann::json{
      {"type", "summary_text"},
      {"text", s.text},
  };
}

void to_json(nlohmann::json& j, const ReasoningOutputItem& r) {
  nlohmann::json summary_arr = nlohmann::json::array();

  for (const auto& s : r.summary) {
    nlohmann::json sj;
    to_json(sj, s);
    summary_arr.push_back(std::move(sj));
  }

  j = nlohmann::json{
      {"type", "reasoning"},
      {"id", r.id},
      {"summary", std::move(summary_arr)},
      {"status", ResponseStatusToString(r.status)},
  };
}

void to_json(nlohmann::json& j, const InputTokensDetails& d) {
  j = nlohmann::json{
      {"cached_tokens", d.cached_tokens},
  };
}

void to_json(nlohmann::json& j, const OutputTokensDetails& d) {
  j = nlohmann::json{
      {"reasoning_tokens", d.reasoning_tokens},
  };
}

void to_json(nlohmann::json& j, const ResponseUsage& u) {
  j = nlohmann::json{
      {"input_tokens", u.input_tokens},
      {"output_tokens", u.output_tokens},
      {"total_tokens", u.total_tokens},
      {"input_tokens_details", u.input_tokens_details},
      {"output_tokens_details", u.output_tokens_details},
  };
}

void to_json(nlohmann::json& j, const ResponseError& e) {
  j = nlohmann::json{
      {"code", e.code},
      {"message", e.message},
  };
}

// ========================================================================
// Tool types to_json (Responses API flat format)
// ========================================================================

void to_json(nlohmann::json& j, const FunctionDefinition& f) {
  j = nlohmann::json{{"name", f.name}};

  if (f.description.has_value()) {
    j["description"] = *f.description;
  }

  if (f.parameters_present || f.parameters_json.has_value()) {
    j["parameters"] = f.parameters_json.has_value()
                          ? nlohmann::json::parse(*f.parameters_json)
                          : nlohmann::json(nullptr);
  }

  if (f.strict_present || f.strict.has_value()) {
    j["strict"] = f.strict.has_value() ? nlohmann::json(*f.strict) : nlohmann::json(nullptr);
  }
}

void to_json(nlohmann::json& j, const CustomToolDefinition& c) {
  j = nlohmann::json{{"name", c.name}};

  if (c.description.has_value()) {
    j["description"] = *c.description;
  }

  // Always spelled out: text is the only format the runtime accepts, so echoing it states the
  // declaration the tool actually runs under instead of leaving it implied by omission.
  j["format"] = c.format;
}

void to_json(nlohmann::json& j, const ToolDefinition& t) {
  if (t.custom.has_value()) {
    // Responses inlines the custom declaration next to "type" rather than nesting it.
    j = nlohmann::json{{"type", "custom"}};
    j.update(nlohmann::json(*t.custom));
    return;
  }

  // Responses API flat format: type/name/description/parameters at tool level
  j = nlohmann::json{
      {"type", t.type},
      {"name", t.function.name},
  };

  if (t.function.description.has_value()) {
    j["description"] = *t.function.description;
  }

  if (t.function.parameters_present || t.function.parameters_json.has_value()) {
    j["parameters"] = t.function.parameters_json.has_value()
                          ? nlohmann::json::parse(*t.function.parameters_json)
                          : nlohmann::json(nullptr);
  }

  if (t.function.strict_present || t.function.strict.has_value()) {
    j["strict"] = t.function.strict.has_value()
                      ? nlohmann::json(*t.function.strict)
                      : nlohmann::json(nullptr);
  }
}

void to_json(nlohmann::json& j, const ForcedCustomTool& f) {
  j = nlohmann::json{
      {"type", "custom"},
      {"name", f.name},
  };
}

void to_json(nlohmann::json& j, const ForcedFunction& f) {
  j = nlohmann::json{
      {"type", "function"},
      {"name", f.name},
  };
}

void to_json(nlohmann::json& j, const AllowedToolReference& r) {
  j = nlohmann::json{{"type", r.type}, {"name", r.name}};
}

void to_json(nlohmann::json& j, const AllowedToolsChoice& c) {
  j = nlohmann::json{
      {"type", "allowed_tools"},
      {"mode", c.mode},
      {"tools", c.tools},
  };
}

// ========================================================================
// ResponseObject to_json — the complete Responses API response
// ========================================================================

void to_json(nlohmann::json& j, const ResponseObject& r) {
  j = nlohmann::json{
      {"id", r.id},
      {"object", "response"},
      {"created_at", r.created_at},
      {"status", ResponseStatusToString(r.status)},
      {"model", r.model},
      {"output_text", r.output_text},
  };

  // Optional timestamps — emit null if absent
  j["completed_at"] = r.completed_at.has_value()
                          ? nlohmann::json(*r.completed_at)
                          : nlohmann::json(nullptr);
  j["failed_at"] = r.failed_at.has_value()
                       ? nlohmann::json(*r.failed_at)
                       : nlohmann::json(nullptr);
  j["cancelled_at"] = r.cancelled_at.has_value()
                          ? nlohmann::json(*r.cancelled_at)
                          : nlohmann::json(nullptr);

  // Usage
  j["usage"] = r.usage;

  // Error — null if absent
  if (r.error.has_value()) {
    j["error"] = *r.error;
  } else {
    j["error"] = nullptr;
  }

  // Incomplete details
  if (r.incomplete_reason.has_value()) {
    j["incomplete_details"] = {{"reason", *r.incomplete_reason}};
  } else {
    j["incomplete_details"] = nullptr;
  }

  // Output array (variant dispatch)
  nlohmann::json output_arr = nlohmann::json::array();
  for (const auto& item : r.output) {
    nlohmann::json item_json;
    std::visit([&](const auto& v) { to_json(item_json, v); }, item);
    output_arr.push_back(std::move(item_json));
  }

  j["output"] = output_arr;

  // Instructions / previous_response_id
  j["instructions"] = r.instructions.has_value()
                          ? nlohmann::json(*r.instructions)
                          : nlohmann::json(nullptr);
  j["previous_response_id"] = r.previous_response_id.has_value()
                                  ? nlohmann::json(*r.previous_response_id)
                                  : nlohmann::json(nullptr);

  // Echoed parameters
  j["temperature"] = r.temperature.has_value()
                         ? nlohmann::json(*r.temperature)
                         : nlohmann::json(1.0);
  j["top_p"] = r.top_p.has_value()
                   ? nlohmann::json(*r.top_p)
                   : nlohmann::json(1.0);
  j["presence_penalty"] = r.presence_penalty.has_value()
                              ? nlohmann::json(*r.presence_penalty)
                              : nlohmann::json(0.0);
  j["frequency_penalty"] = r.frequency_penalty.has_value()
                               ? nlohmann::json(*r.frequency_penalty)
                               : nlohmann::json(0.0);
  j["max_output_tokens"] = r.max_output_tokens.has_value()
                               ? nlohmann::json(*r.max_output_tokens)
                               : nlohmann::json(nullptr);
  j["parallel_tool_calls"] = r.parallel_tool_calls;
  j["store"] = r.store;

  // Tools
  nlohmann::json tools_arr = nlohmann::json::array();
  for (const auto& tool : r.tools) {
    nlohmann::json tj;
    to_json(tj, tool);
    tools_arr.push_back(std::move(tj));
  }

  j["tools"] = tools_arr;

  // Tool choice
  if (r.tool_choice.has_value()) {
    std::visit([&](const auto& v) {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, std::string>) {
        j["tool_choice"] = v;
      } else {
        // Echo through the alternative's own to_json so the forced kind survives the round trip.
        j["tool_choice"] = nlohmann::json(v);
      }
    },
               *r.tool_choice);
  } else {
    j["tool_choice"] = "auto";
  }

  // Metadata
  j["metadata"] = r.metadata.empty()
                      ? nlohmann::json::object()
                      : nlohmann::json(r.metadata);
  j["user"] = r.user.has_value()
                  ? nlohmann::json(*r.user)
                  : nlohmann::json(nullptr);

  // Fixed fields for local inference
  j["service_tier"] = "default";
  j["top_logprobs"] = 0;
  j["background"] = false;

  // Text config and reasoning — use struct fields, falling back to defaults
  if (r.text.has_value()) {
    j["text"] = *r.text;
  } else {
    j["text"] = nlohmann::json::object({{"format", {{"type", "text"}}}});
  }

  if (r.reasoning.has_value()) {
    j["reasoning"] = *r.reasoning;
  } else {
    j["reasoning"] = nullptr;
  }

  j["truncation"] = r.truncation;
}

// ========================================================================
// Streaming to_json
// ========================================================================

void to_json(nlohmann::json& j, const StreamEvent& e) {
  j = nlohmann::json{
      {"type", StreamEventTypeToString(e.type)},
      {"sequence_number", e.sequence_number},
  };

  // Include response for lifecycle events
  if (e.response.has_value()) {
    j["response"] = *e.response;
  }

  // Include item for output_item events
  if (e.item.has_value()) {
    nlohmann::json item_json;
    std::visit([&](const auto& v) { to_json(item_json, v); }, *e.item);
    j["item"] = item_json;
    j["output_index"] = e.output_index;
  }

  // Include content part for content_part events
  if (e.content_part.has_value()) {
    nlohmann::json part_json;
    std::visit([&](const auto& v) { to_json(part_json, v); }, *e.content_part);
    j["part"] = part_json;
    j["output_index"] = e.output_index;
    j["content_index"] = e.content_index;
    j["item_id"] = e.item_id;
  }

  // Text delta/done fields
  if (e.type == StreamEventType::kTextDelta ||
      e.type == StreamEventType::kRefusalDelta ||
      e.type == StreamEventType::kAudioDelta ||
      e.type == StreamEventType::kAudioTranscriptDelta ||
      e.type == StreamEventType::kReasoningDelta) {
    j["delta"] = e.delta;
    j["output_index"] = e.output_index;
    j["content_index"] = e.content_index;
    j["item_id"] = e.item_id;
  }

  if (e.type == StreamEventType::kTextDone ||
      e.type == StreamEventType::kRefusalDone ||
      e.type == StreamEventType::kAudioTranscriptDone ||
      e.type == StreamEventType::kReasoningDone) {
    if (e.text.has_value()) {
      j["text"] = *e.text;
    }

    j["output_index"] = e.output_index;
    j["content_index"] = e.content_index;
    j["item_id"] = e.item_id;
  }

  // Tool call streaming. A function call reports its payload as "arguments" and a custom tool call
  // as "input"; everything else about the two lifecycles is identical.
  if (e.type == StreamEventType::kFunctionCallArgumentsDelta ||
      e.type == StreamEventType::kCustomToolCallInputDelta) {
    j["delta"] = e.delta;
    j["output_index"] = e.output_index;
    j["item_id"] = e.item_id;

    if (e.type == StreamEventType::kFunctionCallArgumentsDelta &&
        e.tool_call_id.has_value()) {
      j["call_id"] = *e.tool_call_id;
    }
  }

  if (e.type == StreamEventType::kFunctionCallArgumentsDone ||
      e.type == StreamEventType::kCustomToolCallInputDone) {
    j["output_index"] = e.output_index;
    j["item_id"] = e.item_id;

    if (e.type == StreamEventType::kFunctionCallArgumentsDone &&
        e.tool_name.has_value()) {
      j["name"] = *e.tool_name;
    }

    if (e.type == StreamEventType::kFunctionCallArgumentsDone &&
        e.tool_call_id.has_value()) {
      j["call_id"] = *e.tool_call_id;
    }

    if (e.tool_payload.has_value()) {
      j[e.type == StreamEventType::kCustomToolCallInputDone ? "input" : "arguments"] = *e.tool_payload;
    }
  }

  // Error event
  if (e.type == StreamEventType::kError) {
    if (e.error_code.has_value()) {
      j["error"] = {
          {"code", *e.error_code},
          {"message", e.error_message.value_or("")},
      };
    }
  }
}

}  // namespace responses
}  // namespace fl
