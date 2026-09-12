// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/chat_completions.h"

#include "contracts/tool_definitions.h"
#include "exception.h"
#include "util/json_helpers.h"

namespace fl {

namespace {

/// Read the non-empty string `name` a tool, tool call or tool_choice must carry.
std::string RequiredName(const nlohmann::json& j, const char* owner) {
  auto name = j.find("name");
  if (name == j.end() || !name->is_string() || name->get<std::string>().empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, owner, " must contain a non-empty string 'name'");
  }

  return name->get<std::string>();
}

/// Read the nested object an entry of the given type must carry — `function` for a function entry,
/// `custom` for a custom one. Nesting the wrong payload under a type is a client mistake worth
/// reporting: silently reading the other member would run a tool the caller did not describe.
const nlohmann::json& RequiredObject(const nlohmann::json& j, const char* key, const char* owner) {
  auto nested = j.find(key);
  if (nested == j.end() || !nested->is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, owner, " of type '", key, "' must contain a '", key, "' object");
  }

  return *nested;
}

void RejectMember(const nlohmann::json& j, const char* key, const char* owner) {
  if (j.contains(key)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, owner, " must not contain '", key, "'");
  }
}

void ReadStrict(const nlohmann::json& j, std::optional<bool>& strict) {
  if (auto value = j.find("strict"); value != j.end()) {
    strict = tools::ParseFunctionStrict(*value);
  }
}

/// The declared `type`, defaulting to "function" for entries that omit it (older clients do).
std::string ToolTypeOf(const nlohmann::json& j, const char* owner) {
  auto type = j.find("type");
  if (type == j.end()) {
    return "function";
  }

  if (!type->is_string()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, owner, " 'type' must be a string");
  }

  return type->get<std::string>();
}

}  // namespace

std::string ChatCompletionToolChoice::ModeString() const {
  switch (kind) {
    case Kind::kNone:
      return "none";
    case Kind::kRequired:
    case Kind::kFunction:
    case Kind::kCustom:
      return "required";
    case Kind::kAuto:
    default:
      return "auto";
  }
}

ChatCompletionToolCall ChatCompletionToolCall::MakeFunction(std::string id, std::string name,
                                                            std::string arguments) {
  ChatCompletionToolCall call;
  call.id = std::move(id);
  call.type = "function";
  call.function.name = std::move(name);
  call.function.arguments = std::move(arguments);
  return call;
}

ChatCompletionToolCall ChatCompletionToolCall::MakeCustom(std::string id, std::string name, std::string input) {
  ChatCompletionToolCall call;
  call.id = std::move(id);
  call.type = "custom";
  call.custom = ChatCompletionCustomCall{std::move(name), std::move(input)};
  return call;
}

// ========================================================================
// Request deserialization (from_json)
// ========================================================================

void from_json(const nlohmann::json& j, ChatCompletionFunctionCall& f) {
  if (!j.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_calls[].function must be an object");
  }

  auto name = j.find("name");
  if (name == j.end() || !name->is_string() || name->get<std::string>().empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_calls[].function.name must be a non-empty string");
  }

  f.name = name->get<std::string>();

  // Arguments are a JSON string on the wire. Objects are accepted too and re-serialized, matching how generated
  // tool calls are parsed elsewhere; either way the raw bytes are what the transcript keeps.
  if (auto arguments = j.find("arguments"); arguments != j.end() && !arguments->is_null()) {
    if (arguments->is_string()) {
      f.arguments = arguments->get<std::string>();
    } else if (arguments->is_object()) {
      f.arguments = arguments->dump();
    } else {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "tool_calls[].function.arguments must be a JSON string or object");
    }
  }
}

void from_json(const nlohmann::json& j, ChatCompletionCustomCall& c) {
  if (!j.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_calls[].custom must be an object");
  }

  auto name = j.find("name");
  if (name == j.end() || !name->is_string() || name->get<std::string>().empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_calls[].custom.name must be a non-empty string");
  }

  c.name = name->get<std::string>();

  // Empty input is valid raw text, but absence and null cannot represent a custom call payload.
  const auto input = j.find("input");
  if (input == j.end() || !input->is_string()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "tool_calls[].custom.input is required and must be a string");
  }

  c.input = input->get<std::string>();
}

void from_json(const nlohmann::json& j, ChatCompletionToolCall& tc) {
  if (!j.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_calls[] entry must be an object");
  }

  auto id = j.find("id");
  if (id == j.end() || !id->is_string() || id->get<std::string>().empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_calls[].id must be a non-empty string");
  }

  tc.id = id->get<std::string>();
  tc.type = j.value("type", "function");
  opt_int(j, "index", tc.index);

  // Polymorphic: only the member matching `type` is present, so nothing here may assume a "function" key exists.
  // Nesting the wrong payload under a type would replay a call the caller never described.
  if (tc.type == "function") {
    auto function = j.find("function");
    if (function == j.end()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_calls[] entry requires a function object");
    }

    tc.function = function->get<ChatCompletionFunctionCall>();
    return;
  }

  if (tc.type == "custom") {
    auto custom = j.find("custom");
    if (custom == j.end()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_calls[] entry requires a custom object");
    }

    tc.custom = custom->get<ChatCompletionCustomCall>();
    return;
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported tool call type '", tc.type,
           "'; expected 'function' or 'custom'");
}

void from_json(const nlohmann::json& j, ChatCompletionMessage& m) {
  m.role = j.at("role").get<std::string>();

  // content can be null (for assistant messages with tool_calls)
  if (j.contains("content") && !j["content"].is_null()) {
    if (j["content"].is_string()) {
      m.content = j["content"].get<std::string>();
    } else {
      // Array of content parts — extract text parts
      std::string text;
      if (j["content"].is_array()) {
        for (const auto& part : j["content"]) {
          if (part.is_object()) {
            std::string part_type = part.value("type", "");
            if (part_type == "text" || part_type == "input_text") {
              text += part.value("text", "");
            }
          }
        }
      }

      m.content = text;
    }
  }

  opt_str(j, "name", m.name);
  opt_str(j, "tool_call_id", m.tool_call_id);
  opt_str(j, "reasoning_content", m.reasoning_content);

  if (m.role == "tool" && (!m.tool_call_id.has_value() || m.tool_call_id->empty())) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "tool message tool_call_id must be a non-empty string");
  }

  if (j.contains("tool_calls") && !j["tool_calls"].is_null()) {
    if (!j["tool_calls"].is_array()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "message tool_calls must be an array");
    }

    m.tool_calls = j["tool_calls"].get<std::vector<ChatCompletionToolCall>>();
  }
}

void from_json(const nlohmann::json& j, ChatCompletionFunctionDef& f) {
  f.name = RequiredName(j, "tool function");
  opt_str(j, "description", f.description);

  if (auto parameters = j.find("parameters"); parameters != j.end()) {
    if (!parameters->is_null() && !parameters->is_object()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "tool function 'parameters' must be an object or null");
    }

    if (parameters->is_object()) {
      f.parameters = *parameters;
    }
  }

  ReadStrict(j, f.strict);
}

void from_json(const nlohmann::json& j, ChatCompletionCustomToolDef& c) {
  c.name = RequiredName(j, "custom tool");
  RejectMember(j, "parameters", "custom tool");
  RejectMember(j, "strict", "custom tool");
  opt_str(j, "description", c.description);

  auto format = j.find("format");
  c.format = tools::ParseCustomToolFormat(format == j.end() ? nlohmann::json() : *format, c.name);
}

// Tool entries are polymorphic: only the member matching `type` is present, so nothing here may
// assume a "function" key exists. Unknown types are rejected rather than coerced into functions — a
// tool the runtime cannot represent must not be offered to the model as if it understood it.
void from_json(const nlohmann::json& j, ChatCompletionTool& t) {
  t.type = ToolTypeOf(j, "tool");

  if (t.type == "function") {
    RejectMember(j, "custom", "function tool");
    t.function = RequiredObject(j, "function", "tool").get<ChatCompletionFunctionDef>();
    return;
  }

  if (t.type == "custom") {
    RejectMember(j, "function", "custom tool");
    t.custom = RequiredObject(j, "custom", "tool").get<ChatCompletionCustomToolDef>();
    return;
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported tool type '", t.type,
           "'; expected 'function' or 'custom'");
}

void from_json(const nlohmann::json& j, ChatCompletionToolChoice& tc) {
  if (j.is_string()) {
    const auto mode = j.get<std::string>();

    if (mode == "auto") {
      tc.kind = ChatCompletionToolChoice::Kind::kAuto;
    } else if (mode == "none") {
      tc.kind = ChatCompletionToolChoice::Kind::kNone;
    } else if (mode == "required") {
      tc.kind = ChatCompletionToolChoice::Kind::kRequired;
    } else {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid tool_choice '", mode,
               "'; expected 'auto', 'none' or 'required'");
    }

    return;
  }

  if (!j.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool_choice must be a string or an object");
  }

  const auto type = ToolTypeOf(j, "tool_choice");

  if (type == "function") {
    tc.kind = ChatCompletionToolChoice::Kind::kFunction;
    tc.name = RequiredName(RequiredObject(j, "function", "tool_choice"), "tool_choice function");
    return;
  }

  if (type == "custom") {
    tc.kind = ChatCompletionToolChoice::Kind::kCustom;
    tc.name = RequiredName(RequiredObject(j, "custom", "tool_choice"), "tool_choice custom");
    return;
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported tool_choice type '", type,
           "'; expected 'function' or 'custom'");
}

void from_json(const nlohmann::json& j, ChatStreamOptions& s) {
  s.include_usage = j.value("include_usage", false);
}

void from_json(const nlohmann::json& j, ChatCompletionRequest& r) {
  // Required fields
  r.model = j.at("model").get<std::string>();
  r.messages = j.at("messages").get<std::vector<ChatCompletionMessage>>();

  // Optional scalar fields
  opt_float(j, "temperature", r.temperature);
  opt_float(j, "top_p", r.top_p);
  opt_int(j, "n", r.n);
  opt_bool(j, "stream", r.stream);
  opt_int(j, "max_tokens", r.max_tokens);
  opt_int(j, "max_completion_tokens", r.max_completion_tokens);
  opt_float(j, "presence_penalty", r.presence_penalty);
  opt_float(j, "frequency_penalty", r.frequency_penalty);
  opt_int(j, "seed", r.seed);
  opt_bool(j, "logprobs", r.logprobs);
  opt_int(j, "top_logprobs", r.top_logprobs);
  opt_bool(j, "parallel_tool_calls", r.parallel_tool_calls);
  opt_str(j, "user", r.user);

  // stream_options — object
  if (j.contains("stream_options") && j["stream_options"].is_object()) {
    r.stream_options = j["stream_options"].get<ChatStreamOptions>();
  }

  // Polymorphic fields — keep as raw JSON
  if (j.contains("stop") && !j["stop"].is_null()) {
    r.stop = j["stop"];
  }

  if (j.contains("tool_choice") && !j["tool_choice"].is_null()) {
    r.tool_choice = j["tool_choice"].get<ChatCompletionToolChoice>();
  }

  if (j.contains("response_format") && !j["response_format"].is_null()) {
    r.response_format = j["response_format"];
  }

  // tools — array of ChatCompletionTool
  if (j.contains("tools") && j["tools"].is_array()) {
    r.tools = j["tools"].get<std::vector<ChatCompletionTool>>();
  }

  // metadata — map<string, string>
  opt(j, "metadata", r.metadata);
}

// ========================================================================
// Request serialization (to_json) - for getting tools JSON to pass to GenAI
// ========================================================================

void to_json(nlohmann::json& j, const ChatCompletionFunctionDef& f) {
  j = nlohmann::json{
      {"name", f.name},
  };

  if (f.description.has_value()) {
    j["description"] = *f.description;
  }

  if (f.parameters.has_value()) {
    j["parameters"] = *f.parameters;
  }

  if (f.strict.has_value()) {
    j["strict"] = *f.strict;
  }
}

void to_json(nlohmann::json& j, const ChatCompletionCustomToolDef& c) {
  j = nlohmann::json{{"name", c.name}};

  if (c.description.has_value()) {
    j["description"] = *c.description;
  }

  j["format"] = c.format;
}

void to_json(nlohmann::json& j, const ChatCompletionTool& t) {
  j = nlohmann::json{{"type", t.type}};

  // Emit only the member that matches the kind — a custom tool has no "function" on the wire.
  if (t.custom.has_value()) {
    j["custom"] = *t.custom;
  } else {
    j["function"] = t.function;
  }
}

void to_json(nlohmann::json& j, const ChatCompletionToolChoice& tc) {
  switch (tc.kind) {
    case ChatCompletionToolChoice::Kind::kFunction:
      j = nlohmann::json{{"type", "function"}, {"function", {{"name", tc.name}}}};
      break;
    case ChatCompletionToolChoice::Kind::kCustom:
      j = nlohmann::json{{"type", "custom"}, {"custom", {{"name", tc.name}}}};
      break;
    default:
      j = tc.ModeString();
      break;
  }
}

void to_json(nlohmann::json& j, const ChatCompletionFunctionCall& f) {
  j = nlohmann::json{
      {"name", f.name},
      {"arguments", f.arguments},
  };
}

void to_json(nlohmann::json& j, const ChatCompletionCustomCall& c) {
  j = nlohmann::json{
      {"name", c.name},
      {"input", c.input},
  };
}

void to_json(nlohmann::json& j, const ChatCompletionToolCall& tc) {
  j = nlohmann::json{
      {"id", tc.id},
      {"type", tc.type},
  };

  // A custom call carries raw text under "custom"; a function call carries JSON under "function".
  // Emitting both would let a client read the payload through the wrong contract.
  if (tc.custom.has_value()) {
    j["custom"] = *tc.custom;
  } else {
    j["function"] = tc.function;
  }

  if (tc.index.has_value()) {
    j["index"] = *tc.index;
  }
}

void to_json(nlohmann::json& j, const ChatCompletionResponseMessage& m) {
  j = nlohmann::json{
      {"role", m.role},
      {"refusal", nullptr},
  };

  // content → null when tool_calls present and no content (per OpenAI spec)
  if (m.content.has_value()) {
    j["content"] = *m.content;
  } else {
    j["content"] = nullptr;
  }

  if (m.reasoning_content.has_value()) {
    j["reasoning_content"] = *m.reasoning_content;
  }

  if (m.tool_calls.has_value()) {
    j["tool_calls"] = *m.tool_calls;
  }
}

void to_json(nlohmann::json& j, const ChatCompletionChoice& c) {
  j = nlohmann::json{
      {"index", c.index},
      {"message", c.message},
      {"logprobs", nullptr},
      {"finish_reason", c.finish_reason},
  };

  if (c.logprobs.has_value()) {
    j["logprobs"] = *c.logprobs;
  }
}

void to_json(nlohmann::json& j, const PromptTokensDetails& d) {
  j = nlohmann::json{
      {"cached_tokens", d.cached_tokens},
  };
}

void to_json(nlohmann::json& j, const CompletionTokensDetails& d) {
  j = nlohmann::json{
      {"reasoning_tokens", d.reasoning_tokens},
  };
}

void to_json(nlohmann::json& j, const ChatCompletionUsage& u) {
  j = nlohmann::json{
      {"prompt_tokens", u.prompt_tokens},
      {"completion_tokens", u.completion_tokens},
      {"total_tokens", u.total_tokens},
      {"prompt_tokens_details", u.prompt_tokens_details},
      {"completion_tokens_details", u.completion_tokens_details},
  };
}

void to_json(nlohmann::json& j, const ChatCompletionResponse& r) {
  j = nlohmann::json{
      {"id", r.id},
      {"object", r.object},
      {"created", r.created},
      {"model", r.model},
      {"system_fingerprint", nullptr},
      {"choices", r.choices},
      {"usage", r.usage},
  };

  if (r.system_fingerprint.has_value()) {
    j["system_fingerprint"] = *r.system_fingerprint;
  }
}

// ========================================================================
// Streaming serialization (to_json)
// ========================================================================

void to_json(nlohmann::json& j, const ChatCompletionDelta& d) {
  j = nlohmann::json::object();

  // Only emit fields that have values
  if (d.role.has_value()) {
    j["role"] = *d.role;
  }

  if (d.content.has_value()) {
    j["content"] = *d.content;
  }

  if (d.reasoning_content.has_value()) {
    j["reasoning_content"] = *d.reasoning_content;
  }

  if (d.tool_calls.has_value()) {
    j["tool_calls"] = *d.tool_calls;
  }
}

void to_json(nlohmann::json& j, const ChatCompletionChunkChoice& c) {
  j = nlohmann::json{
      {"index", c.index},
      {"delta", c.delta},
      {"logprobs", nullptr},
      {"finish_reason", nullptr},
  };

  if (c.logprobs.has_value()) {
    j["logprobs"] = *c.logprobs;
  }

  if (c.finish_reason.has_value()) {
    j["finish_reason"] = *c.finish_reason;
  }
}

void to_json(nlohmann::json& j, const ChatCompletionChunk& c) {
  j = nlohmann::json{
      {"id", c.id},
      {"object", c.object},
      {"created", c.created},
      {"model", c.model},
      {"system_fingerprint", nullptr},
      {"choices", c.choices},
  };

  if (c.system_fingerprint.has_value()) {
    j["system_fingerprint"] = *c.system_fingerprint;
  }

  if (c.usage.has_value()) {
    j["usage"] = *c.usage;
  }
}

}  // namespace fl
