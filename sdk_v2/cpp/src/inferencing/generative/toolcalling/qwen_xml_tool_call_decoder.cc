// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/qwen_xml_tool_call_decoder.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/session/tool_registry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>
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
constexpr std::string_view kFunctionClose = "</function>\n";
constexpr std::string_view kFunctionEnd = "</function>\n</tool_call>";
constexpr std::string_view kParameterEnd = "\n</parameter>\n";
constexpr size_t kMaxSchemaNesting = 16;
constexpr size_t kMaxSchemaNormalizationBytes = 8 * 1024 * 1024;
constexpr size_t kMaxQwenXmlGrammarBytes = 1024 * 1024;
constexpr std::array<std::string_view, 6> kReservedMarkup = {
    kQwenXmlToolCallStartMarker,
    kQwenXmlToolCallEndMarker,
    kFunctionPrefix,
    "</function>",
    kParameterPrefix,
    "</parameter>",
};

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

bool HasCompleteDeclaredSet(const FunctionSchemas& schemas, size_t declaration_count,
                            const std::unordered_map<std::string, ToolKind>& tool_kinds) {
  return declaration_count != 0 && schemas.size() == declaration_count &&
         schemas.size() == tool_kinds.size();
}

ToolCallPayloadParseResult RejectUndecodableToolCall(std::string_view source, bool) {
  return {
      .disposition = ToolCallPayloadDisposition::kMalformed,
      .consumed_size = source.size(),
      .calls = {},
  };
}

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
      "$schema",
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

  return std::ranges::all_of(values, [type](const auto& value) {
    return IsSupportedEnumValue(value, type);
  });
}

// Resolve only local definitions. Expansion is bounded and cyclic references remain ineligible.
std::optional<Json> NormalizeParameterSchema(const Json& schema, const Json& definitions,
                                             size_t& remaining_bytes, size_t depth = 0) {
  if (!schema.is_object() || depth >= kMaxSchemaNesting) {
    return std::nullopt;
  }

  const auto schema_size = schema.dump().size();
  if (schema_size > remaining_bytes) {
    return std::nullopt;
  }
  remaining_bytes -= schema_size;

  if (schema.contains("$ref")) {
    if (schema.size() != 1 || !schema["$ref"].is_string()) {
      return std::nullopt;
    }

    const auto& ref = schema["$ref"].get_ref<const std::string&>();
    constexpr std::string_view prefix = "#/$defs/";
    if (!ref.starts_with(prefix) || ref.size() == prefix.size() ||
        ref.find_first_of("~/", prefix.size()) != std::string::npos ||
        !definitions.is_object() || !definitions.contains(ref.substr(prefix.size()))) {
      return std::nullopt;
    }

    return NormalizeParameterSchema(definitions[ref.substr(prefix.size())], definitions, remaining_bytes, depth + 1);
  }

  Json result = schema;
  if (result.contains("type") && result["type"].is_array()) {
    // Erasing "type" invalidates references into result.
    const Json types = result["type"];
    if (types.empty() || !std::ranges::all_of(types, [](const auto& type) { return type.is_string(); })) {
      return std::nullopt;
    }

    result.erase("type");
    Json branches = Json::array();
    if (types.size() > remaining_bytes / schema_size) {
      return std::nullopt;
    }
    for (const auto& type : types) {
      Json branch = result;
      branch["type"] = type;
      branches.push_back(std::move(branch));
    }

    result = {{"anyOf", std::move(branches)}};
  }

  if (result.contains("enum") && result["enum"].is_array()) {
    Json unique = Json::array();
    std::unordered_set<std::string> seen;
    for (const auto& value : result["enum"]) {
      if (seen.insert(EnumValueKey(value)).second) {
        unique.push_back(value);
      }
    }

    result["enum"] = std::move(unique);
  }

  const auto normalize_child = [&](Json& child) {
    auto normalized = NormalizeParameterSchema(child, definitions, remaining_bytes, depth + 1);
    if (!normalized) {
      return false;
    }

    child = std::move(*normalized);
    return true;
  };

  for (const auto* key : {"anyOf", "oneOf", "items", "properties", "additionalProperties"}) {
    if (!result.contains(key)) {
      continue;
    }

    auto& value = result[key];
    if ((std::string_view(key) == "anyOf" || std::string_view(key) == "oneOf") && value.is_array()) {
      if (!std::ranges::all_of(value, normalize_child)) {
        return std::nullopt;
      }
    } else if (std::string_view(key) == "properties" && value.is_object()) {
      for (auto& property : value.items()) {
        if (!normalize_child(property.value())) {
          return std::nullopt;
        }
      }
    } else if (std::string_view(key) == "items" && value.is_object() ||
               std::string_view(key) == "additionalProperties" && value.is_object()) {
      if (!normalize_child(value)) {
        return std::nullopt;
      }
    }
  }

  return result;
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
      return branch.is_object() && !branch.contains("anyOf") && !branch.contains("oneOf") &&
             IsSupportedParameterSchema(branch, depth + 1);
    });
  }

  if (schema.contains("oneOf")) {
    // Top-level XML values have special string decoding, so only nested JSON values may use exclusive unions.
    if (depth == 0 || schema.contains("allOf") || schema.contains("not") || schema.contains("if") ||
        !schema["oneOf"].is_array() || schema["oneOf"].empty() ||
        std::ranges::any_of(schema.items(), [](const auto& item) {
          return item.key() != "oneOf" && !IsSupportedAnnotation(item.key());
        })) {
      return false;
    }

    return std::ranges::all_of(schema["oneOf"], [depth](const auto& branch) {
      return IsSupportedParameterSchema(branch, depth + 1);
    });
  }

  const auto type = GetSupportedType(schema);
  if (!type.has_value()) {
    return false;
  }

  if (std::ranges::any_of(schema.items(), [&](const auto& item) {
        return item.key() != "type" && !IsSupportedAnnotation(item.key()) &&
               item.key() != "enum" && item.key() != "const" &&
               !(*type == "array" && item.key() == "items") &&
               !(*type == "object" && (item.key() == "properties" || item.key() == "required" ||
                                        item.key() == "additionalProperties"));
      })) {
    return false;
  }

  if (!IsSupportedEnum(schema, *type) ||
      (schema.contains("const") &&
       (!IsSupportedEnumValue(schema["const"], *type) || schema.contains("enum")))) {
    return false;
  }

  if (*type == "array") {
    return !schema.contains("items") || IsSupportedParameterSchema(schema["items"], depth + 1);
  }

  if (*type != "object") {
    return true;
  }

  if (schema.contains("properties") && !schema["properties"].is_object()) {
    return false;
  }

  if (schema.contains("additionalProperties") && !schema["additionalProperties"].is_boolean() &&
      !IsSupportedParameterSchema(schema["additionalProperties"], depth + 1)) {
    return false;
  }

  if (schema.contains("required")) {
    if (!schema["required"].is_array() ||
        !std::ranges::all_of(schema["required"], [](const auto& item) { return item.is_string(); })) {
      return false;
    }

    std::unordered_set<std::string> required;
    for (const auto& item : schema["required"]) {
      const auto& name = item.get_ref<const std::string&>();
      if (!required.insert(name).second || !schema.value("properties", Json::object()).contains(name)) {
        return false;
      }
    }
  }

  return !schema.contains("properties") ||
         std::ranges::all_of(schema["properties"].items(), [depth](const auto& item) {
           return IsSupportedParameterSchema(item.value(), depth + 1);
         });
}

bool IsSupportedParametersObject(const Json& schema) {
  if (!schema.is_object() || HasUnsupportedComposition(schema)) {
    return false;
  }

  return std::ranges::all_of(schema.items(), [](const auto& item) {
    if (IsSupportedAnnotation(item.key()) || item.key() == "$defs") {
      return true;
    }
    if (item.key() == "type" || item.key() == "properties" || item.key() == "required") {
      return true;
    }
    return item.key() == "additionalProperties" && item.value() == false;
  });
}

FunctionSchemas ParseFunctionSchemas(
    const std::string& tools_json,
    const std::unordered_map<std::string, ToolKind>& tool_kinds,
    size_t& declaration_count) {
  FunctionSchemas schemas;
  if (tools_json.size() > kMaxSchemaNormalizationBytes) {
    return schemas;
  }

  size_t remaining_bytes = kMaxSchemaNormalizationBytes;
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
    const auto& definitions = parameters.value("$defs", Json::object());
    for (auto& property : schema.properties.items()) {
      auto normalized = NormalizeParameterSchema(property.value(), definitions, remaining_bytes);
      if (!normalized) {
        property.value() = Json();
        continue;
      }

      property.value() = std::move(*normalized);
    }
    const bool properties_valid = std::ranges::all_of(schema.properties.items(), [](const auto& property) {
      return IsSupportedParameterSchema(property.value());
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

bool IsCompatibleParameterValue(const Json& value, const Json& schema, size_t depth = 0);

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
                    return IsCompatibleParameterValue(item, schema["items"], depth + 1);
                  }));
  }
  if (*type == "object") {
    compatible = value.is_object();
    if (compatible) {
      const auto& properties = schema.value("properties", Json::object());
      compatible = (!schema.contains("required") ||
                    std::ranges::all_of(schema["required"], [&](const auto& required) {
                      return value.contains(required.template get_ref<const std::string&>());
                    })) &&
                   std::ranges::all_of(value.items(), [&](const auto& item) {
                     if (properties.contains(item.key())) {
                       return IsCompatibleParameterValue(item.value(), properties[item.key()], depth + 1);
                     }

                     if (!schema.contains("additionalProperties")) {
                       return true;
                     }

                     const auto& additional = schema["additionalProperties"];
                     return additional.is_boolean() ? additional.get<bool>() :
                            IsCompatibleParameterValue(item.value(), additional, depth + 1);
                   });
    }
  }
  return compatible &&
         (!schema.contains("enum") ||
          std::ranges::any_of(schema["enum"], [&](const auto& expected) {
            return JsonScalarEquals(value, expected);
          })) &&
         (!schema.contains("const") || JsonScalarEquals(value, schema["const"]));
}

bool IsCompatibleParameterValue(const Json& value, const Json& schema, size_t depth) {
  if (depth >= kMaxSchemaNesting) {
    return false;
  }

  if (schema.contains("oneOf")) {
    size_t matches = 0;
    for (const auto& branch : schema["oneOf"]) {
      matches += IsCompatibleParameterValue(value, branch, depth + 1) ? 1u : 0u;
      if (matches > 1) {
        return false;
      }
    }

    return matches == 1;
  }

  if (!schema.contains("anyOf")) {
    return IsCompatibleJsonValue(value, schema, depth);
  }

  return std::ranges::any_of(schema["anyOf"], [&](const auto& branch) {
    return IsCompatibleParameterValue(value, branch, depth + 1);
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

  const auto value = ParseJsonWithoutDuplicateObjectKeys(body);
  if (!value || !IsCompatibleJsonValue(*value, schema)) {
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

bool HasSiblingFramingBefore(std::string_view source, size_t sibling, size_t body_end) {
  size_t name_position = sibling + kCallStart.size();
  if (!ReadTagName(source, name_position, kFunctionPrefix)) {
    return false;
  }

  // A bare header can be literal body text; a parameter or function close before the body's close is ambiguous.
  const auto parameter = source.find(kParameterPrefix, name_position);
  const auto function_end = source.find(kFunctionClose, name_position);
  return (parameter != std::string_view::npos &&
          (body_end == std::string_view::npos || parameter < body_end)) ||
         (function_end != std::string_view::npos &&
          (body_end == std::string_view::npos || function_end < body_end));
}

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
      const auto outer_end = source.find(kFunctionClose, position);
      for (auto call_end = source.find(kQwenXmlToolCallEndMarker, position);
           call_end != std::string_view::npos &&
           (body_end == std::string_view::npos || call_end < body_end);
           call_end = source.find(kQwenXmlToolCallEndMarker,
                                  call_end + kQwenXmlToolCallEndMarker.size())) {
        const auto next = source.find_first_not_of(" \t\r\n", call_end + kQwenXmlToolCallEndMarker.size());
        if (next != std::string_view::npos && source.substr(next).starts_with(kCallStart)) {
          if (HasSiblingFramingBefore(source, next, body_end)) {
            return {source.size(), true};
          }
        }
      }
      if (outer_end != std::string_view::npos &&
          (body_end == std::string_view::npos || outer_end < body_end)) {
        for (auto sibling = source.find(kCallStart, outer_end + kFunctionClose.size());
             sibling != std::string_view::npos &&
             (body_end == std::string_view::npos || sibling < body_end);
             sibling = source.find(kCallStart, sibling + kCallStart.size())) {
          if (HasSiblingFramingBefore(source, sibling, body_end)) {
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
  const auto boundary = function_end != std::string_view::npos &&
                                (end == std::string_view::npos || function_end < end)
                            ? function_end + kFunctionEnd.size()
                            : end == std::string_view::npos ? source.size() : end;
  for (auto sibling = source.find(kCallStart, position);
       sibling != std::string_view::npos && sibling < boundary;
       sibling = source.find(kCallStart, sibling + kCallStart.size())) {
    if (HasSiblingFramingBefore(source, sibling, boundary)) {
      return {source.size(), true};
    }
  }

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

std::optional<std::string> BuildQwenXmlToolBodyGrammar(
    const std::string& tools_json, const std::unordered_map<std::string, ToolKind>& tool_kinds) {
  size_t declaration_count = 0;
  const auto schemas = ParseFunctionSchemas(tools_json, tool_kinds, declaration_count);
  if (!HasCompleteDeclaredSet(schemas, declaration_count, tool_kinds)) {
    return std::nullopt;
  }

  std::vector<std::string> names;
  names.reserve(schemas.size());
  for (const auto& [name, schema] : schemas) {
    if (!schema.valid || tool_kinds.at(name) != ToolKind::kFunction ||
        name.find_first_of("<>=\r\n\t ") != std::string::npos || schema.properties.size() > 64) {
      return std::nullopt;
    }

    for (const auto& [property, definition] : schema.properties.items()) {
      if (property.empty() || property.find_first_of("<>=\r\n\t ") != std::string::npos) {
        return std::nullopt;
      }
    }

    names.push_back(name);
  }

  std::ranges::sort(names);
  std::ostringstream grammar;
  grammar << "start: \"\\n\" (";
  for (size_t index = 0; index < names.size(); ++index) {
    if (index != 0) {
      grammar << " | ";
    }
    grammar << "tool_" << index;
  }
  grammar << ")\n";
  // Keep code values and '<' intact while excluding the framing markup rejected by the strict decoder.
  grammar << "body[suffix=\"\\n</parameter>\\n\"]: BODY\n"
             "BODY: /(?s:.*)/ & ~/(?s:.*)(";
  for (size_t index = 0; index < kReservedMarkup.size(); ++index) {
    if (index != 0) {
      grammar << "|";
    }
    for (const char ch : kReservedMarkup[index]) {
      if (ch == '/') {
        grammar << "\\";
      }
      grammar << ch;
    }
  }
  grammar << ")(?s:.*)/\n"
             "ws: /[ \\t\\r\\n]*/\n"
             "json_string: /\"([^\"\\\\\\x00-\\x1f]|\\\\([\"\\\\\\/bfnrt]|u[0-9a-fA-F]{4}))*\"/\n"
             "string_array: ws \"[\" ws (json_string (ws \",\" ws json_string)*)? ws \"]\" ws\n";

  for (size_t index = 0; index < names.size(); ++index) {
    const auto& schema = schemas.at(names[index]);
    std::vector<std::string> properties;
    // Required fields lead so the model can still emit optional paths after the required grep pattern.
    // Preserve schema iteration order within each group; the strict decoder accepts either XML field order.
    for (const bool required_field : {true, false}) {
      for (const auto& [property, definition] : schema.properties.items()) {
        if (schema.required.contains(property) == required_field) {
          properties.push_back(property);
        }
      }
    }

    grammar << "tool_" << index << ": " << EscapeLarkLiteral("<function=" + names[index] + ">\n")
            << " state_" << index << "_0\n";
    // Canonical key order needs only N+1 states; optional fields can be skipped but never repeated.
    for (size_t field = 0; field < properties.size(); ++field) {
      const auto& property = properties[field];
      const auto& definition = schema.properties[property];
      const bool string_array = GetSupportedType(definition) == "array" &&
                                definition.contains("items") &&
                                GetSupportedType(definition["items"]) == "string" &&
                                !definition["items"].contains("enum");
      grammar << "state_" << index << "_" << field << ": "
              << EscapeLarkLiteral("<parameter=" + property + ">\n") << " ";
      if (string_array) {
        grammar << "string_array " << EscapeLarkLiteral("\n</parameter>\n");
      } else {
        // The strict decoder validates JSON shape, union membership and nested constraints before admission.
        grammar << "body";
      }

      grammar << " state_" << index << "_" << (field + 1);
      if (!schema.required.contains(property)) {
        grammar << " | state_" << index << "_" << (field + 1);
      }

      grammar << "\n";
    }

    grammar << "state_" << index << "_" << properties.size() << ": "
            << EscapeLarkLiteral("</function>\n") << "\n";
  }

  auto result = grammar.str();
  if (result.size() > kMaxQwenXmlGrammarBytes) {
    return std::nullopt;
  }

  return result;
}

std::optional<std::string> PlanQwenXmlToolBodyGuidance(
    const ToolCallContext& context, bool native_qwen_xml, ChatBackendKind backend_kind) {
  if (!native_qwen_xml || backend_kind != ChatBackendKind::kEngine ||
      !context.tool_output || !context.text_output || context.forced_tool ||
      context.ActiveRawEnvelope() || context.guidance_disabled || context.HasAnyExplicitGuidance() ||
      context.tool_call_start != kQwenXmlToolCallStartMarker ||
      context.tool_call_end != kQwenXmlToolCallEndMarker ||
      !context.tool_call_start_token_id || !context.tool_call_end_token_id ||
      *context.tool_call_start_token_id < 0 || *context.tool_call_end_token_id < 0 ||
      context.tool_call_start_token_id == context.tool_call_end_token_id) {
    return std::nullopt;
  }

  return BuildQwenXmlToolBodyGrammar(context.tools_json, context.tool_kinds);
}

ToolCallPayloadParser CreateQwenXmlToolCallPayloadParser(
    std::string tools_json, std::unordered_map<std::string, ToolKind> tool_kinds,
    bool recovery_aware) {
  size_t declaration_count = 0;
  auto schemas = ParseFunctionSchemas(tools_json, tool_kinds, declaration_count);
  if (!HasCompleteDeclaredSet(schemas, declaration_count, tool_kinds)) {
    // A present but undecodable offer must retain the selected parser: falling back to the generic
    // accumulator would expose native XML as assistant text instead of failing the tool turn closed.
    if (tools_json.empty() && tool_kinds.empty()) {
      return {};
    }

    return RejectUndecodableToolCall;
  }

  const bool unsupported_only = std::ranges::none_of(schemas, [](const auto& schema) {
    return schema.second.recognizable;
  });

  return [schemas = std::move(schemas), recovery_aware, unsupported_only](
             std::string_view source, bool end_of_stream) {
    if (unsupported_only) {
      return RejectUndecodableToolCall(source, end_of_stream);
    }

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
  const auto schemas = ParseFunctionSchemas(tools_json, tool_kinds, declaration_count);
  if (!HasCompleteDeclaredSet(schemas, declaration_count, tool_kinds) ||
      !std::ranges::all_of(schemas, [](const auto& schema) {
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
