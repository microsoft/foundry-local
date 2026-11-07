// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/tool_call_plan.h"

#include "exception.h"
#include "inferencing/session/tool_registry.h"
#include "util/json_helpers.h"

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace fl {
namespace {

using Json = nlohmann::ordered_json;
using ValidatorJson = jsoncons::json;
using Validator = jsoncons::jsonschema::json_schema<ValidatorJson>;
namespace schema = jsoncons::jsonschema;

constexpr size_t kMaxSchemaVisits = 16384;

[[noreturn]] void Fail(flErrorCode code, std::string_view context, std::string_view detail) {
  FL_THROW(code, context, ": ", detail);
}

std::string PointerToken(std::string_view text) {
  std::string result;
  for (const char c : text) {
    result += c == '~' ? "~0" : c == '/' ? "~1"
                                         : std::string(1, c);
  }

  return result;
}

void CheckJsonDepth(std::string_view text, flErrorCode code, const std::string& context) {
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (const char c : text) {
    if (quoted) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        quoted = false;
      }

      continue;
    }

    if (c == '"') {
      quoted = true;
    } else if (c == '{' || c == '[') {
      if (++depth > ToolCallPlan::kMaxDepth) {
        Fail(code, context, "JSON nesting limit exceeded (" + std::to_string(ToolCallPlan::kMaxDepth) + ")");
      }
    } else if ((c == '}' || c == ']') && depth > 0) {
      --depth;
    }
  }
}

Json ParseJson(std::string_view text, size_t limit, flErrorCode code, const std::string& context) {
  if (text.size() > limit) {
    Fail(code, context, "JSON byte limit exceeded (" + std::to_string(limit) + ")");
  }

  try {
    // Bound nesting before DOM allocation. Syntax and duplicate-key checks belong to the shared strict parser.
    CheckJsonDepth(text, code, context);
    return ParseJsonStrict(text);
  } catch (const Exception&) {
    throw;
  } catch (const std::exception& e) {
    Fail(code, context, e.what());
  }
}

std::string ValidationDetail(const schema::validation_message& message) {
  auto result = "instance '" + message.instance_location().string() + "', keyword '" +
                message.eval_path().string() + "': " + message.message();
  for (const auto& detail : message.details()) {
    result += "; " + ValidationDetail(detail);
  }

  return result;
}

void Validate(const Validator& validator, const ValidatorJson& value, flErrorCode code,
              const std::string& context) {
  validator.validate(value, [&](const schema::validation_message& message) -> schema::walk_state {
    Fail(code, context, ValidationDetail(message));
  });
}

Validator Compile(const Json& document) {
  // Without a resolver, jsoncons only resolves its bundled metaschemas; it never fetches a URI.
  schema::evaluation_options options;
  options.require_format_validation(false);
  return schema::make_json_schema(ValidatorJson::parse(document.dump()), options);
}

void ValidateMetaSchema(const Json& document, bool draft7, const std::string& context) {
  static const auto draft7_meta =
      schema::make_json_schema(schema::draft7::schema_draft7<ValidatorJson>::get_schema());
  static const auto modern_meta =
      schema::make_json_schema(schema::draft202012::schema_draft202012<ValidatorJson>::get_schema());
  Validate(draft7 ? draft7_meta : modern_meta, ValidatorJson::parse(document.dump()),
           FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, context);
}

bool IsDraft7(const Json& document, const std::string& context) {
  const auto dialect = document.is_object()
                           ? document.value("$schema", std::string(schema::schema_version::draft202012()))
                           : std::string(schema::schema_version::draft202012());
  if (dialect == schema::schema_version::draft7()) {
    return true;
  }

  if (dialect != schema::schema_version::draft202012()) {
    Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, context + "/$schema", "unsupported JSON Schema dialect");
  }

  return false;
}

// Visit only schema-valued positions: a literal default/const/enum may itself contain "$ref" or other keys.
void WalkSchema(Json& node, const std::string& path,
                const std::function<void(Json&, const std::string&)>& visitor) {
  visitor(node, path);
  if (node.is_boolean()) {
    return;
  }

  for (auto& [key, value] : node.items()) {
    const auto child_path = path + "/" + PointerToken(key);
    if (key == "properties" || key == "patternProperties" || key == "$defs" || key == "definitions" ||
        key == "dependentSchemas") {
      for (auto& [name, child] : value.items()) {
        WalkSchema(child, child_path + "/" + PointerToken(name), visitor);
      }
    } else if (key == "allOf" || key == "anyOf" || key == "oneOf" || key == "prefixItems") {
      for (size_t i = 0; i < value.size(); ++i) {
        WalkSchema(value[i], child_path + "/" + std::to_string(i), visitor);
      }
    } else if (key == "items" || key == "additionalProperties" || key == "contains" ||
               key == "propertyNames" || key == "not" || key == "if" || key == "then" || key == "else" ||
               key == "unevaluatedProperties" || key == "unevaluatedItems") {
      WalkSchema(value, child_path, visitor);
    }
  }
}

void CheckProfile(Json& document, bool draft7, const std::string& context) {
  static const std::set<std::string> core = {"$schema", "$ref", "$comment"};
  static const std::set<std::string> modern = {
      "$defs", "prefixItems", "dependentRequired", "dependentSchemas", "unevaluatedProperties", "unevaluatedItems",
      "minContains", "maxContains"};
  static const std::set<std::string> unsupported = {
      "dependencies", "additionalItems", "contentEncoding", "contentMediaType", "contentSchema"};

  WalkSchema(document, "", [&](Json& node, const std::string& path) {
    if (node.is_boolean()) {
      return;
    }

    for (const auto& [key, value] : node.items()) {
      const auto location = context + path + "/" + PointerToken(key);
      // Unknown non-reserved keywords are annotations in the admitted dialects, not extra assertions.
      if ((key.starts_with('$') && !core.contains(key) && !(!draft7 && key == "$defs")) ||
          unsupported.contains(key) || (draft7 && modern.contains(key)) || (!draft7 && key == "definitions")) {
        Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, location, "keyword is outside the offline schema profile");
      }

      if (key == "$schema" && !path.empty()) {
        Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, location, "nested dialect changes are unsupported");
      }

      if (key == "items" && value.is_array()) {
        Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, location, "Draft 7 tuple items are unsupported; use 2020-12");
      }

      if (key == "$ref") {
        const auto ref = value.get<std::string>();
        if (ref != "#" && !ref.starts_with("#/")) {
          Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, location, "only local JSON Pointer references are supported");
        }

        if (ref.find('%') != std::string::npos) {
          Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, location, "percent-encoded references are unsupported");
        }
      }
    }
  });
}

void CheckReferences(Json& document, const std::string& context) {
  std::set<const Json*> schema_nodes;
  WalkSchema(document, "", [&](Json& node, const std::string&) { schema_nodes.insert(&node); });
  std::set<std::string> active;
  size_t visits = 0;
  std::function<void(Json&, const std::string&)> check;
  check = [&](Json& node, const std::string& path) {
    if (++visits > kMaxSchemaVisits) {
      Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, context + path, "schema reference expansion limit exceeded");
    }

    if (!node.contains("$ref")) {
      return;
    }

    const auto ref = node.at("$ref").get<std::string>();
    if (!active.insert(ref).second || active.size() > static_cast<size_t>(ToolCallPlan::kMaxDepth)) {
      Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, context + path + "/$ref", "recursive reference is unsupported");
    }

    Json* target = nullptr;
    try {
      target = &document.at(Json::json_pointer(ref.substr(1)));
    } catch (const std::exception& e) {
      Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, context + path + "/$ref", e.what());
    }

    if (!schema_nodes.contains(target)) {
      Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, context + path + "/$ref",
           "reference target must be a declared schema location");
    }

    WalkSchema(*target, ref.substr(1), check);
    active.erase(ref);
  };
  WalkSchema(document, "", check);
}

std::string BuildGrammar(const std::vector<std::string>& names, bool parallel) {
  std::string grammar = parallel
                            ? "start: tool_json_ws \"[\" tool_json_ws tool_json_call "
                              "(tool_json_comma tool_json_call)* tool_json_ws \"]\" tool_json_ws\n"
                            : "start: tool_json_ws \"[\" tool_json_ws tool_json_call "
                              "tool_json_ws \"]\" tool_json_ws\n";
  grammar += "tool_json_name: ";
  for (size_t i = 0; i < names.size(); ++i) {
    if (i != 0) {
      grammar += " | ";
    }

    // The inner string is JSON data; the outer string quotes those exact bytes as a Lark literal.
    grammar += Json(Json(names[i]).dump()).dump();
  }

  // Whitespace is explicit at structural boundaries, never ignored inside string, escape or number terminals.
  // Escaped surrogates must be paired; all other BMP escapes and raw Unicode scalar values are accepted.
  grammar += R"lark(
tool_json_call: "{" tool_json_ws "\"name\"" tool_json_colon tool_json_name tool_json_comma )lark"
             R"lark("\"parameters\"" tool_json_colon tool_json_object tool_json_ws "}"
tool_json_value: tool_json_object | tool_json_array | TOOL_JSON_STRING | TOOL_JSON_NUMBER | "true" | "false" | "null"
tool_json_object: "{" tool_json_ws (tool_json_member (tool_json_comma tool_json_member)*)? tool_json_ws "}"
tool_json_member: TOOL_JSON_STRING tool_json_colon tool_json_value
tool_json_array: "[" tool_json_ws (tool_json_value (tool_json_comma tool_json_value)*)? tool_json_ws "]"
tool_json_comma: tool_json_ws "," tool_json_ws
tool_json_colon: tool_json_ws ":" tool_json_ws
tool_json_ws: TOOL_JSON_WS?
TOOL_JSON_WS: /[ \t\r\n]+/
TOOL_JSON_STRING: /)lark"
             R"regex("(?:[^"\\\x00-\x1f]|\\(?:["\\\/bfnrt]|u(?:)regex"
             R"regex([0-9a-cA-Ce-fE-F][0-9a-fA-F]{3}|[dD][0-7][0-9a-fA-F]{2}|)regex"
             R"regex([dD][89aAbB][0-9a-fA-F]{2}\\u[dD][c-fC-F][0-9a-fA-F]{2})))*")regex"
             R"lark(/
TOOL_JSON_NUMBER: /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/
)lark";
  return grammar;
}

struct NormalizedTool {
  std::string name;
  Json parameters;
};

void from_json(const Json& value, NormalizedTool& tool) {
  if (!value.is_object() || value.at("type") != "function") {
    throw std::invalid_argument("expected normalized function tool");
  }

  const auto& function = value.contains("function") ? value.at("function") : value;
  tool.name = function.at("name").get<std::string>();
  const auto parameters = function.find("parameters");
  // Omission is the registry's parameterless declaration, not permission to replace an invalid supplied schema.
  tool.parameters = parameters == function.end()
                        ? Json{{"type", "object"}, {"additionalProperties", false}}
                        : *parameters;
}

struct Envelope {
  std::string name;
  Json parameters;
};

void from_json(const Json& value, Envelope& envelope) {
  if (!value.is_object() || value.size() != 2 || !value.contains("name") || !value.contains("parameters")) {
    throw std::invalid_argument("expected exactly 'name' and 'parameters' envelope members");
  }

  envelope.name = value.at("name").get<std::string>();
  envelope.parameters = value.at("parameters");
  if (!envelope.parameters.is_object()) {
    throw std::invalid_argument("parameters must be an object");
  }
}

}  // namespace

struct ToolCallPlan::Impl {
  std::map<std::string, Validator> validators;
  std::string grammar;
  std::optional<std::string> forced_name;
  bool parallel = true;

  void ValidateArguments(const std::string& name, const Json& arguments) const {
    const auto found = validators.find(name);
    if (found == validators.end() || (forced_name && name != *forced_name)) {
      Fail(FOUNDRY_LOCAL_ERROR_INTERNAL, "tool '" + name + "'", "unknown or disallowed tool name");
    }

    if (!arguments.is_object()) {
      Fail(FOUNDRY_LOCAL_ERROR_INTERNAL, "tool '" + name + "' instance ''", "parameters must be an object");
    }

    Validate(found->second, ValidatorJson::parse(arguments.dump()), FOUNDRY_LOCAL_ERROR_INTERNAL,
             "tool '" + name + "'");
  }
};

ToolCallPlan::ToolCallPlan(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ToolCallPlan::~ToolCallPlan() = default;

std::shared_ptr<const ToolCallPlan> ToolCallPlan::Create(
    const std::string& tools_json, bool parallel_tool_calls, const std::optional<ForcedToolChoice>& forced_tool) {
  const auto tools = ParseJson(tools_json, kMaxSchemaBytes, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tools");
  if (!tools.is_array() || tools.empty()) {
    Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tools", "expected a non-empty normalized function array");
  }

  auto impl = std::make_unique<Impl>();
  impl->parallel = parallel_tool_calls;
  if (forced_tool) {
    impl->forced_name = forced_tool->name;
  }

  std::vector<std::string> names;
  for (size_t i = 0; i < tools.size(); ++i) {
    std::string context = "tools/" + std::to_string(i) + "/function";
    try {
      const auto tool = tools[i].get<NormalizedTool>();
      context += " '" + tool.name + "'/parameters";
      ValidateToolCallText(tool.name, "tool name");
      if (tool.name.empty() || impl->validators.contains(tool.name)) {
        Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, context, "empty or duplicate tool name");
      }

      auto document = tool.parameters;
      const bool draft7 = IsDraft7(document, context);
      ValidateMetaSchema(document, draft7, context);
      CheckProfile(document, draft7, context);
      CheckReferences(document, context);
      impl->validators.emplace(tool.name, Compile(document));
      if (!forced_tool || tool.name == forced_tool->name) {
        names.push_back(tool.name);
      }
    } catch (const Exception&) {
      throw;
    } catch (const std::exception& e) {
      Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, context, e.what());
    }
  }

  if (names.empty()) {
    Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "forced tool choice", "name is not declared in tools");
  }

  impl->grammar = BuildGrammar(names, parallel_tool_calls);
  return std::shared_ptr<const ToolCallPlan>(new ToolCallPlan(std::move(impl)));
}

std::string ToolCallPlan::Grammar(std::string_view root_rule) const {
  const auto is_initial = [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; };
  const auto is_remaining = [&](char c) { return is_initial(c) || (c >= '0' && c <= '9'); };
  if (root_rule.empty() || !is_initial(root_rule.front()) || root_rule.starts_with("tool_json_") ||
      !std::ranges::all_of(root_rule, is_remaining)) {
    Fail(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "grammar root rule",
         "expected a lowercase Lark identifier outside the reserved tool_json_ prefix");
  }

  return std::string(root_rule) + impl_->grammar.substr(std::string_view("start").size());
}

std::vector<ParsedToolCall> ToolCallPlan::ParseCalls(std::string_view canonical_json) const {
  const auto batch = ParseJson(canonical_json, kMaxBatchBytes, FOUNDRY_LOCAL_ERROR_INTERNAL, "tool call batch");
  if (!batch.is_array() || batch.empty() || (!impl_->parallel && batch.size() > 1)) {
    Fail(FOUNDRY_LOCAL_ERROR_INTERNAL, "tool call batch", "expected non-empty array within parallel call limit");
  }

  std::vector<ParsedToolCall> calls;
  calls.reserve(batch.size());
  for (size_t i = 0; i < batch.size(); ++i) {
    try {
      const auto envelope = batch[i].get<Envelope>();
      ParsedToolCall call{"", envelope.name, envelope.parameters.dump()};
      call.argument_source = call.arguments;
      ValidateCall(call);
      calls.push_back(std::move(call));
    } catch (const Exception&) {
      throw;
    } catch (const std::exception& e) {
      Fail(FOUNDRY_LOCAL_ERROR_INTERNAL, "tool call batch/" + std::to_string(i), e.what());
    }
  }

  for (auto& call : calls) {
    call.id = GenerateToolCallId();
  }

  return calls;
}

void ToolCallPlan::ValidateCall(const ParsedToolCall& call) const {
  const auto context = "tool '" + call.name + "'";
  const auto& source = call.argument_source.empty() ? call.arguments : call.argument_source;
  try {
    ValidateToolCallText(call.name, "tool name");
    ValidateToolCallText(call.arguments, "tool arguments");
    ValidateToolCallText(source, "tool argument source");
  } catch (const Exception& e) {
    Fail(FOUNDRY_LOCAL_ERROR_INTERNAL, context, e.what());
  }

  const auto arguments = ParseJson(source, kMaxArgumentBytes, FOUNDRY_LOCAL_ERROR_INTERNAL, context);
  impl_->ValidateArguments(call.name, arguments);
}

}  // namespace fl
