// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

// ========================================================================
// OpenAI Chat Completions API — Request/Response contract types.
// Mirrors Betalgo.Ranul.OpenAI ChatCompletionCreateRequest/Response
// extended by ChatCompletionCreateRequestExtended (metadata field).
// ========================================================================

// --- Tool call types (shared by request transcripts and responses) ---

/// A function call's payload. JSON keys: "name", "arguments"
struct ChatCompletionFunctionCall {
  std::string name;
  std::string arguments;  // JSON object, as a string
};

/// A custom tool call's payload. JSON keys: "name", "input"
struct ChatCompletionCustomCall {
  std::string name;
  std::string input;  // raw text, exactly as the model produced it
};

/// A tool call. JSON keys: "id", "type", "function" | "custom"
///
/// The same shape appears in an assistant message the client echoes back and in the calls a
/// response reports, so both directions share one type. `type` selects which payload member is
/// meaningful: a function call carries JSON `arguments`, a custom call carries raw `input`.
struct ChatCompletionToolCall {
  std::string id;
  std::string type = "function";  // "function" or "custom"
  ChatCompletionFunctionCall function;
  std::optional<ChatCompletionCustomCall> custom;
  std::optional<int> index;  // streaming only — distinguishes parallel tool calls

  bool IsCustom() const { return custom.has_value(); }

  /// The called tool's name, whichever kind of call this is.
  const std::string& Name() const { return custom.has_value() ? custom->name : function.name; }

  /// The payload the tool receives: JSON arguments for a function, raw text for a custom tool.
  const std::string& Payload() const { return custom.has_value() ? custom->input : function.arguments; }

  static ChatCompletionToolCall MakeFunction(std::string id, std::string name, std::string arguments);
  static ChatCompletionToolCall MakeCustom(std::string id, std::string name, std::string input);
};

// --- Request types ---

/// A single message in the conversation. Maps to OpenAI ChatMessage.
/// JSON keys: "role", "content", "name", "tool_call_id", "tool_calls"
struct ChatCompletionMessage {
  ChatCompletionMessage() = default;

  ChatCompletionMessage(std::string role_in, std::optional<std::string> content_in,
                        std::optional<std::string> name_in, std::optional<std::string> tool_call_id_in,
                        std::vector<ChatCompletionToolCall> tool_calls_in,
                        std::optional<std::string> reasoning_content_in = {})
      : role(std::move(role_in)),
        content(std::move(content_in)),
        name(std::move(name_in)),
        tool_call_id(std::move(tool_call_id_in)),
        tool_calls(std::move(tool_calls_in)),
        reasoning_content(std::move(reasoning_content_in)) {}

  std::string role;                         // "system", "user", "assistant", "tool"
  std::optional<std::string> content;       // nullable for assistant messages with tool_calls
  std::optional<std::string> name;          // optional sender name
  std::optional<std::string> tool_call_id;  // for role="tool": the tool call this is responding to
  // Parsed into the same typed shape the response side emits, so a transcript round-trips through
  // one representation and a custom call keeps its raw text instead of being read as JSON arguments.
  std::vector<ChatCompletionToolCall> tool_calls;  // for role="assistant": the calls this message issued
  std::optional<std::string> reasoning_content;    // replay marker only; never projected into the model prompt
};

/// Function definition within a tool. JSON keys: "name", "description", "parameters", "strict"
struct ChatCompletionFunctionDef {
  std::string name;
  std::optional<std::string> description;
  std::optional<nlohmann::json> parameters;  // JSON Schema object
  std::optional<bool> strict;
};

/// Custom (free-form) tool definition, nested under "custom". JSON keys: "name", "description", "format"
///
/// A custom tool takes a single raw text payload rather than a JSON argument object.
struct ChatCompletionCustomToolDef {
  std::string name;
  std::optional<std::string> description;
  nlohmann::json format = {{"type", "text"}};
};

/// A tool available to the model. JSON keys: "type", "function" | "custom"
/// `type` selects which member carries the definition; the other is meaningless.
struct ChatCompletionTool {
  std::string type = "function";  // "function" or "custom"
  ChatCompletionFunctionDef function;
  std::optional<ChatCompletionCustomToolDef> custom;

  bool IsCustom() const { return custom.has_value(); }

  /// The declared name, whichever kind of tool this is.
  const std::string& Name() const { return custom.has_value() ? custom->name : function.name; }
};

/// Parsed "tool_choice": the mode strings "auto"/"none"/"required", or an object forcing one named
/// tool — {"type":"function","function":{"name":..}} / {"type":"custom","custom":{"name":..}}.
struct ChatCompletionToolChoice {
  enum class Kind { kAuto,
                    kNone,
                    kRequired,
                    kFunction,
                    kCustom };

  Kind kind = Kind::kAuto;
  std::string name;  // forced tool name; empty unless kind is kFunction or kCustom

  bool IsForced() const { return kind == Kind::kFunction || kind == Kind::kCustom; }

  /// The session-option value: "auto", "none" or "required". Forcing a tool implies "required".
  std::string ModeString() const;
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
  std::optional<float> presence_penalty;                       // "presence_penalty"; only 0 is supported
  std::optional<float> frequency_penalty;                      // "frequency_penalty"; only 0 is supported
  std::optional<std::vector<ChatCompletionTool>> tools;        // "tools"
  std::optional<ChatCompletionToolChoice> tool_choice;         // "tool_choice" — string or object
  std::optional<nlohmann::json> response_format;               // "response_format"
  std::optional<int> seed;                                     // "seed"
  std::optional<bool> logprobs;                                // "logprobs"
  std::optional<int> top_logprobs;                             // "top_logprobs"
  std::optional<bool> parallel_tool_calls;                     // "parallel_tool_calls"
  std::optional<std::string> user;                             // "user"
  std::optional<std::map<std::string, std::string>> metadata;  // "metadata" — from ChatCompletionCreateRequestExtended
};

// --- Response types ---

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
void from_json(const nlohmann::json& j, ChatCompletionFunctionCall& f);
void from_json(const nlohmann::json& j, ChatCompletionCustomCall& c);
void from_json(const nlohmann::json& j, ChatCompletionToolCall& tc);
void from_json(const nlohmann::json& j, ChatCompletionMessage& m);
void from_json(const nlohmann::json& j, ChatCompletionFunctionDef& f);
void from_json(const nlohmann::json& j, ChatCompletionCustomToolDef& c);
void from_json(const nlohmann::json& j, ChatCompletionTool& t);
void from_json(const nlohmann::json& j, ChatCompletionToolChoice& tc);
void from_json(const nlohmann::json& j, ChatStreamOptions& s);
void from_json(const nlohmann::json& j, ChatCompletionRequest& r);

// --- Request serialization (round-trips the tool declarations tests and clients compare against) ---
void to_json(nlohmann::json& j, const ChatCompletionFunctionDef& f);
void to_json(nlohmann::json& j, const ChatCompletionCustomToolDef& c);
void to_json(nlohmann::json& j, const ChatCompletionTool& t);
void to_json(nlohmann::json& j, const ChatCompletionToolChoice& tc);

// --- Response serialization ---
void to_json(nlohmann::json& j, const ChatCompletionFunctionCall& f);
void to_json(nlohmann::json& j, const ChatCompletionCustomCall& c);
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
