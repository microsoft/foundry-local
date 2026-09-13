// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/qwen_xml_tool_call_decoder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fl {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kStartMarker = "<tool_call>";
constexpr std::string_view kEndMarker = "</tool_call>";
constexpr std::string_view kFunctionPrefix = "<function=";
constexpr std::string_view kParameterPrefix = "<parameter=";
constexpr std::string_view kFunctionEnd = "</function>\n</tool_call>";
constexpr std::string_view kParameterEnd = "\n</parameter>\n";

enum class ParseState {
  kComplete,
  kIncomplete,
  kInvalid,
};

struct FunctionSchema {
  Json properties = Json::object();
  std::unordered_set<std::string> required;
  bool valid = false;
};

using FunctionSchemas = std::unordered_map<std::string, FunctionSchema>;

struct BlockParseResult {
  ParseState state = ParseState::kIncomplete;
  size_t end = 0;
  ParsedToolCall call;
};

BlockParseResult BlockResult(ParseState state) {
  return {
      .state = state,
      .end = 0,
      .call = {},
  };
}

ParseState ConsumeLiteral(std::string_view source, size_t& position, std::string_view expected) {
  const auto remaining = source.substr(position);
  if (remaining.starts_with(expected)) {
    position += expected.size();
    return ParseState::kComplete;
  }

  if (expected.starts_with(remaining)) {
    return ParseState::kIncomplete;
  }

  return ParseState::kInvalid;
}

bool HasUnsupportedComposition(const Json& schema) {
  return schema.contains("anyOf") || schema.contains("oneOf") || schema.contains("allOf") ||
         schema.contains("not") || schema.contains("if");
}

std::optional<std::string> GetSupportedType(const Json& schema) {
  if (!schema.is_object() || HasUnsupportedComposition(schema) || !schema.contains("type") ||
      !schema["type"].is_string()) {
    return std::nullopt;
  }

  const auto type = schema["type"].get<std::string>();
  static const std::unordered_set<std::string> kSupportedTypes = {
      "string",
      "number",
      "integer",
      "boolean",
      "array",
      "object",
      "null",
  };
  if (!kSupportedTypes.contains(type)) {
    return std::nullopt;
  }

  return type;
}

FunctionSchemas ParseFunctionSchemas(
    const std::string& tools_json,
    const std::unordered_map<std::string, ToolKind>& tool_kinds) {
  FunctionSchemas schemas;
  const auto tools = Json::parse(tools_json, nullptr, false);
  if (!tools.is_array()) {
    return schemas;
  }

  for (const auto& tool : tools) {
    if (!tool.is_object() || !tool.contains("type") || !tool["type"].is_string() ||
        tool["type"].get<std::string>() != "function") {
      continue;
    }

    const Json* function = &tool;
    if (tool.contains("function")) {
      if (!tool["function"].is_object()) {
        continue;
      }

      function = &tool["function"];
    }

    if (!function->contains("name") || !(*function)["name"].is_string()) {
      continue;
    }

    const auto name = (*function)["name"].get<std::string>();
    const auto kind = tool_kinds.find(name);
    if (kind == tool_kinds.end() || kind->second != ToolKind::kFunction) {
      continue;
    }

    FunctionSchema schema;
    if (name.empty()) {
      schemas.emplace(name, std::move(schema));
      continue;
    }

    if (!function->contains("parameters") || (*function)["parameters"].is_null()) {
      schema.valid = true;
      schemas.emplace(name, std::move(schema));
      continue;
    }

    const auto& parameters = (*function)["parameters"];
    if (!parameters.is_object() || HasUnsupportedComposition(parameters)) {
      schemas.emplace(name, std::move(schema));
      continue;
    }

    const auto parameter_type = GetSupportedType(parameters);
    const bool has_no_properties =
        !parameters.contains("properties") ||
        (parameters["properties"].is_object() && parameters["properties"].empty());
    const bool has_no_required_parameters =
        !parameters.contains("required") ||
        (parameters["required"].is_array() && parameters["required"].empty());
    if (parameters.empty() ||
        (parameter_type.has_value() && *parameter_type == "object" && has_no_properties &&
         has_no_required_parameters)) {
      schema.valid =
          !parameters.contains("properties") || parameters["properties"].is_object();
      schemas.emplace(name, std::move(schema));
      continue;
    }

    if (!parameter_type.has_value() || *parameter_type != "object" ||
        !parameters.contains("properties") || !parameters["properties"].is_object()) {
      schemas.emplace(name, std::move(schema));
      continue;
    }

    schema.properties = parameters["properties"];
    const bool properties_valid =
        std::ranges::all_of(schema.properties.items(), [](const auto& property) {
          return GetSupportedType(property.value()).has_value();
        });
    bool required_valid = true;
    if (parameters.contains("required")) {
      if (!parameters["required"].is_array()) {
        required_valid = false;
      } else {
        for (const auto& required : parameters["required"]) {
          if (!required.is_string() || !schema.required.insert(required.get<std::string>()).second) {
            required_valid = false;
            break;
          }
        }
      }
    }

    schema.valid = properties_valid && required_valid &&
                   std::ranges::all_of(schema.required, [&](const auto& required) {
                     return schema.properties.contains(required);
                   });
    const auto insertion = schemas.emplace(name, std::move(schema));
    if (!insertion.second) {
      schemas[name].valid = false;
    }
  }

  return schemas;
}

bool IsCompatibleJsonValue(const Json& value, std::string_view type) {
  if (type == "number") {
    return value.is_number();
  }
  if (type == "integer") {
    return value.is_number_integer() || value.is_number_unsigned();
  }
  if (type == "boolean") {
    return value.is_boolean();
  }
  if (type == "array") {
    return value.is_array();
  }
  if (type == "object") {
    return value.is_object();
  }
  if (type == "null") {
    return value.is_null();
  }

  return false;
}

std::optional<Json> DecodeParameterValue(std::string_view body, const Json& schema) {
  const auto type = GetSupportedType(schema);
  if (!type.has_value()) {
    return std::nullopt;
  }

  if (*type == "string") {
    return Json(std::string(body));
  }

  const auto value = Json::parse(body, nullptr, false);
  if (value.is_discarded() || !IsCompatibleJsonValue(value, *type)) {
    return std::nullopt;
  }

  return value;
}

bool ContainsAmbiguousMarkup(std::string_view body) {
  return body.find('<') != std::string_view::npos;
}

std::optional<std::string_view> ReadTagName(std::string_view source, size_t& position,
                                            std::string_view prefix) {
  const auto prefix_state = ConsumeLiteral(source, position, prefix);
  if (prefix_state != ParseState::kComplete) {
    return std::nullopt;
  }

  const auto end = source.find(">\n", position);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }

  const auto name = source.substr(position, end - position);
  if (name.empty() || name.find_first_of("<>=\r\n\t ") != std::string_view::npos) {
    return std::nullopt;
  }

  position = end + 2;
  return name;
}

BlockParseResult ParseBlock(std::string_view source, size_t start, const FunctionSchemas& schemas) {
  size_t position = start;
  auto state = ConsumeLiteral(source, position, "<tool_call>\n");
  if (state != ParseState::kComplete) {
    return BlockResult(state);
  }

  const auto function_name = ReadTagName(source, position, kFunctionPrefix);
  if (!function_name.has_value()) {
    return BlockResult(source.find(kEndMarker, position) == std::string_view::npos
                           ? ParseState::kIncomplete
                           : ParseState::kInvalid);
  }

  const auto schema_it = schemas.find(std::string(*function_name));
  if (schema_it == schemas.end() || !schema_it->second.valid) {
    return BlockResult(ParseState::kInvalid);
  }

  Json arguments = Json::object();
  std::unordered_set<std::string> seen_parameters;
  while (true) {
    if (source.substr(position).starts_with(kFunctionEnd)) {
      position += kFunctionEnd.size();
      break;
    }

    if (kFunctionEnd.starts_with(source.substr(position))) {
      return BlockResult(ParseState::kIncomplete);
    }

    const auto parameter_name = ReadTagName(source, position, kParameterPrefix);
    if (!parameter_name.has_value()) {
      return BlockResult(source.find(kEndMarker, position) == std::string_view::npos
                             ? ParseState::kIncomplete
                             : ParseState::kInvalid);
    }

    const auto parameter = std::string(*parameter_name);
    if (!seen_parameters.insert(parameter).second || !schema_it->second.properties.contains(parameter)) {
      return BlockResult(ParseState::kInvalid);
    }

    const auto body_end = source.find(kParameterEnd, position);
    if (body_end == std::string_view::npos) {
      return BlockResult(source.find(kEndMarker, position) == std::string_view::npos
                             ? ParseState::kIncomplete
                             : ParseState::kInvalid);
    }

    const auto body = source.substr(position, body_end - position);
    if (ContainsAmbiguousMarkup(body)) {
      return BlockResult(ParseState::kInvalid);
    }

    auto value = DecodeParameterValue(body, schema_it->second.properties[parameter]);
    if (!value.has_value()) {
      return BlockResult(ParseState::kInvalid);
    }

    arguments[parameter] = std::move(*value);
    position = body_end + kParameterEnd.size();
  }

  if (!std::ranges::all_of(schema_it->second.required, [&](const auto& required) {
        return seen_parameters.contains(required);
      })) {
    return BlockResult(ParseState::kInvalid);
  }

  const auto arguments_json = arguments.dump();
  return {
      .state = ParseState::kComplete,
      .end = position,
      .call = ParsedToolCall{"", std::string(*function_name), arguments_json},
  };
}

size_t RejectedBatchEnd(std::string_view source, size_t invalid_position, bool end_of_stream) {
  size_t position = invalid_position;
  while (true) {
    const auto end = source.find(kEndMarker, position);
    if (end == std::string_view::npos) {
      return end_of_stream ? source.size() : 0;
    }

    const auto block_end = end + kEndMarker.size();
    position = source.find_first_not_of(" \t\r\n", block_end);
    if (position == std::string_view::npos) {
      return end_of_stream ? block_end : 0;
    }

    const auto remaining = source.substr(position);
    if (remaining.starts_with(kStartMarker)) {
      position += kStartMarker.size();
      continue;
    }
    if (kStartMarker.starts_with(remaining) && !end_of_stream) {
      return 0;
    }

    return block_end;
  }
}

ToolCallPayloadParseResult ParseBatch(std::string_view source, bool end_of_stream,
                                      const FunctionSchemas& schemas) {
  std::vector<ParsedToolCall> calls;
  size_t position = 0;
  size_t batch_end = 0;

  while (true) {
    auto block = ParseBlock(source, position, schemas);
    if (block.state == ParseState::kIncomplete) {
      if (!end_of_stream) {
        return {};
      }

      return {
          .disposition = ToolCallPayloadDisposition::kRejected,
          .consumed_size = source.size(),
          .calls = {},
      };
    }
    if (block.state == ParseState::kInvalid) {
      const auto rejected_end = RejectedBatchEnd(source, position, end_of_stream);
      if (rejected_end == 0) {
        return {};
      }

      return {
          .disposition = ToolCallPayloadDisposition::kRejected,
          .consumed_size = rejected_end,
          .calls = {},
      };
    }

    calls.push_back(std::move(block.call));
    batch_end = block.end;
    position = source.find_first_not_of(" \t\r\n", batch_end);
    if (position == std::string_view::npos) {
      if (!end_of_stream) {
        return {};
      }

      break;
    }

    const auto remaining = source.substr(position);
    if (remaining.starts_with(kStartMarker)) {
      continue;
    }
    if (kStartMarker.starts_with(remaining) && !end_of_stream) {
      return {};
    }

    break;
  }

  for (auto& call : calls) {
    call.id = GenerateToolCallId();
    call.argument_source = call.arguments;
  }

  return {
      .disposition = ToolCallPayloadDisposition::kParsed,
      .consumed_size = batch_end,
      .calls = std::move(calls),
  };
}

}  // namespace

ToolCallPayloadParser CreateQwenXmlToolCallPayloadParser(
    std::string tools_json, std::unordered_map<std::string, ToolKind> tool_kinds) {
  auto schemas = ParseFunctionSchemas(tools_json, tool_kinds);
  return [schemas = std::move(schemas)](std::string_view source, bool end_of_stream) {
    return ParseBatch(source, end_of_stream, schemas);
  };
}

}  // namespace fl
