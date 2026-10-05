// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/qwen_xml_tool_call_decoder.h"
#include "inferencing/session/tool_registry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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

constexpr std::string_view kCallStart = "<tool_call>\n";
constexpr std::string_view kFunctionPrefix = "<function=";
constexpr std::string_view kParameterPrefix = "<parameter=";
constexpr std::string_view kFunctionEnd = "</function>\n</tool_call>";
constexpr std::string_view kParameterEnd = "\n</parameter>\n";
constexpr size_t kMaxSchemaNesting = 16;

enum class ParseState {
  kComplete,
  kIncomplete,
  kQualifiedIncomplete,
  kStructuralFailure,
  kInvalid,
  kSchemaViolation,
};

struct FunctionSchema {
  Json properties = Json::object();
  std::unordered_set<std::string> required;
  bool has_parameters = false;
  bool recognizable = false;
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

bool IsSupportedAnnotation(std::string_view keyword) {
  static const std::unordered_set<std::string_view> kSupportedAnnotations = {
      "$comment",
      "default",
      "deprecated",
      "description",
      "examples",
      "readOnly",
      "title",
      "writeOnly",
      "x-mcp-header",
  };
  return kSupportedAnnotations.contains(keyword);
}

bool JsonScalarEquals(const Json& lhs, const Json& rhs) {
  // nlohmann stores floating numbers as double and does not retain their source lexemes. Requiring the same JSON
  // representation prevents distinct decimal literals that round to one double from authorizing a tool call.
  return (!lhs.is_number() || lhs.type() == rhs.type()) && lhs == rhs;
}

bool IsCompatibleScalarType(const Json& value, std::string_view type) {
  if (type == "string") {
    return value.is_string();
  }
  if (type == "number") {
    return value.is_number();
  }
  if (type == "integer") {
    return value.is_number_integer() || value.is_number_unsigned();
  }
  if (type == "boolean") {
    return value.is_boolean();
  }
  return type == "null" && value.is_null();
}

bool IsSupportedEnumValue(const Json& value, std::string_view type) {
  if (type == "number" || type == "integer") {
    // Floating JSON numbers lose their source lexemes when parsed. Support exact integer representations only so
    // distinct decimal literals that round to the same double can never authorize a tool call.
    return value.is_number_integer() || value.is_number_unsigned();
  }
  return IsCompatibleScalarType(value, type);
}

std::string EnumValueKey(const Json& value) {
  std::string key = std::to_string(static_cast<int>(value.type()));
  key.push_back(':');
  key += value.dump();
  return key;
}

bool IsSupportedEnum(const Json& schema, std::string_view type) {
  if (!schema.contains("enum")) {
    return true;
  }

  const auto& values = schema["enum"];
  if (!values.is_array() || values.empty() ||
      (type != "string" && type != "number" && type != "integer" && type != "boolean" && type != "null")) {
    return false;
  }

  std::unordered_set<std::string> seen_values;
  seen_values.reserve(values.size());
  for (const auto& value : values) {
    if (!IsSupportedEnumValue(value, type) || !seen_values.insert(EnumValueKey(value)).second) {
      return false;
    }
  }
  return true;
}

bool IsSupportedParameterSchema(const Json& schema, size_t depth = 0) {
  if (!schema.is_object() || depth >= kMaxSchemaNesting) {
    return false;
  }

  if (schema.contains("anyOf")) {
    if (schema.contains("oneOf") || schema.contains("allOf") || schema.contains("not") ||
        schema.contains("if") || !schema["anyOf"].is_array() || schema["anyOf"].empty() ||
        std::ranges::any_of(schema.items(), [](const auto& item) {
          return item.key() != "anyOf" && !IsSupportedAnnotation(item.key());
        })) {
      return false;
    }

    return std::ranges::all_of(schema["anyOf"], [depth](const auto& branch) {
      return branch.is_object() && !branch.contains("anyOf") &&
             IsSupportedParameterSchema(branch, depth + 1);
    });
  }

  const auto type = GetSupportedType(schema);
  if (!type.has_value()) {
    return false;
  }

  if (std::ranges::any_of(schema.items(), [&](const auto& item) {
        return item.key() != "type" && !IsSupportedAnnotation(item.key()) &&
               item.key() != "enum" && !(*type == "array" && item.key() == "items");
      })) {
    return false;
  }

  return IsSupportedEnum(schema, *type) &&
         (*type != "array" || !schema.contains("items") ||
          IsSupportedParameterSchema(schema["items"], depth + 1));
}

bool HasNestedAnyOf(const Json& schema, size_t depth = 0) {
  if (!schema.is_object()) {
    return false;
  }
  if (depth != 0 && schema.contains("anyOf")) {
    return true;
  }
  if (schema.contains("anyOf") &&
      std::ranges::any_of(schema["anyOf"], [depth](const auto& branch) {
        return HasNestedAnyOf(branch, depth + 1);
      })) {
    return true;
  }

  return schema.contains("items") && HasNestedAnyOf(schema["items"], depth + 1);
}

bool IsSupportedParametersObject(const Json& schema) {
  if (!schema.is_object() || HasUnsupportedComposition(schema)) {
    return false;
  }

  return std::ranges::all_of(schema.items(), [](const auto& item) {
    if (IsSupportedAnnotation(item.key())) {
      return true;
    }
    if (item.key() == "type" || item.key() == "properties" || item.key() == "required") {
      return true;
    }
    return item.key() == "additionalProperties" && item.value().is_boolean() &&
           !item.value().template get<bool>();
  });
}

FunctionSchemas ParseFunctionSchemas(
    const std::string& tools_json,
    const std::unordered_map<std::string, ToolKind>& tool_kinds,
    size_t& declaration_count,
    bool recovery_aware) {
  FunctionSchemas schemas;
  const auto tools = Json::parse(tools_json, nullptr, false);
  if (!tools.is_array()) {
    return schemas;
  }
  declaration_count = tools.size();

  const auto custom_tool_schema = Json::parse(kCustomToolInputSchema);
  const auto insert_schema = [&schemas](const std::string& name, FunctionSchema schema) {
    const auto [existing, inserted] = schemas.emplace(name, std::move(schema));
    if (!inserted) {
      existing->second.valid = false;
      existing->second.recognizable = false;
    }
  };

  for (const auto& tool : tools) {
    if (!tool.is_object()) {
      continue;
    }

    const Json* function = &tool;
    if (tool.contains("function")) {
      if (!tool.contains("type") || !tool["type"].is_string() ||
          tool["type"].get_ref<const std::string&>() != "function" ||
          !tool["function"].is_object()) {
        continue;
      }

      function = &tool["function"];
    } else if (tool.contains("type") &&
               (!tool["type"].is_string() ||
                tool["type"].get_ref<const std::string&>() != "function")) {
      continue;
    }

    if (!function->contains("name") || !(*function)["name"].is_string()) {
      continue;
    }

    const auto name = (*function)["name"].get<std::string>();
    const auto kind = tool_kinds.find(name);
    if (kind == tool_kinds.end()) {
      continue;
    }
    FunctionSchema schema;
    if (name.empty()) {
      insert_schema(name, std::move(schema));
      continue;
    }

    if (!function->contains("parameters") || (*function)["parameters"].is_null()) {
      schema.recognizable = true;
      schema.valid = true;
      insert_schema(name, std::move(schema));
      continue;
    }

    const auto& parameters = (*function)["parameters"];
    if (kind->second == ToolKind::kCustom && parameters != custom_tool_schema) {
      insert_schema(name, std::move(schema));
      continue;
    }

    schema.recognizable =
        parameters.is_boolean() ||
        (parameters.is_object() &&
         (!parameters.contains("type") || parameters["type"].is_string() ||
          (parameters["type"].is_array() && !parameters["type"].empty() &&
           std::ranges::all_of(parameters["type"], [](const auto& type) { return type.is_string(); }))) &&
         (!parameters.contains("properties") || parameters["properties"].is_object()) &&
         (!parameters.contains("required") || parameters["required"].is_array()));
    schema.has_parameters = !parameters.empty();
    if (!IsSupportedParametersObject(parameters)) {
      insert_schema(name, std::move(schema));
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
      insert_schema(name, std::move(schema));
      continue;
    }

    if (!parameter_type.has_value() || *parameter_type != "object" ||
        !parameters.contains("properties") || !parameters["properties"].is_object()) {
      insert_schema(name, std::move(schema));
      continue;
    }

    schema.properties = parameters["properties"];
    const bool properties_valid =
        std::ranges::all_of(schema.properties.items(), [recovery_aware](const auto& property) {
          return IsSupportedParameterSchema(property.value()) &&
                 (!recovery_aware || !HasNestedAnyOf(property.value()));
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
    insert_schema(name, std::move(schema));
  }

  return schemas;
}

bool IsCompatibleJsonValue(const Json& value, const Json& schema, size_t depth = 0) {
  if (depth >= kMaxSchemaNesting) {
    return false;
  }

  const auto type = GetSupportedType(schema);
  if (!type.has_value()) {
    return false;
  }

  bool compatible = false;
  if (*type == "string" || *type == "number" || *type == "integer" || *type == "boolean" ||
      *type == "null") {
    compatible = IsCompatibleScalarType(value, *type);
  }
  if (*type == "array") {
    compatible = value.is_array() &&
                 (!schema.contains("items") ||
                  std::ranges::all_of(value, [&](const auto& item) {
                    return IsCompatibleJsonValue(item, schema["items"], depth + 1);
                  }));
  }
  if (*type == "object") {
    compatible = value.is_object();
  }
  return compatible &&
         (!schema.contains("enum") ||
          std::ranges::any_of(schema["enum"], [&](const auto& expected) {
            return JsonScalarEquals(value, expected);
          }));
}

bool IsCompatibleParameterValue(const Json& value, const Json& schema) {
  if (!IsSupportedParameterSchema(schema)) {
    return false;
  }

  if (!schema.contains("anyOf")) {
    return IsCompatibleJsonValue(value, schema);
  }

  return std::ranges::any_of(schema["anyOf"], [&](const auto& branch) {
    return IsCompatibleJsonValue(value, branch);
  });
}

std::optional<Json> ParseJsonWithoutDuplicateObjectKeys(std::string_view source) {
  std::vector<std::unordered_set<std::string>> object_keys;
  bool duplicate_key = false;
  const auto detect_duplicate_keys = [&](int, Json::parse_event_t event, Json& parsed) {
    if (event == Json::parse_event_t::object_start) {
      object_keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (object_keys.empty() ||
          !object_keys.back().insert(parsed.get_ref<const std::string&>()).second) {
        duplicate_key = true;
      }
    } else if (event == Json::parse_event_t::object_end) {
      object_keys.pop_back();
    }

    return true;
  };

  auto parsed = Json::parse(source, detect_duplicate_keys, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || duplicate_key) {
    return std::nullopt;
  }

  return parsed;
}

std::optional<Json> DecodeSimpleParameterValue(std::string_view body, const Json& schema) {
  const auto type = GetSupportedType(schema);
  if (!type.has_value()) {
    return std::nullopt;
  }

  if (*type == "string") {
    Json value = std::string(body);
    return IsCompatibleJsonValue(value, schema) ? std::optional<Json>(std::move(value)) : std::nullopt;
  }

  if (*type == "boolean" && (body == "True" || body == "False")) {
    Json value = body == "True";
    return IsCompatibleJsonValue(value, schema) ? std::optional<Json>(std::move(value)) : std::nullopt;
  }

  const auto value = Json::parse(body, nullptr, false);
  if (value.is_discarded() || !IsCompatibleJsonValue(value, schema)) {
    return std::nullopt;
  }

  return value;
}

bool HasStructuredJsonPrefix(std::string_view body) {
  const auto first = body.find_first_not_of(" \t\r\n");
  return first != std::string_view::npos && (body[first] == '[' || body[first] == '{');
}

std::optional<Json> DecodeParameterValue(std::string_view body, const Json& schema) {
  if (!schema.is_object() || !schema.contains("anyOf")) {
    return DecodeSimpleParameterValue(body, schema);
  }

  if (!IsSupportedParameterSchema(schema)) {
    return std::nullopt;
  }

  std::optional<Json> decoded_string;
  std::optional<Json> decoded;
  for (const auto& branch : schema["anyOf"]) {
    const auto type = GetSupportedType(branch);
    if (type.has_value() && *type == "string") {
      auto candidate = DecodeSimpleParameterValue(body, branch);
      if (candidate.has_value()) {
        decoded_string = std::move(candidate);
      }
      continue;
    }

    auto candidate = DecodeSimpleParameterValue(body, branch);
    if (!candidate.has_value()) {
      continue;
    }

    if (decoded.has_value() && *decoded != *candidate) {
      return std::nullopt;
    }
    decoded = std::move(candidate);
  }

  if (decoded.has_value()) {
    return decoded;
  }
  // A leading array/object delimiter claims the structured branch. Reinterpreting malformed structured output as
  // a string would turn a model type error into an executable call; ambiguous union values therefore fail closed.
  if (decoded_string.has_value() && !HasStructuredJsonPrefix(body)) {
    return decoded_string;
  }

  return std::nullopt;
}

bool ContainsReservedFramingMarkup(std::string_view body) {
  constexpr std::array<std::string_view, 6> kReservedMarkup = {
      kQwenXmlToolCallStartMarker,
      kQwenXmlToolCallEndMarker,
      kFunctionPrefix,
      "</function>",
      kParameterPrefix,
      "</parameter>",
  };
  return std::ranges::any_of(kReservedMarkup, [&](const auto marker) {
    return body.find(marker) != std::string_view::npos;
  });
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
  auto state = ConsumeLiteral(source, position, kCallStart);
  if (state != ParseState::kComplete) {
    return BlockResult(state);
  }

  const auto function_name = ReadTagName(source, position, kFunctionPrefix);
  if (!function_name.has_value()) {
    return BlockResult(source.find(kQwenXmlToolCallEndMarker, position) == std::string_view::npos
                           ? ParseState::kIncomplete
                           : ParseState::kInvalid);
  }

  const auto schema_it = schemas.find(std::string(*function_name));
  if (schema_it == schemas.end()) {
    return BlockResult(ParseState::kInvalid);
  }
  if (!schema_it->second.valid) {
    return BlockResult(ParseState::kSchemaViolation);
  }

  Json arguments = Json::object();
  std::unordered_set<std::string> seen_parameters;
  while (true) {
    if (source.substr(position).starts_with(kFunctionEnd)) {
      position += kFunctionEnd.size();
      break;
    }

    if (kFunctionEnd.starts_with(source.substr(position))) {
      return BlockResult(ParseState::kQualifiedIncomplete);
    }

    const auto parameter_name = ReadTagName(source, position, kParameterPrefix);
    if (!parameter_name.has_value()) {
      return BlockResult(source.find(kQwenXmlToolCallEndMarker, position) == std::string_view::npos
                             ? ParseState::kQualifiedIncomplete
                             : ParseState::kStructuralFailure);
    }

    const auto parameter = std::string(*parameter_name);
    if (!seen_parameters.insert(parameter).second || !schema_it->second.properties.contains(parameter)) {
      return BlockResult(ParseState::kSchemaViolation);
    }

    const auto body_end = source.find(kParameterEnd, position);
    if (body_end == std::string_view::npos) {
      return BlockResult(source.find(kQwenXmlToolCallEndMarker, position) == std::string_view::npos
                             ? ParseState::kQualifiedIncomplete
                             : ParseState::kSchemaViolation);
    }

    const auto body = source.substr(position, body_end - position);
    if (ContainsReservedFramingMarkup(body)) {
      return BlockResult(ParseState::kSchemaViolation);
    }

    auto value = DecodeParameterValue(body, schema_it->second.properties[parameter]);
    if (!value.has_value()) {
      return BlockResult(ParseState::kSchemaViolation);
    }

    arguments[parameter] = std::move(*value);
    position = body_end + kParameterEnd.size();
  }

  if (!std::ranges::all_of(schema_it->second.required, [&](const auto& required) {
        return seen_parameters.contains(required);
      })) {
    return BlockResult(ParseState::kSchemaViolation);
  }

  const auto arguments_json = arguments.dump();
  return {
      .state = ParseState::kComplete,
      .end = position,
      .call = ParsedToolCall{"", std::string(*function_name), arguments_json},
  };
}

struct RejectedBatch {
  size_t end = 0;
  bool schema_violation = false;
};

RejectedBatch RejectedBlockEnd(std::string_view source, size_t start, bool end_of_stream) {
  size_t position = start + kCallStart.size();
  if (auto function_name = ReadTagName(source, position, kFunctionPrefix)) {
    while (position < source.size()) {
      if (source.substr(position).starts_with(kFunctionEnd)) {
        return {position + kFunctionEnd.size(), false};
      }

      if (kFunctionEnd.starts_with(source.substr(position)) && !end_of_stream) {
        return {};
      }

      if (!ReadTagName(source, position, kParameterPrefix)) {
        break;
      }

      const auto body_end = source.find(kParameterEnd, position);
      const auto outer_end = source.find(kFunctionEnd, position);
      for (auto call_end = source.find(kQwenXmlToolCallEndMarker, position);
           call_end != std::string_view::npos &&
           (body_end == std::string_view::npos || call_end < body_end);
           call_end = source.find(kQwenXmlToolCallEndMarker,
                                  call_end + kQwenXmlToolCallEndMarker.size())) {
        const auto next = source.find_first_not_of(" \t\r\n", call_end + kQwenXmlToolCallEndMarker.size());
        if (next != std::string_view::npos && source.substr(next).starts_with(kCallStart)) {
          size_t name_position = next + kCallStart.size();
          // A sibling parameter or function close before this body's close is ambiguous framing. Fail closed;
          // a bare function header without either can still be literal body text.
          if (ReadTagName(source, name_position, kFunctionPrefix) &&
              (body_end == std::string_view::npos ||
               source.find(kParameterPrefix, name_position) < body_end ||
               source.find(kFunctionEnd, name_position) < body_end)) {
            return {source.size(), true};
          }
        }
      }
      if (outer_end != std::string_view::npos &&
          (body_end == std::string_view::npos || outer_end < body_end)) {
        for (auto sibling = source.find(kCallStart, outer_end + kFunctionEnd.size());
             sibling != std::string_view::npos &&
             (body_end == std::string_view::npos || sibling < body_end);
             sibling = source.find(kCallStart, sibling + kCallStart.size())) {
          size_t name_position = sibling + kCallStart.size();
          if (ReadTagName(source, name_position, kFunctionPrefix)) {
            return {source.size(), true};
          }
        }
      }
      if (body_end == std::string_view::npos) {
        return {end_of_stream ? source.size() : 0, false};
      }

      position = body_end + kParameterEnd.size();
    }
  }

  const auto function_end = source.find(kFunctionEnd, position);
  const auto end = source.find(kQwenXmlToolCallEndMarker, position);
  if (function_end != std::string_view::npos &&
      (end == std::string_view::npos || function_end < end)) {
    return {function_end + kFunctionEnd.size(), false};
  }

  return {end == std::string_view::npos ? (end_of_stream ? source.size() : 0)
                                       : end + kQwenXmlToolCallEndMarker.size(), false};
}

RejectedBatch RejectedBatchEnd(std::string_view source, size_t invalid_position, bool end_of_stream,
                              const FunctionSchemas& schemas) {
  size_t position = invalid_position;
  while (true) {
    const auto block = ParseBlock(source, position, schemas);
    if (block.state == ParseState::kSchemaViolation) {
      return {source.size(), true};
    }
    if (block.state == ParseState::kIncomplete || block.state == ParseState::kQualifiedIncomplete) {
      return {end_of_stream ? source.size() : 0, false};
    }

    const auto rejected_block = block.state == ParseState::kComplete
                                    ? RejectedBatch{block.end, false}
                                    : RejectedBlockEnd(source, position, end_of_stream);
    if (rejected_block.schema_violation) {
      return rejected_block;
    }
    if (rejected_block.end == 0) {
      return {};
    }

    position = source.find_first_not_of(" \t\r\n", rejected_block.end);
    if (position == std::string_view::npos) {
      return {end_of_stream ? rejected_block.end : 0, false};
    }

    const auto remaining = source.substr(position);
    if (remaining.starts_with(kQwenXmlToolCallStartMarker)) {
      continue;
    }
    if (kQwenXmlToolCallStartMarker.starts_with(remaining) && !end_of_stream) {
      return {};
    }

    return rejected_block;
  }
}

ToolCallPayloadParseResult ParseBatch(std::string_view source, bool end_of_stream,
                                      const FunctionSchemas& schemas, bool recovery_aware) {
  std::vector<ParsedToolCall> calls;
  size_t position = 0;
  size_t batch_end = 0;

  while (true) {
    auto block = ParseBlock(source, position, schemas);
    if (block.state == ParseState::kIncomplete || block.state == ParseState::kQualifiedIncomplete) {
      if (!end_of_stream) {
        return {};
      }

      return {
          .disposition = recovery_aware && calls.empty() &&
                                 block.state == ParseState::kQualifiedIncomplete
                             ? ToolCallPayloadDisposition::kMalformed
                             : ToolCallPayloadDisposition::kRejected,
          .consumed_size = source.size(),
          .calls = {},
      };
    }
    if (block.state == ParseState::kInvalid || block.state == ParseState::kStructuralFailure ||
        block.state == ParseState::kSchemaViolation) {
      const auto rejected = RejectedBatchEnd(source, position, end_of_stream, schemas);
      if (rejected.end == 0) {
        return {};
      }

      return {
          .disposition = rejected.schema_violation ||
                                 (recovery_aware && calls.empty() &&
                                  block.state == ParseState::kStructuralFailure)
                             ? ToolCallPayloadDisposition::kMalformed
                             : ToolCallPayloadDisposition::kRejected,
          .consumed_size = rejected.end,
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
    if (remaining.starts_with(kQwenXmlToolCallStartMarker)) {
      continue;
    }
    if (kQwenXmlToolCallStartMarker.starts_with(remaining) && !end_of_stream) {
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
    std::string tools_json, std::unordered_map<std::string, ToolKind> tool_kinds,
    bool recovery_aware) {
  size_t declaration_count = 0;
  auto schemas = ParseFunctionSchemas(tools_json, tool_kinds, declaration_count, recovery_aware);
  if (declaration_count == 0 || schemas.size() != declaration_count ||
      schemas.size() != tool_kinds.size() ||
      std::ranges::none_of(schemas, [](const auto& schema) {
        return schema.second.recognizable;
      })) {
    return {};
  }

  return [schemas = std::move(schemas), recovery_aware](std::string_view source, bool end_of_stream) {
    return ParseBatch(source, end_of_stream, schemas, recovery_aware);
  };
}

std::vector<ParsedToolCall> ParseQwenGuidedToolCalls(
    std::string_view payload,
    const std::string& tools_json,
    const std::unordered_map<std::string, ToolKind>& tool_kinds) {
  const auto calls_json = ParseJsonWithoutDuplicateObjectKeys(payload);
  if (!calls_json.has_value() || !calls_json->is_array() || calls_json->empty()) {
    return {};
  }

  size_t declaration_count = 0;
  const auto schemas = ParseFunctionSchemas(
      tools_json, tool_kinds, declaration_count, /*recovery_aware=*/true);
  if (declaration_count == 0 || schemas.size() != declaration_count ||
      schemas.size() != tool_kinds.size() ||
      std::ranges::none_of(schemas, [](const auto& schema) {
        return schema.second.valid;
      })) {
    return {};
  }

  std::vector<ParsedToolCall> calls;
  calls.reserve(calls_json->size());

  for (const auto& call_json : *calls_json) {
    if (!call_json.is_object() || !call_json.contains("name") ||
        !call_json["name"].is_string()) {
      return {};
    }

    auto name = call_json["name"].get<std::string>();
    const auto schema = schemas.find(name);
    if (schema == schemas.end() || !schema->second.valid) {
      return {};
    }

    const auto expected_field_count = schema->second.has_parameters ? 2u : 1u;
    if (call_json.size() != expected_field_count) {
      return {};
    }

    Json arguments = Json::object();
    if (schema->second.has_parameters) {
      if (!call_json.contains("parameters") || !call_json["parameters"].is_object()) {
        return {};
      }

      arguments = call_json["parameters"];
      if (std::ranges::any_of(arguments.items(), [&](const auto& argument) {
            const auto property = schema->second.properties.find(argument.key());
            return property == schema->second.properties.end() ||
                   !IsCompatibleParameterValue(argument.value(), *property);
          }) ||
          !std::ranges::all_of(schema->second.required, [&](const auto& required) {
            return arguments.contains(required);
          })) {
        return {};
      }
    }

    auto call = ParsedToolCall{GenerateToolCallId(), std::move(name), arguments.dump()};
    call.argument_source = call.arguments;
    calls.push_back(std::move(call));
  }

  return calls;
}

}  // namespace fl
