// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/chat_completions.h"

#include "util/json_helpers.h"

namespace fl {

// ========================================================================
// Request deserialization (from_json)
// ========================================================================

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

  if (j.contains("tool_calls") && !j["tool_calls"].is_null()) {
    m.tool_calls = j["tool_calls"];
  }
}

void from_json(const nlohmann::json& j, ChatCompletionFunctionDef& f) {
  f.name = j.at("name").get<std::string>();
  opt_str(j, "description", f.description);

  if (j.contains("parameters") && !j["parameters"].is_null()) {
    f.parameters = j["parameters"];
  }

  opt_bool(j, "strict", f.strict);
}

void from_json(const nlohmann::json& j, ChatCompletionTool& t) {
  t.type = j.value("type", "function");
  t.function = j.at("function").get<ChatCompletionFunctionDef>();
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
    r.tool_choice = j["tool_choice"];
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

void to_json(nlohmann::json& j, const ChatCompletionTool& t) {
  j = nlohmann::json{
      {"type", t.type},
      {"function", t.function},
  };
}

void to_json(nlohmann::json& j, const ChatCompletionFunctionCall& f) {
  j = nlohmann::json{
      {"name", f.name},
      {"arguments", f.arguments},
  };
}

void to_json(nlohmann::json& j, const ChatCompletionToolCall& tc) {
  j = nlohmann::json{
      {"id", tc.id},
      {"type", tc.type},
      {"function", tc.function},
  };

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
