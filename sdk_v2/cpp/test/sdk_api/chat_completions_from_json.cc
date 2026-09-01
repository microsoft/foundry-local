// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Response/streaming from_json implementations for test use.
// Production code only serializes responses (to_json); tests need to
// deserialize them for typed validation of HTTP responses.

#include "chat_completions.h"
#include "json_helpers.h"

namespace fl {

void from_json(const nlohmann::json& j, PromptTokensDetails& d) {
  d.cached_tokens = j.value("cached_tokens", 0);
}

void from_json(const nlohmann::json& j, CompletionTokensDetails& d) {
  d.reasoning_tokens = j.value("reasoning_tokens", 0);
}

void from_json(const nlohmann::json& j, ChatCompletionUsage& u) {
  u.prompt_tokens = j.at("prompt_tokens").get<int>();
  u.completion_tokens = j.at("completion_tokens").get<int>();
  u.total_tokens = j.at("total_tokens").get<int>();

  if (j.contains("prompt_tokens_details") && !j["prompt_tokens_details"].is_null()) {
    u.prompt_tokens_details = j["prompt_tokens_details"].get<PromptTokensDetails>();
  }

  if (j.contains("completion_tokens_details") && !j["completion_tokens_details"].is_null()) {
    u.completion_tokens_details = j["completion_tokens_details"].get<CompletionTokensDetails>();
  }
}

void from_json(const nlohmann::json& j, ChatCompletionFunctionCall& f) {
  f.name = j.at("name").get<std::string>();
  f.arguments = j.at("arguments").get<std::string>();
}

void from_json(const nlohmann::json& j, ChatCompletionToolCall& tc) {
  tc.id = j.at("id").get<std::string>();
  tc.type = j.value("type", std::string("function"));
  tc.function = j.at("function").get<ChatCompletionFunctionCall>();
}

void from_json(const nlohmann::json& j, ChatCompletionResponseMessage& m) {
  m.role = j.value("role", std::string("assistant"));
  opt_str(j, "content", m.content);
  opt_str(j, "reasoning_content", m.reasoning_content);
  opt_str(j, "refusal", m.refusal);
  opt(j, "tool_calls", m.tool_calls);
}

void from_json(const nlohmann::json& j, ChatCompletionChoice& c) {
  c.index = j.value("index", 0);
  c.message = j.at("message").get<ChatCompletionResponseMessage>();
  c.finish_reason = j.value("finish_reason", std::string());
}

void from_json(const nlohmann::json& j, ChatCompletionResponse& r) {
  r.id = j.at("id").get<std::string>();
  r.object = j.value("object", std::string("chat.completion"));
  r.created = j.at("created").get<int64_t>();
  r.model = j.at("model").get<std::string>();
  opt_str(j, "system_fingerprint", r.system_fingerprint);
  r.choices = j.at("choices").get<std::vector<ChatCompletionChoice>>();
  r.usage = j.at("usage").get<ChatCompletionUsage>();
}

void from_json(const nlohmann::json& j, ChatCompletionDelta& d) {
  opt_str(j, "role", d.role);
  opt_str(j, "content", d.content);
  opt_str(j, "reasoning_content", d.reasoning_content);
  opt(j, "tool_calls", d.tool_calls);
}

void from_json(const nlohmann::json& j, ChatCompletionChunkChoice& c) {
  c.index = j.value("index", 0);
  c.delta = j.at("delta").get<ChatCompletionDelta>();
  opt_str(j, "finish_reason", c.finish_reason);
}

void from_json(const nlohmann::json& j, ChatCompletionChunk& c) {
  c.id = j.at("id").get<std::string>();
  c.object = j.value("object", std::string("chat.completion.chunk"));
  c.created = j.at("created").get<int64_t>();
  c.model = j.at("model").get<std::string>();
  opt_str(j, "system_fingerprint", c.system_fingerprint);
  c.choices = j.at("choices").get<std::vector<ChatCompletionChunkChoice>>();
  opt(j, "usage", c.usage);
}

}  // namespace fl
