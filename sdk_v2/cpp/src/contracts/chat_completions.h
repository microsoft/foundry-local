// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// ========================================================================
// OpenAI Chat Completions API — Request/Response contract types.
// Mirrors Betalgo.Ranul.OpenAI ChatCompletionCreateRequest/Response
// extended by ChatCompletionCreateRequestExtended (metadata field).
// ========================================================================

// --- Request types ---

/// A single message in the conversation. Maps to OpenAI ChatMessage.
/// JSON keys: "role", "content", "name", "tool_call_id", "tool_calls"
struct ChatCompletionMessage {
  std::string role;                          // "system", "user", "assistant", "tool"
  std::optional<std::string> content;        // nullable for assistant messages with tool_calls
  std::optional<std::string> name;           // optional sender name
  std::optional<std::string> tool_call_id;   // for role="tool": the tool call this is responding to
  std::optional<nlohmann::json> tool_calls;  // for role="assistant": array of tool call objects
};

/// Function definition within a tool. JSON keys: "name", "description", "parameters", "strict"
struct ChatCompletionFunctionDef {
  std::string name;
  std::optional<std::string> description;
  std::optional<nlohmann::json> parameters;  // JSON Schema object
  std::optional<bool> strict;
};

/// A tool available to the model. JSON keys: "type", "function"
struct ChatCompletionTool {
  std::string type = "function";  // currently only "function"
  ChatCompletionFunctionDef function;
};

/// Stream options. JSON key: "include_usage"
struct ChatStreamOptions {
  bool include_usage = false;
};

/// The chat completion request. Maps to ChatCompletionCreateRequestExtended.
/// JSON keys match the OpenAI API specification.
/// Fields we intentionally skip (not relevant for local inference):
///   store, service_tier, reasoning_effort, audio, logit_bias, prediction,
///   web_search_options, modalities
struct ChatCompletionRequest {
  std::string model;                                           // "model"
  std::vector<ChatCompletionMessage> messages;                 // "messages"
  std::optional<float> temperature;                            // "temperature"
  std::optional<float> top_p;                                  // "top_p"
  std::optional<int> n;                                        // "n" — number of completions
  std::optional<bool> stream;                                  // "stream"
  std::optional<ChatStreamOptions> stream_options;             // "stream_options"
  std::optional<nlohmann::json> stop;                          // "stop" — string or array
  std::optional<int> max_tokens;                               // "max_tokens" (deprecated)
  std::optional<int> max_completion_tokens;                    // "max_completion_tokens"
  std::optional<float> presence_penalty;                       // "presence_penalty"
  std::optional<float> frequency_penalty;                      // "frequency_penalty"
  std::optional<std::vector<ChatCompletionTool>> tools;        // "tools"
  std::optional<nlohmann::json> tool_choice;                   // "tool_choice" — string or object
  std::optional<nlohmann::json> response_format;               // "response_format"
  std::optional<int> seed;                                     // "seed"
  std::optional<bool> logprobs;                                // "logprobs"
  std::optional<int> top_logprobs;                             // "top_logprobs"
  std::optional<bool> parallel_tool_calls;                     // "parallel_tool_calls"
  std::optional<std::string> user;                             // "user"
  std::optional<std::map<std::string, std::string>> metadata;  // "metadata" — from ChatCompletionCreateRequestExtended
};

// --- Response types ---

/// A function call in a tool call. JSON keys: "name", "arguments"
struct ChatCompletionFunctionCall {
  std::string name;
  std::string arguments;
};

/// A tool call from the assistant. JSON keys: "id", "type", "function"
struct ChatCompletionToolCall {
  std::string id;
  std::string type = "function";
  ChatCompletionFunctionCall function;
  std::optional<int> index;  // streaming only — distinguishes parallel tool calls
};

/// The message in a response choice. JSON keys: "role", "content", "reasoning_content", "refusal", "tool_calls"
struct ChatCompletionResponseMessage {
  std::string role = "assistant";
  std::optional<std::string> content;            // nullable when tool_calls present
  std::optional<std::string> reasoning_content;  // present only for reasoning models
  std::optional<std::string> refusal;            // null unless refusal
  std::optional<std::vector<ChatCompletionToolCall>> tool_calls;
};

/// A single choice in the response. JSON keys: "index", "message", "logprobs", "finish_reason"
struct ChatCompletionChoice {
  int index = 0;
  ChatCompletionResponseMessage message;
  std::optional<nlohmann::json> logprobs;  // null — we don't produce logprobs
  std::string finish_reason;               // "stop", "length", "tool_calls"
};

/// Detailed prompt token breakdown. JSON key: "cached_tokens"
struct PromptTokensDetails {
  int cached_tokens = 0;
};

/// Detailed completion token breakdown. JSON key: "reasoning_tokens"
struct CompletionTokensDetails {
  int reasoning_tokens = 0;
};

/// Token usage statistics. JSON keys: "prompt_tokens", "completion_tokens", "total_tokens", etc.
struct ChatCompletionUsage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
  int total_tokens = 0;
  PromptTokensDetails prompt_tokens_details;
  CompletionTokensDetails completion_tokens_details;
};

/// The complete chat completion response.
/// JSON keys: "id", "object", "created", "model", "system_fingerprint", "choices", "usage"
struct ChatCompletionResponse {
  std::string id;
  std::string object = "chat.completion";
  int64_t created = 0;
  std::string model;
  std::optional<std::string> system_fingerprint;
  std::vector<ChatCompletionChoice> choices;
  ChatCompletionUsage usage;
};

// --- Streaming types ---

/// Delta content in a streaming chunk. JSON keys: "role", "content", "reasoning_content", "tool_calls"
struct ChatCompletionDelta {
  std::optional<std::string> role;
  std::optional<std::string> content;
  std::optional<std::string> reasoning_content;  // present only for reasoning model chunks
  std::optional<std::vector<ChatCompletionToolCall>> tool_calls;
};

/// A single choice in a streaming chunk.
/// JSON keys: "index", "delta", "logprobs", "finish_reason"
struct ChatCompletionChunkChoice {
  int index = 0;
  ChatCompletionDelta delta;
  std::optional<nlohmann::json> logprobs;
  std::optional<std::string> finish_reason;  // null during streaming, set on final chunk
};

/// A streaming chunk response.
/// JSON keys: "id", "object", "created", "model", "system_fingerprint", "choices", "usage"
struct ChatCompletionChunk {
  std::string id;
  std::string object = "chat.completion.chunk";
  int64_t created = 0;
  std::string model;
  std::optional<std::string> system_fingerprint;
  std::vector<ChatCompletionChunkChoice> choices;
  std::optional<ChatCompletionUsage> usage;  // only present when stream_options.include_usage
};

// ========================================================================
// JSON serialization (nlohmann ADL)
// ========================================================================

// --- Request deserialization ---
void from_json(const nlohmann::json& j, ChatCompletionMessage& m);
void from_json(const nlohmann::json& j, ChatCompletionFunctionDef& f);
void from_json(const nlohmann::json& j, ChatCompletionTool& t);
void from_json(const nlohmann::json& j, ChatStreamOptions& s);
void from_json(const nlohmann::json& j, ChatCompletionRequest& r);

// --- Cheaper to re-serialize to pass tools json to GenAI ---
void to_json(nlohmann::json& j, const ChatCompletionFunctionDef& f);
void to_json(nlohmann::json& j, const ChatCompletionTool& t);

// --- Response serialization ---
void to_json(nlohmann::json& j, const ChatCompletionFunctionCall& f);
void to_json(nlohmann::json& j, const ChatCompletionToolCall& tc);
void to_json(nlohmann::json& j, const ChatCompletionResponseMessage& m);
void to_json(nlohmann::json& j, const ChatCompletionChoice& c);
void to_json(nlohmann::json& j, const PromptTokensDetails& d);
void to_json(nlohmann::json& j, const CompletionTokensDetails& d);
void to_json(nlohmann::json& j, const ChatCompletionUsage& u);
void to_json(nlohmann::json& j, const ChatCompletionResponse& r);

// --- Streaming serialization ---
void to_json(nlohmann::json& j, const ChatCompletionDelta& d);
void to_json(nlohmann::json& j, const ChatCompletionChunkChoice& c);
void to_json(nlohmann::json& j, const ChatCompletionChunk& c);

}  // namespace fl
