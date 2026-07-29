// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/openresponses/response_converter.h"

#include <azure/core/base64.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>

namespace fl {
namespace responses {
// ---------------------------------------------------------------------------
// Field-audit assertions.
//
// These static_asserts intentionally break the build when ResponseCreateParams
// or ResponseObject grows a new field. The purpose is to force a reviewer to
// audit the three converter entry points whenever the request/response shape
// changes:
//
//   - ResponseCreateParams → audit ToSessionRequest and
//     ExtractResponsesToolDefinitions (does the new field map to a session
//     option, a tool filter, or is it intentionally dropped?).
//   - ResponseObject → audit EchoRequestParams and BuildResponseObject (does
//     the new field need to be echoed back to the client?).
//
// When you intentionally add (or remove) a field, update the converters as
// needed and then update the byte counts below. The number itself is not
// meaningful — only the change-detection is.
//
// We compile this check only on MSVC release-style builds (NDEBUG defined).
// Other toolchains and Debug builds have different std::string/vector
// internal layouts; gating to one canonical configuration keeps the check
// stable while still catching every meaningful field addition on the primary
// development platform.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER) && defined(NDEBUG) && defined(_WIN64)
static_assert(sizeof(ResponseCreateParams) == 648,
              "ResponseCreateParams size changed. A new field was likely added — "
              "review ToSessionRequest, ExtractResponsesToolDefinitions, and EchoRequestParams "
              "to ensure the new field is handled (or explicitly skipped). "
              "Update the expected size once those converters have been audited.");

static_assert(sizeof(ResponseObject) == 832,
              "ResponseObject size changed. A new field was likely added — "
              "review EchoRequestParams and BuildResponseObject to ensure the new field is "
              "populated (or explicitly skipped). "
              "Update the expected size once those builders have been audited.");
#endif
}  // namespace responses
}  // namespace fl

#include "exception.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"
#include "utils.h"

namespace fl {
namespace ResponseConverter {

// ---------------------------------------------------------------------------
// ID generation — random hex for uniqueness (matches C# Guid-based IDs)
// ---------------------------------------------------------------------------

std::string GenerateId(const std::string& prefix) {
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

  std::ostringstream ss;
  ss << prefix << "_" << std::hex;
  ss << dist(rng) << dist(rng) << dist(rng) << dist(rng);
  return ss.str();
}

// ---------------------------------------------------------------------------
// Helper: add items from a JSON array to a session request
// (used for previous context from the store, which is JSON)
// ---------------------------------------------------------------------------

static void AddJsonItemsToRequest(Request& request, const nlohmann::json& items) {
  for (const auto& entry : items) {
    if (!entry.is_object()) {
      continue;
    }

    std::string type = entry.value("type", "");
    std::string role = entry.value("role", "");

    if (type == "function_call_output") {
      request.AddOwnedItem(std::make_unique<ToolResultItem>(entry.value("call_id", ""),
                                                            entry.value("output", "")));
      continue;
    }

    if (type == "function_call") {
      request.AddOwnedItem(std::make_unique<ToolCallItem>(entry.value("call_id", ""),
                                                          entry.value("name", ""),
                                                          entry.value("arguments", "")));
      continue;
    }

    if (role.empty()) {
      continue;
    }

    std::string text_content;
    if (entry.contains("content")) {
      const auto& content = entry["content"];
      if (content.is_string()) {
        text_content = content.get<std::string>();
      } else if (content.is_array()) {
        for (const auto& part : content) {
          if (part.is_object()) {
            std::string part_type = part.value("type", "");
            if (part_type == "input_text" || part_type == "text" ||
                part_type == "output_text") {
              text_content += part.value("text", "");
            }
          }
        }
      }
    }

    auto r = Utils::StringToRole(role);
    if (r == FOUNDRY_LOCAL_ROLE_DEVELOPER) {
      r = FOUNDRY_LOCAL_ROLE_SYSTEM;
    }

    if (text_content.empty()) {
      // Skip messages with no extractable text content.
      continue;
    }

    auto i = std::make_unique<MessageItem>(r, text_content);
    request.AddOwnedItem(std::move(i));
  }
}

// ---------------------------------------------------------------------------
// Helper: turn an InputImageContent into an owning ImageItem.
//
// Supports two source forms:
//   - data URLs:  "data:<mime-type>;base64,<payload>"
//                 → base64-decoded into an owning byte buffer.
//   - local files: absolute path or a `file://` URI
//                 → bytes read from disk.
//
// Anything else (http/https URL, file_id reference) is rejected with
// NOT_IMPLEMENTED. Matches upstream C# ResponseInputConverter behaviour:
// only data URLs and local files are supported in the on-device runtime.
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view kDataUrlPrefix = "data:";
constexpr std::string_view kFileScheme = "file://";

// Infer a MIME type from a file extension. Lowercase comparison; returns
// empty string when the extension is unknown — caller decides whether to
// fall back to a generic "image" placeholder or error out.
std::string MimeFromExtension(const std::string& path) {
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos) {
    return {};
  }

  std::string ext = path.substr(dot + 1);
  for (auto& ch : ext) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "bmp") return "image/bmp";
  return {};
}

std::unique_ptr<ImageItem> MakeImageItemFromDataUrl(const std::string& url) {
  // Format: "data:<media-type>;base64,<payload>"
  // Strict: we require base64 encoding (not raw text) and a `;base64,`
  // marker. Anything else throws — vision models can't consume non-base64
  // data URLs.
  auto comma = url.find(',');
  if (comma == std::string::npos) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "image_url data URL is missing payload separator ','");
  }

  std::string header = url.substr(kDataUrlPrefix.size(), comma - kDataUrlPrefix.size());

  constexpr std::string_view kBase64Marker = ";base64";
  auto base64_pos = header.find(kBase64Marker);
  if (base64_pos == std::string::npos) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "image_url data URL must use ';base64,' encoding");
  }

  std::string media_type = header.substr(0, base64_pos);
  std::string payload = url.substr(comma + 1);

  std::vector<std::uint8_t> bytes;
  try {
    bytes = Azure::Core::Convert::Base64Decode(payload);
  } catch (const std::exception& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             std::string("image_url data URL has invalid base64 payload: ") + e.what());
  }

  if (bytes.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "image_url data URL decoded to zero bytes");
  }

  return std::make_unique<ImageItem>(std::move(bytes), std::move(media_type));
}

std::unique_ptr<ImageItem> MakeImageItemFromLocalFile(const std::string& url,
                                                      const std::optional<std::string>& explicit_media_type) {
  std::string path = url;
  if (path.compare(0, kFileScheme.size(), kFileScheme) == 0) {
    path.erase(0, kFileScheme.size());
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             std::string("image_url local file not found: ") + path);
  }

  in.seekg(0, std::ios::end);
  auto size = in.tellg();
  in.seekg(0, std::ios::beg);

  if (size <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             std::string("image_url local file is empty: ") + path);
  }

  std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
  if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             std::string("image_url local file read failed: ") + path);
  }

  std::string media_type = explicit_media_type.value_or(MimeFromExtension(path));
  return std::make_unique<ImageItem>(std::move(bytes), std::move(media_type));
}

std::unique_ptr<ImageItem> MakeImageItemFromInputImage(const InputImageContent& c) {
  if (c.file_id.has_value() && !c.file_id->empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED,
             "image input via file_id is not supported on Foundry Local");
  }

  if (!c.image_url.has_value() || c.image_url->empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "input_image content requires a non-empty image_url or file_id");
  }

  const std::string& url = *c.image_url;

  if (url.compare(0, kDataUrlPrefix.size(), kDataUrlPrefix) == 0) {
    return MakeImageItemFromDataUrl(url);
  }

  // file:// URI or absolute local path.
  if (url.compare(0, kFileScheme.size(), kFileScheme) == 0 ||
      (url.size() >= 2 && (url[0] == '/' || url[0] == '\\' ||
                           (url.size() >= 3 && url[1] == ':' && (url[2] == '/' || url[2] == '\\'))))) {
    return MakeImageItemFromLocalFile(url, c.media_type);
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED,
           "image_url must be a data: URL or a local file path; remote http(s) URLs are not supported");
}

}  // namespace

// ---------------------------------------------------------------------------
// Helper: add typed InputItems to a session request
// ---------------------------------------------------------------------------

static void AddTypedInputItems(Request& request,
                               const std::vector<InputItem>& input_items) {
  for (const auto& input_item : input_items) {
    if (auto* fc_result = std::get_if<FunctionCallResultInputItem>(&input_item)) {
      auto i = std::make_unique<ToolResultItem>(fc_result->call_id, fc_result->output);
      request.AddOwnedItem(std::move(i));
    } else if (auto* msg = std::get_if<InputMessage>(&input_item)) {
      // Build typed parts from the message's content array. Text and image
      // parts are forwarded; other content variants (file, audio) are
      // rejected at the converter so we fail fast rather than silently
      // dropping content.
      std::vector<std::unique_ptr<Item>> parts;
      bool has_text = false;
      for (const auto& c : msg->content) {
        if (auto* tc = std::get_if<InputTextContent>(&c)) {
          if (!tc->text.empty()) {
            parts.push_back(std::make_unique<TextItem>(tc->text));
            has_text = true;
          }
        } else if (auto* ic = std::get_if<InputImageContent>(&c)) {
          parts.push_back(MakeImageItemFromInputImage(*ic));
        } else {
          FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED,
                   "input message content type not supported (only input_text and input_image)");
        }
      }

      // Empty messages (no usable content) are silently skipped to match
      // the prior behaviour — callers occasionally send messages with only
      // tool-call follow-ups and no text.
      if (parts.empty()) {
        continue;
      }

      // The chat template requires every message to carry at least one
      // text part. A pure-image message (e.g. "input_image" with no
      // accompanying "input_text") would render as an empty content
      // string. Inject a single space so the template still renders the
      // message and the model receives the image sentinel.
      if (!has_text) {
        parts.push_back(std::make_unique<TextItem>(" "));
      }

      auto i = std::make_unique<MessageItem>(Utils::StringToRole(msg->role), std::move(parts));
      request.AddOwnedItem(std::move(i));
    }
  }
}

// ---------------------------------------------------------------------------
// ToSessionRequest — typed params version
// ---------------------------------------------------------------------------

Request ToSessionRequest(const ResponseCreateParams& params,
                         const nlohmann::json* previous_input,
                         const nlohmann::json* previous_output) {
  Request request;

  // Instructions → system message
  if (params.instructions.has_value() && !params.instructions->empty()) {
    auto i = std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_SYSTEM, *params.instructions);
    request.AddOwnedItem(std::move(i));
  }

  // Add previous context (for conversation chaining via previous_response_id)
  if (previous_input && previous_input->is_array()) {
    AddJsonItemsToRequest(request, *previous_input);
  }

  if (previous_output && previous_output->is_array()) {
    AddJsonItemsToRequest(request, *previous_output);
  }

  // Parse current input — variant dispatch
  if (auto* str_input = std::get_if<std::string>(&params.input)) {
    if (!str_input->empty()) {
      auto i = std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, *str_input);
      request.AddOwnedItem(std::move(i));
    }
  } else if (auto* items_input = std::get_if<std::vector<InputItem>>(&params.input)) {
    AddTypedInputItems(request, *items_input);
  }

  // Map parameters
  if (params.temperature.has_value()) {
    request.options["temperature"] = std::to_string(*params.temperature);
  }

  if (params.top_p.has_value()) {
    request.options["top_p"] = std::to_string(*params.top_p);
  }

  if (params.max_output_tokens.has_value()) {
    request.options["max_output_tokens"] =
        std::to_string(*params.max_output_tokens);
  }

  if (params.presence_penalty.has_value()) {
    request.options["presence_penalty"] =
        std::to_string(*params.presence_penalty);
  }

  if (params.frequency_penalty.has_value()) {
    request.options["frequency_penalty"] =
        std::to_string(*params.frequency_penalty);
  }

  if (params.seed.has_value()) {
    request.options["seed"] = std::to_string(*params.seed);
  }

  // Text format / grammar guidance → metadata parameters
  if (params.text.has_value()) {
    const auto& text_cfg = *params.text;

    if (text_cfg.format == "json_schema" && text_cfg.json_schema.has_value()) {
      request.options["guidance_type"] = "json_schema";
      request.options["guidance_data"] = *text_cfg.json_schema;
    } else if (text_cfg.format == "lark_grammar" &&
               text_cfg.lark_grammar.has_value()) {
      request.options["guidance_type"] = "lark_grammar";
      request.options["guidance_data"] = *text_cfg.lark_grammar;
    } else if (text_cfg.format == "regex" && text_cfg.lark_grammar.has_value()) {
      request.options["guidance_type"] = "regex";
      request.options["guidance_data"] = *text_cfg.lark_grammar;
    }
  }

  return request;
}

// ---------------------------------------------------------------------------
// ExtractResponsesToolDefinitions — mirrors chat_completions::ExtractToolDefinitions
// ---------------------------------------------------------------------------

namespace {

// Serialize a Responses ToolDefinition into the chat-template (OpenAI nested) format
// that ChatSession::BuildToolCallContext expects for pre-serialized tools arrays.
// Fully qualified to disambiguate from fl::ToolDefinition (session-side struct).
nlohmann::json ToChatTemplateTool(const responses::ToolDefinition& td) {
  nlohmann::json tool;
  tool["type"] = td.type.empty() ? "function" : td.type;
  tool["function"]["name"] = td.function.name;

  if (td.function.description.has_value()) {
    tool["function"]["description"] = *td.function.description;
  }

  if (td.function.parameters_json.has_value() && !td.function.parameters_json->empty()) {
    tool["function"]["parameters"] = nlohmann::json::parse(*td.function.parameters_json);
  }

  if (td.function.strict.has_value()) {
    tool["function"]["strict"] = *td.function.strict;
  }

  return tool;
}

nlohmann::json SerializeTools(const std::vector<responses::ToolDefinition>& tools) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& td : tools) {
    arr.push_back(ToChatTemplateTool(td));
  }
  return arr;
}

}  // namespace

std::string ExtractResponsesToolDefinitions(const ResponseCreateParams& params, Request& session_request) {
  // Start with the full tool set the caller declared.
  std::vector<responses::ToolDefinition> filtered;
  if (params.tools.has_value()) {
    filtered = *params.tools;
  }

  // tool_choice: string variants ("auto"/"none"/"required") flow straight to options.
  // ForcedFunction additionally narrows the tool set to the named function and forces "required",
  // matching chat-completions SetToolChoice behaviour.
  if (params.tool_choice.has_value()) {
    std::visit([&](const auto& tc) {
      using T = std::decay_t<decltype(tc)>;

      if constexpr (std::is_same_v<T, std::string>) {
        session_request.options["tool_choice"] = tc;
      } else if constexpr (std::is_same_v<T, ForcedFunction>) {
        session_request.options["tool_choice"] = "required";

        std::vector<responses::ToolDefinition> only;
        for (const auto& tool : filtered) {
          if (tool.function.name == tc.name) {
            only.push_back(tool);
          }
        }
        filtered = std::move(only);
      }
    },
               *params.tool_choice);
  }

  // allowed_tools: case-insensitive intersection on function name (matches the C# reference,
  // which uses StringComparer.OrdinalIgnoreCase). Applied after tool_choice so a ForcedFunction
  // that names a tool excluded by allowed_tools collapses to an empty set — same strict semantics.
  if (params.allowed_tools.has_value()) {
    std::unordered_set<std::string> allowed;
    allowed.reserve(params.allowed_tools->size());
    for (const auto& name : *params.allowed_tools) {
      allowed.insert(ToLower(name));
    }

    std::vector<responses::ToolDefinition> intersected;
    for (const auto& tool : filtered) {
      if (allowed.count(ToLower(tool.function.name)) > 0) {
        intersected.push_back(tool);
      }
    }
    filtered = std::move(intersected);
  }

  if (filtered.empty()) {
    return {};
  }

  return SerializeTools(filtered).dump();
}

// ---------------------------------------------------------------------------
// FromSessionResponse — returns typed output items
// ---------------------------------------------------------------------------

std::pair<std::vector<ResponseOutputItem>, std::string> FromSessionResponse(const fl::Response& session_response,
                                                                            const std::string& msg_id_prefix) {
  std::vector<ResponseOutputItem> output;
  std::string output_text;

  for (const auto& item : session_response.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
      ToolCallItem& call_item = static_cast<ToolCallItem&>(*item);
      FunctionCallOutputItem fc;
      fc.id = GenerateId("fc");
      fc.type = "function_call";
      fc.call_id = call_item.call_id.empty() ? GenerateId("call")
                                             : call_item.call_id;
      fc.name = call_item.name;
      fc.arguments = call_item.arguments;
      fc.status = ResponseStatus::kCompleted;
      output.push_back(std::move(fc));
    } else if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      MessageItem& msg_item = static_cast<MessageItem&>(*item);
      if (msg_item.role == FOUNDRY_LOCAL_ROLE_ASSISTANT) {
        if (msg_item.IsSimpleText()) {
          // Single-text fast path: no reasoning possible, emit one message item.
          std::string text = msg_item.GetSimpleText();

          if (text.empty()) {
            continue;
          }

          output_text += text;

          ResponseOutputMessage msg;
          msg.id = GenerateId(msg_id_prefix);
          msg.role = "assistant";
          msg.status = ResponseStatus::kCompleted;
          msg.content.push_back(OutputTextContent{std::move(text)});
          output.push_back(std::move(msg));
          continue;
        }

        // Multi-part message (reasoning model, possibly interleaved). Walk the parts in stream order and start
        // a fresh output item on every type transition. This preserves the produced sequence — e.g. the model
        // can emit `reasoning -> answer -> reasoning -> answer` and each run becomes its own output item, which
        // matches how the OpenAI Responses API surfaces interleaved reasoning (one `reasoning` item per
        // contiguous reasoning run, one `message` item per contiguous visible run).
        std::optional<flTextItemType> current_type;
        std::string current_text;

        auto flush_current = [&]() {
          if (current_text.empty() || !current_type.has_value()) {
            current_text.clear();
            current_type.reset();
            return;
          }

          if (*current_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
            ReasoningOutputItem rs;
            rs.id = GenerateId("rs");
            rs.status = ResponseStatus::kCompleted;
            rs.summary.push_back(ReasoningSummaryText{std::move(current_text)});
            output.push_back(std::move(rs));
          } else {
            output_text += current_text;

            ResponseOutputMessage msg;
            msg.id = GenerateId(msg_id_prefix);
            msg.role = "assistant";
            msg.status = ResponseStatus::kCompleted;
            msg.content.push_back(OutputTextContent{std::move(current_text)});
            output.push_back(std::move(msg));
          }

          current_text.clear();
          current_type.reset();
        };

        for (const auto& part : msg_item.content) {
          if (!part.view || part.view->type != FOUNDRY_LOCAL_ITEM_TEXT) {
            continue;
          }

          const auto& ti = static_cast<const TextItem&>(*part.view);

          if (ti.text_type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT &&
              ti.text_type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
            continue;
          }

          if (current_type.has_value() && *current_type != ti.text_type) {
            flush_current();
          }

          current_type = ti.text_type;
          current_text += ti.text;
        }

        flush_current();
      }
    }
  }

  return {std::move(output), output_text};
}

// ---------------------------------------------------------------------------
// EchoRequestParams — copies typed fields from params to ResponseObject
// ---------------------------------------------------------------------------

static void EchoRequestParams(ResponseObject& r,
                              const ResponseCreateParams& params) {
  r.instructions = params.instructions;
  r.previous_response_id = params.previous_response_id;
  r.temperature = params.temperature;
  r.top_p = params.top_p;
  r.presence_penalty = params.presence_penalty;
  r.frequency_penalty = params.frequency_penalty;
  r.max_output_tokens = params.max_output_tokens;
  r.parallel_tool_calls = params.parallel_tool_calls.value_or(true);
  r.store = params.store;
  r.metadata = params.metadata;
  r.user = params.user;
  r.text = params.text;
  r.truncation = "disabled";  // always disabled for local inference
  r.reasoning = params.reasoning;

  if (params.tools.has_value()) {
    r.tools = *params.tools;
  }

  r.tool_choice = params.tool_choice;
}

// ---------------------------------------------------------------------------
// BuildResponseObject — returns typed ResponseObject
// ---------------------------------------------------------------------------

ResponseObject BuildResponseObject(const std::string& response_id,
                                   int64_t created_at,
                                   const std::string& model_name,
                                   const ResponseCreateParams& params,
                                   std::vector<ResponseOutputItem> output,
                                   const std::string& output_text,
                                   const TokenUsage& usage) {
  ResponseObject r;
  r.id = response_id;
  r.created_at = created_at;
  r.completed_at = created_at;  // local inference completes immediately
  r.model = model_name;
  r.status = ResponseStatus::kCompleted;
  r.output = std::move(output);
  r.output_text = output_text;
  r.usage.input_tokens = static_cast<int>(usage.prompt_tokens);
  r.usage.output_tokens = static_cast<int>(usage.completion_tokens);
  r.usage.total_tokens = static_cast<int>(usage.total_tokens);

  EchoRequestParams(r, params);

  return r;
}

// ---------------------------------------------------------------------------
// BuildFailedResponseObject — returns typed ResponseObject with error
// ---------------------------------------------------------------------------

ResponseObject BuildFailedResponseObject(const std::string& response_id,
                                         int64_t created_at,
                                         const std::string& model_name,
                                         const ResponseCreateParams& params,
                                         const std::string& error_code,
                                         const std::string& error_message) {
  auto now = std::chrono::system_clock::now();
  int64_t failed_at = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  ResponseObject r;
  r.id = response_id;
  r.created_at = created_at;
  r.failed_at = failed_at;
  r.model = model_name;
  r.status = ResponseStatus::kFailed;
  r.error = ResponseError{error_code, error_message};

  EchoRequestParams(r, params);

  return r;
}

// ---------------------------------------------------------------------------
// BuildInitialResponseObject — returns typed ResponseObject (in_progress)
// ---------------------------------------------------------------------------

ResponseObject BuildInitialResponseObject(const std::string& response_id,
                                          int64_t created_at,
                                          const std::string& model_name,
                                          const ResponseCreateParams& params) {
  ResponseObject r;
  r.id = response_id;
  r.created_at = created_at;
  r.model = model_name;
  r.status = ResponseStatus::kInProgress;

  EchoRequestParams(r, params);

  return r;
}

// ---------------------------------------------------------------------------
// ToInputItems — unchanged, takes JSON and returns JSON for the store
// ---------------------------------------------------------------------------

nlohmann::json ToInputItems(const nlohmann::json& req_json) {
  nlohmann::json items = nlohmann::json::array();

  // Instructions → system message item
  if (req_json.contains("instructions") && req_json["instructions"].is_string()) {
    items.push_back({
        {"type", "message"},
        {"id", GenerateId("msg")},
        {"role", "system"},
        {"status", "completed"},
        {"content", req_json["instructions"].get<std::string>()},
    });
  }

  if (!req_json.contains("input")) {
    return items;
  }

  const auto& input = req_json["input"];

  if (input.is_string()) {
    items.push_back({
        {"type", "message"},
        {"id", GenerateId("msg")},
        {"role", "user"},
        {"status", "completed"},
        {"content", input.get<std::string>()},
    });
  } else if (input.is_array()) {
    for (const auto& item : input) {
      if (item.is_object()) {
        nlohmann::json stored = item;

        if (!stored.contains("id") || !stored["id"].is_string() ||
            stored["id"].get<std::string>().empty()) {
          std::string type = stored.value("type", "item");
          std::string prefix = "item";
          if (type == "message") {
            prefix = "msg";
          } else if (type == "function_call") {
            prefix = "fc";
          } else if (type == "function_call_output") {
            prefix = "fco";
          }
          stored["id"] = GenerateId(prefix);
        }

        items.push_back(std::move(stored));
      }
    }
  }

  return items;
}

}  // namespace ResponseConverter
}  // namespace fl
