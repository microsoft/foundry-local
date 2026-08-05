// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fl {
namespace responses {

// ---------------------------------------------------------------------------
// Input content types
// ---------------------------------------------------------------------------

struct InputTextContent {
  std::string text;
};

struct InputImageContent {
  std::string detail;
  std::optional<std::string> image_url;
  // Foundry Local v1 extension: raw base64 image payload. image_url takes precedence when both are supplied.
  std::optional<std::string> image_data;
  std::optional<std::string> file_id;
  // Optional MIME type ("image/png", "image/jpeg", ...). Required by the
  // OpenAI spec when `image_url` is anything other than a `data:` URL,
  // because data URLs already carry their media type inline. The converter
  // (response_converter.cc) enforces this when it routes images through
  // the vision pipeline.
  std::optional<std::string> media_type;
};

struct InputFileContent {
  std::optional<std::string> file_id;
  std::optional<std::string> file_data;
  std::optional<std::string> filename;
};

struct InputAudioContent {
  std::string data;
  std::string format;
};

using InputContent = std::variant<InputTextContent, InputImageContent,
                                  InputFileContent, InputAudioContent>;

// ---------------------------------------------------------------------------
// Input items
// ---------------------------------------------------------------------------

struct InputMessage {
  std::string role;
  std::vector<InputContent> content;

  /// Factory: create a user message with text content.
  static InputMessage UserMessage(const std::string& text);
  /// Factory: create a developer (system) message with text content.
  static InputMessage DeveloperMessage(const std::string& text);
  /// Factory: create an assistant message with text content.
  static InputMessage AssistantMessage(const std::string& text);
};

struct FunctionCallResultInputItem {
  std::string type = "function_call_output";
  std::string call_id;
  std::string output;
};

using InputItem = std::variant<InputMessage, FunctionCallResultInputItem>;

// ---------------------------------------------------------------------------
// Tool calling types (AD-010)
// ---------------------------------------------------------------------------

/// JSON schema property definition.
/// AD-007: items and properties are stored as JSON strings, not nlohmann::json.
struct PropertyDefinition {
  std::string type;
  std::optional<std::string> description;
  std::optional<std::vector<std::string>> enum_values;
  std::optional<std::string> items_json;
  std::optional<std::string> properties_json;
};

/// A function that can be called by the model.
/// AD-007: parameters is a JSON string, not nlohmann::json.
struct FunctionDefinition {
  std::string name;
  std::optional<std::string> description;
  std::optional<std::string> parameters_json;
  std::optional<bool> strict;
};

/// A tool the model may use during generation.
struct ToolDefinition {
  std::string type = "function";
  FunctionDefinition function;
};

/// Forces the model to call a specific function.
struct ForcedFunction {
  std::string name;
};

/// How the model should choose tools.
/// string values: "auto", "none", "required"
using ToolChoice = std::variant<std::string, ForcedFunction>;

// ---------------------------------------------------------------------------
// Request parameters
// ---------------------------------------------------------------------------

struct ResponseTextConfig {
  std::string format;
  std::optional<std::string> json_schema;  // AD-007: JSON string
  std::optional<std::string> lark_grammar;
};

struct ReasoningConfig {
  std::optional<std::string> effort;
  std::optional<bool> generate_summary;
};

struct ResponseCreateParams {
  std::string model;
  std::variant<std::string, std::vector<InputItem>> input;
  std::optional<std::string> instructions;
  std::optional<std::string> previous_response_id;
  std::optional<float> temperature;
  std::optional<int> max_output_tokens;
  std::optional<float> top_p;
  std::optional<float> presence_penalty;
  std::optional<float> frequency_penalty;
  std::optional<int> seed;
  bool stream = false;
  bool store = false;
  std::optional<ResponseTextConfig> text;
  std::optional<std::vector<ToolDefinition>> tools;
  std::optional<ToolChoice> tool_choice;
  std::optional<std::vector<std::string>> allowed_tools;
  std::optional<bool> parallel_tool_calls;
  std::optional<ReasoningConfig> reasoning;
  std::unordered_map<std::string, std::string> metadata;
  std::optional<std::string> user;
  std::optional<std::string> extra_json;  // AD-007: arbitrary JSON extension
};

// ---------------------------------------------------------------------------
// Output types
// ---------------------------------------------------------------------------

enum class ResponseStatus {
  kInProgress,
  kCompleted,
  kFailed,
  kCancelled,
  kIncomplete,
};

struct OutputTextContent {
  std::string text;
};

struct OutputRefusalContent {
  std::string refusal;
};

struct OutputAudioContent {
  std::string data;
  std::string transcript;
};

using OutputContent = std::variant<OutputTextContent, OutputRefusalContent,
                                   OutputAudioContent>;

struct ResponseOutputMessage {
  std::string id;
  std::string role;
  ResponseStatus status = ResponseStatus::kInProgress;
  std::vector<OutputContent> content;
};

struct FunctionCallOutputItem {
  std::string id;
  std::string type = "function_call";
  std::string call_id;
  std::string name;
  std::string arguments;
  ResponseStatus status = ResponseStatus::kInProgress;
};

// Reasoning output item (OpenAI Responses API). Surfaces chain-of-thought text emitted between the model's
// reasoning markers (e.g. `<think>...</think>`) as a typed output entry. The `summary` array models the
// OpenAI shape one-to-one; for our v1 we always emit a single `summary_text` part containing the full
// reasoning trace, since we do not produce a separate condensed summary.
struct ReasoningSummaryText {
  std::string text;
};

struct ReasoningOutputItem {
  std::string id;
  std::vector<ReasoningSummaryText> summary;
  ResponseStatus status = ResponseStatus::kInProgress;
};

using ResponseOutputItem = std::variant<ResponseOutputMessage, FunctionCallOutputItem, ReasoningOutputItem>;

struct InputTokensDetails {
  int cached_tokens = 0;
};

struct OutputTokensDetails {
  int reasoning_tokens = 0;
};

struct ResponseUsage {
  int input_tokens = 0;
  int output_tokens = 0;
  int total_tokens = 0;
  InputTokensDetails input_tokens_details;
  OutputTokensDetails output_tokens_details;
};

struct ResponseError {
  std::string code;
  std::string message;
};

/// Full Responses API response object.
/// Named ResponseObject to avoid collision with the internal session Response.
struct ResponseObject {
  std::string id;
  int64_t created_at = 0;
  std::optional<int64_t> completed_at;
  std::optional<int64_t> failed_at;
  std::optional<int64_t> cancelled_at;
  std::string model;
  ResponseStatus status = ResponseStatus::kInProgress;
  std::optional<std::string> previous_response_id;
  std::optional<std::string> instructions;
  std::vector<ResponseOutputItem> output;
  std::string output_text;
  ResponseUsage usage;
  std::optional<ResponseError> error;
  std::optional<std::string> incomplete_reason;

  // Echoed request parameters
  std::vector<ToolDefinition> tools;
  std::optional<ToolChoice> tool_choice;
  std::optional<float> temperature;
  std::optional<float> top_p;
  std::optional<float> presence_penalty;
  std::optional<float> frequency_penalty;
  std::optional<int> max_output_tokens;
  bool parallel_tool_calls = true;
  bool store = false;
  std::unordered_map<std::string, std::string> metadata;
  std::optional<std::string> user;
  std::optional<ResponseTextConfig> text;
  std::string truncation = "disabled";
  std::optional<ReasoningConfig> reasoning;
};

// ---------------------------------------------------------------------------
// Streaming (AD-003: callbacks for streaming)
// ---------------------------------------------------------------------------

enum class StreamEventType {
  kResponseCreated,
  kResponseInProgress,
  kResponseCompleted,
  kResponseFailed,
  kResponseIncomplete,
  kOutputItemAdded,
  kOutputItemDone,
  kContentPartAdded,
  kContentPartDone,
  kTextDelta,
  kTextDone,
  kRefusalDelta,
  kRefusalDone,
  kAudioDelta,
  kAudioDone,
  kAudioTranscriptDelta,
  kAudioTranscriptDone,
  kFunctionCallArgumentsDelta,
  kFunctionCallArgumentsDone,
  kReasoningDelta,
  kReasoningDone,
  kError,
};

struct StreamEvent {
  StreamEventType type = StreamEventType::kError;
  int sequence_number = 0;
  std::string delta;
  int output_index = 0;
  int content_index = 0;
  std::string item_id;

  std::optional<ResponseObject> response;
  std::optional<ResponseOutputItem> item;
  std::optional<OutputContent> content_part;
  std::optional<std::string> text;

  // Function call streaming fields
  std::optional<std::string> function_name;
  std::optional<std::string> function_call_id;
  std::optional<std::string> function_arguments;

  // Error fields
  std::optional<std::string> error_code;
  std::optional<std::string> error_message;
};

// ========================================================================
// JSON serialization (nlohmann ADL)
// ========================================================================

// --- Enum string helpers ---
std::string ResponseStatusToString(ResponseStatus status);
ResponseStatus ResponseStatusFromString(const std::string& s);
std::string StreamEventTypeToString(StreamEventType type);

// --- Input content from_json (request deserialization) ---
void from_json(const nlohmann::json& j, InputTextContent& c);
void from_json(const nlohmann::json& j, InputImageContent& c);
void from_json(const nlohmann::json& j, InputFileContent& c);
void from_json(const nlohmann::json& j, InputAudioContent& c);
void from_json(const nlohmann::json& j, InputMessage& m);
void from_json(const nlohmann::json& j, FunctionCallResultInputItem& f);

// --- Tool types from_json ---
void from_json(const nlohmann::json& j, FunctionDefinition& f);
void from_json(const nlohmann::json& j, ToolDefinition& t);
void from_json(const nlohmann::json& j, ForcedFunction& f);

// --- Request from_json ---
void from_json(const nlohmann::json& j, ResponseTextConfig& c);
void from_json(const nlohmann::json& j, ReasoningConfig& c);
void from_json(const nlohmann::json& j, ResponseCreateParams& p);

// --- Config types to_json (for echoing in response) ---
void to_json(nlohmann::json& j, const ResponseTextConfig& c);
void to_json(nlohmann::json& j, const ReasoningConfig& c);

// --- Output to_json (response serialization) ---
void to_json(nlohmann::json& j, const OutputTextContent& c);
void to_json(nlohmann::json& j, const OutputRefusalContent& c);
void to_json(nlohmann::json& j, const OutputAudioContent& c);
void to_json(nlohmann::json& j, const ResponseOutputMessage& m);
void to_json(nlohmann::json& j, const FunctionCallOutputItem& f);
void to_json(nlohmann::json& j, const ReasoningSummaryText& s);
void to_json(nlohmann::json& j, const ReasoningOutputItem& r);
void to_json(nlohmann::json& j, const InputTokensDetails& d);
void to_json(nlohmann::json& j, const OutputTokensDetails& d);
void to_json(nlohmann::json& j, const ResponseUsage& u);
void to_json(nlohmann::json& j, const ResponseError& e);
void to_json(nlohmann::json& j, const ResponseObject& r);

// --- Tool types to_json (for echoing in response) ---
void to_json(nlohmann::json& j, const FunctionDefinition& f);
void to_json(nlohmann::json& j, const ToolDefinition& t);
void to_json(nlohmann::json& j, const ForcedFunction& f);

// --- Streaming to_json ---
void to_json(nlohmann::json& j, const StreamEvent& e);

}  // namespace responses
}  // namespace fl
