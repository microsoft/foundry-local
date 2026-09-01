// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contracts/responses.h"
#include "util/json_helpers.h"

#include <type_traits>

namespace fl {
namespace responses {

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

        if (type == "input_text") {
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

void from_json(const nlohmann::json& j, FunctionCallResultInputItem& f) {
  f.type = j.value("type", "function_call_output");
  f.call_id = j.at("call_id").get<std::string>();
  f.output = j.at("output").get<std::string>();
}

void from_json(const nlohmann::json& j, FunctionCallInputItem& f) {
  f.type = j.value("type", "function_call");
  f.call_id = j.at("call_id").get<std::string>();
  f.name = j.at("name").get<std::string>();
  f.arguments = j.at("arguments").get<std::string>();
}

// ========================================================================
// Tool types from_json
// ========================================================================

void from_json(const nlohmann::json& j, FunctionDefinition& f) {
  f.name = j.at("name").get<std::string>();
  opt_str(j, "description", f.description);

  // AD-007: store parameters as JSON string, not nlohmann::json
  if (j.contains("parameters") && !j["parameters"].is_null()) {
    f.parameters_json = j["parameters"].dump();
  }

  opt_bool(j, "strict", f.strict);
}

void from_json(const nlohmann::json& j, ToolDefinition& t) {
  t.type = j.value("type", "function");

  // Responses API uses flat format: name/description/parameters at tool level
  if (j.contains("name")) {
    t.function.name = j["name"].get<std::string>();

    if (j.contains("description") && j["description"].is_string()) {
      t.function.description = j["description"].get<std::string>();
    }

    if (j.contains("parameters") && !j["parameters"].is_null()) {
      t.function.parameters_json = j["parameters"].dump();
    }

    opt_bool(j, "strict", t.function.strict);
  } else if (j.contains("function")) {
    // Fallback: Chat Completions nested format
    t.function = j["function"].get<FunctionDefinition>();
  }
}

void from_json(const nlohmann::json& j, ForcedFunction& f) {
  f.name = j.at("name").get<std::string>();
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

        if (type == "function_call") {
          items.push_back(entry.get<FunctionCallInputItem>());
        } else if (type == "function_call_output") {
          items.push_back(entry.get<FunctionCallResultInputItem>());
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

  // tool_choice: string ("auto"/"none"/"required") or {"type":"function","name":"..."}
  if (j.contains("tool_choice") && !j["tool_choice"].is_null()) {
    const auto& tc = j["tool_choice"];

    if (tc.is_string()) {
      p.tool_choice = tc.get<std::string>();
    } else if (tc.is_object() && tc.contains("name")) {
      p.tool_choice = tc.get<ForcedFunction>();
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

  if (f.parameters_json.has_value()) {
    j["parameters"] = nlohmann::json::parse(*f.parameters_json);
  }

  if (f.strict.has_value()) {
    j["strict"] = *f.strict;
  }
}

void to_json(nlohmann::json& j, const ToolDefinition& t) {
  // Responses API flat format: type/name/description/parameters at tool level
  j = nlohmann::json{
      {"type", t.type},
      {"name", t.function.name},
  };

  if (t.function.description.has_value()) {
    j["description"] = *t.function.description;
  }

  if (t.function.parameters_json.has_value()) {
    j["parameters"] = nlohmann::json::parse(*t.function.parameters_json);
  }

  if (t.function.strict.has_value()) {
    j["strict"] = *t.function.strict;
  }
}

void to_json(nlohmann::json& j, const ForcedFunction& f) {
  j = nlohmann::json{
      {"type", "function"},
      {"name", f.name},
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
        j["tool_choice"] = nlohmann::json{{"type", "function"}, {"name", v.name}};
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

  // Function call streaming
  if (e.type == StreamEventType::kFunctionCallArgumentsDelta) {
    j["delta"] = e.delta;
    j["output_index"] = e.output_index;
    j["item_id"] = e.item_id;

    if (e.function_call_id.has_value()) {
      j["call_id"] = *e.function_call_id;
    }
  }

  if (e.type == StreamEventType::kFunctionCallArgumentsDone) {
    j["output_index"] = e.output_index;
    j["item_id"] = e.item_id;

    if (e.function_name.has_value()) {
      j["name"] = *e.function_name;
    }

    if (e.function_call_id.has_value()) {
      j["call_id"] = *e.function_call_id;
    }

    if (e.function_arguments.has_value()) {
      j["arguments"] = *e.function_arguments;
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
