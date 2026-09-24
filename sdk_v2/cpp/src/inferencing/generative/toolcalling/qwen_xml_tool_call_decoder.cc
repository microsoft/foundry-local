// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/qwen_xml_tool_call_decoder.h"
#include "inferencing/session/tool_registry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
};

struct FunctionSchema {
  Json properties = Json::object();
  std::unordered_set<std::string> required;
  bool has_parameters = false;
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
  };
  return kSupportedAnnotations.contains(keyword);
}

bool DoubleEqualsSignedInteger(double floating, std::int64_t integer) {
  constexpr double kLowerBound = -9223372036854775808.0;
  constexpr double kUpperBound = 9223372036854775808.0;
  return std::isfinite(floating) && std::trunc(floating) == floating && floating >= kLowerBound &&
         floating < kUpperBound && static_cast<std::int64_t>(floating) == integer;
}

bool DoubleEqualsUnsignedInteger(double floating, std::uint64_t integer) {
  constexpr double kUpperBound = 18446744073709551616.0;
  return std::isfinite(floating) && std::trunc(floating) == floating && floating >= 0.0 &&
         floating < kUpperBound && static_cast<std::uint64_t>(floating) == integer;
}

bool JsonNumbersEqual(const Json& lhs, const Json& rhs) {
  if (lhs.is_number_float()) {
    const auto floating = lhs.get<double>();
    if (rhs.is_number_float()) {
      return floating == rhs.get<double>();
    }
    if (rhs.is_number_unsigned()) {
      return DoubleEqualsUnsignedInteger(floating, rhs.get<std::uint64_t>());
    }
    return DoubleEqualsSignedInteger(floating, rhs.get<std::int64_t>());
  }
  if (lhs.is_number_unsigned()) {
    const auto integer = lhs.get<std::uint64_t>();
    if (rhs.is_number_float()) {
      return DoubleEqualsUnsignedInteger(rhs.get<double>(), integer);
    }
    if (rhs.is_number_unsigned()) {
      return integer == rhs.get<std::uint64_t>();
    }
    const auto signed_integer = rhs.get<std::int64_t>();
    return signed_integer >= 0 && integer == static_cast<std::uint64_t>(signed_integer);
  }

  const auto integer = lhs.get<std::int64_t>();
  if (rhs.is_number_float()) {
    return DoubleEqualsSignedInteger(rhs.get<double>(), integer);
  }
  if (rhs.is_number_unsigned()) {
    return integer >= 0 && static_cast<std::uint64_t>(integer) == rhs.get<std::uint64_t>();
  }
  return integer == rhs.get<std::int64_t>();
}

bool JsonScalarEquals(const Json& lhs, const Json& rhs) {
  return lhs.is_number() && rhs.is_number() ? JsonNumbersEqual(lhs, rhs) : lhs == rhs;
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

bool IsSupportedEnum(const Json& schema, std::string_view type) {
  if (!schema.contains("enum")) {
    return true;
  }

  const auto& values = schema["enum"];
  if (!values.is_array() || values.empty() ||
      (type != "string" && type != "number" && type != "integer" && type != "boolean" && type != "null")) {
    return false;
  }

  for (size_t index = 0; index < values.size(); ++index) {
    if (!IsCompatibleScalarType(values[index], type) ||
        std::ranges::any_of(values.begin(), values.begin() + static_cast<Json::difference_type>(index),
                            [&](const auto& prior) { return JsonScalarEquals(prior, values[index]); })) {
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
    return item.key() == "additionalProperties" && item.value().is_boolean();
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
      schema.valid = true;
      insert_schema(name, std::move(schema));
      continue;
    }

    const auto& parameters = (*function)["parameters"];
    if (kind->second == ToolKind::kCustom && parameters != custom_tool_schema) {
      insert_schema(name, std::move(schema));
      continue;
    }

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
  auto state = ConsumeLiteral(source, position, "<tool_call>\n");
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
      return BlockResult(ParseState::kInvalid);
    }

    const auto body_end = source.find(kParameterEnd, position);
    if (body_end == std::string_view::npos) {
      return BlockResult(source.find(kQwenXmlToolCallEndMarker, position) == std::string_view::npos
                             ? ParseState::kQualifiedIncomplete
                             : ParseState::kStructuralFailure);
    }

    const auto body = source.substr(position, body_end - position);
    if (ContainsReservedFramingMarkup(body)) {
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
    const auto end = source.find(kQwenXmlToolCallEndMarker, position);
    if (end == std::string_view::npos) {
      return end_of_stream ? source.size() : 0;
    }

    const auto block_end = end + kQwenXmlToolCallEndMarker.size();
    position = source.find_first_not_of(" \t\r\n", block_end);
    if (position == std::string_view::npos) {
      return end_of_stream ? block_end : 0;
    }

    const auto remaining = source.substr(position);
    if (remaining.starts_with(kQwenXmlToolCallStartMarker)) {
      position += kQwenXmlToolCallStartMarker.size();
      continue;
    }
    if (kQwenXmlToolCallStartMarker.starts_with(remaining) && !end_of_stream) {
      return 0;
    }

    return block_end;
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
    if (block.state == ParseState::kInvalid || block.state == ParseState::kStructuralFailure) {
      const auto rejected_end = RejectedBatchEnd(source, position, end_of_stream);
      if (rejected_end == 0) {
        return {};
      }

      return {
          .disposition = recovery_aware && calls.empty() &&
                                 block.state == ParseState::kStructuralFailure
                             ? ToolCallPayloadDisposition::kMalformed
                             : ToolCallPayloadDisposition::kRejected,
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
      std::ranges::any_of(schemas, [](const auto& schema) {
        return !schema.second.valid;
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
      std::ranges::any_of(schemas, [](const auto& schema) {
        return !schema.second.valid;
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
