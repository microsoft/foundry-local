// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/tool_definitions.h"

#include "exception.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fl {
namespace tools {

namespace {

/// The neutral schema for a function tool that declares no parameters. The registry requires a
/// schema; this is the JSON Schema spelling of "no constraints stated".
constexpr const char* kEmptyFunctionSchema = "{}";

constexpr const char* kTextFormatType = "text";

std::optional<std::string> ReadGrammar(const nlohmann::json& value) {
  const auto syntax = value.find("syntax");
  const auto definition = value.find("definition");
  if (syntax == value.end() || !syntax->is_string() || syntax->get<std::string>() != "lark" ||
      definition == value.end() || !definition->is_string()) {
    return std::nullopt;
  }

  return definition->get<std::string>();
}
}  // namespace

nlohmann::json ParseCustomToolFormat(const nlohmann::json& format, const std::string& tool_name,
                                     CustomToolFormatSurface surface) {
  if (format.is_null()) {
    return {{"type", kTextFormatType}};
  }

  if (!format.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "custom tool '", tool_name,
             "' format must be an object");
  }

  auto type = format.find("type");
  if (type == format.end() || !type->is_string()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "custom tool '", tool_name,
             "' format must contain a string 'type'");
  }

  const auto type_name = type->get<std::string>();
  if (type_name == "grammar") {
    const auto grammar_member = format.find("grammar");
    const bool chat = surface == CustomToolFormatSurface::kChatCompletions;
    const bool exact_shape =
        chat ? format.size() == 2 && grammar_member != format.end() && grammar_member->is_object() &&
                   grammar_member->size() == 2
             : format.size() == 3 && grammar_member == format.end();
    const auto grammar = exact_shape ? ReadGrammar(chat ? *grammar_member : format) : std::nullopt;
    if (tool_name != "apply_patch" || !grammar.has_value() ||
        *grammar != kStockGhcpApplyPatchLarkGrammar) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "custom tool '", tool_name,
               "' may use only the exact stock GHCP apply-patch Lark grammar");
    }

    return format;
  }

  if (type_name != kTextFormatType) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "custom tool '", tool_name, "' declares unsupported format type '",
             type_name, "'; only 'text' is supported");
  }

  if (format.size() != 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "custom tool '", tool_name,
             "' declares a text format carrying additional members; 'text' takes no parameters");
  }

  return {{"type", kTextFormatType}};
}

std::optional<std::string> CustomToolLarkGrammar(const nlohmann::json& format) {
  if (!format.is_object() || format.value("type", "") != "grammar") {
    return std::nullopt;
  }

  const auto grammar = format.find("grammar");
  return ReadGrammar(grammar != format.end() && grammar->is_object() ? *grammar : format);
}

RawEnvelopeDescriptor ParseRawEnvelopeDescriptor(const std::string& value) {
  nlohmann::json descriptor;
  try {
    descriptor = nlohmann::json::parse(value);
  } catch (const nlohmann::json::exception& error) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, kRawEnvelopeMetadataKey,
             " must be valid JSON: ", error.what());
  }

  constexpr std::array<const char*, 4> keys{"type", "tool_name", "start_marker", "end_marker"};
  if (!descriptor.is_object() || descriptor.size() != keys.size()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, kRawEnvelopeMetadataKey,
             " must contain only type, tool_name, start_marker, and end_marker");
  }

  for (const auto* key : keys) {
    if (!descriptor.contains(key) || !descriptor[key].is_string()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, kRawEnvelopeMetadataKey,
               " requires string member '", key, "'");
    }
  }

  if (descriptor["type"] != "raw_envelope") {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, kRawEnvelopeMetadataKey,
             " supports only type 'raw_envelope'");
  }

  RawEnvelopeDescriptor result{descriptor["tool_name"].get<std::string>(),
                               descriptor["start_marker"].get<std::string>(),
                               descriptor["end_marker"].get<std::string>()};
  if (result.tool_name.empty() || result.start_marker.empty() || result.end_marker.empty() ||
      result.start_marker == result.end_marker ||
      result.start_marker.find_first_of("\r\n") != std::string::npos ||
      result.end_marker.find_first_of("\r\n") != std::string::npos) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, kRawEnvelopeMetadataKey,
             " requires non-empty, distinct, single-line markers and a non-empty tool_name");
  }

  return result;
}

std::optional<bool> ParseFunctionStrict(const nlohmann::json& strict) {
  if (strict.is_null()) {
    return std::nullopt;
  }

  if (!strict.is_boolean()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "function tool 'strict' must be a boolean or null");
  }

  if (strict.get<bool>()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "function tool 'strict' true is not supported until constrained decoding is implemented");
  }

  return false;
}

ToolDefinition MakeFunctionTool(std::string name, std::string description, std::string json_schema,
                                bool description_present, bool parameters_present,
                                std::optional<bool> strict) {
  ToolDefinition definition;
  definition.name = std::move(name);
  definition.description = std::move(description);
  definition.json_schema = json_schema.empty() ? kEmptyFunctionSchema : std::move(json_schema);
  definition.kind = ToolKind::kFunction;
  definition.include_description_in_prompt = description_present;
  definition.include_parameters_in_prompt = parameters_present;
  definition.strict = strict;
  return definition;
}

ToolDefinition MakeCustomTool(std::string name, std::string description, bool description_present,
                              std::optional<std::string> custom_lark_grammar) {
  ToolDefinition definition;
  definition.name = std::move(name);
  definition.description = std::move(description);
  definition.kind = ToolKind::kCustom;
  definition.include_description_in_prompt = description_present;
  definition.custom_lark_grammar = std::move(custom_lark_grammar);
  return definition;
}

std::unordered_map<std::string, ToolKind> KindsByName(const std::vector<ToolDefinition>& definitions) {
  std::unordered_map<std::string, ToolKind> kinds;
  kinds.reserve(definitions.size());

  for (const auto& definition : definitions) {
    // Version 1 C ABI callers may register one unnamed definition containing a complete,
    // pre-serialized tools array. It has no generated-call name to classify.
    if (!definition.name.empty()) {
      kinds.emplace(definition.name, definition.kind);
    }
  }

  return kinds;
}

void NarrowToForcedTool(std::vector<ToolDefinition>& definitions, const std::string& name, ToolKind kind) {
  auto forced = std::find_if(definitions.begin(), definitions.end(), [&](const ToolDefinition& definition) {
    return definition.name == name && definition.kind == kind;
  });

  if (forced == definitions.end()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "tool_choice references an undeclared tool or the wrong tool kind: " + name);
  }

  ToolDefinition only = std::move(*forced);
  definitions.clear();
  definitions.push_back(std::move(only));
}

void RetainAllowedTools(std::vector<ToolDefinition>& definitions, const std::vector<std::string>& allowed_names) {
  std::unordered_set<std::string> allowed;
  allowed.reserve(allowed_names.size());
  for (const auto& name : allowed_names) {
    allowed.insert(ToLower(name));
  }

  auto excluded = std::remove_if(definitions.begin(), definitions.end(), [&](const ToolDefinition& definition) {
    return allowed.count(ToLower(definition.name)) == 0;
  });

  definitions.erase(excluded, definitions.end());
}

}  // namespace tools
}  // namespace fl
