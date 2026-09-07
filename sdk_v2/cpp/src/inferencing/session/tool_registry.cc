// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/tool_registry.h"

#include "exception.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace fl {

namespace {

/// First flToolDefinition version that carries `kind`. Below this, the caller allocated a struct
/// that ends at `json_schema` and nothing past that offset may be read.
constexpr uint32_t kToolDefinitionKindVersion = 2;

// The version gate above is only sound while `kind` sits immediately after the version 1 prefix
// and has a fixed width. Both are ABI promises, so pin them here.
static_assert(offsetof(flToolDefinition, kind) == offsetof(flToolDefinition, json_schema) + sizeof(const char*),
              "flToolDefinition::kind must be appended directly after the version 1 prefix");
static_assert(sizeof(flToolKind) == sizeof(uint32_t), "flToolKind must be a fixed-width 32-bit ABI type");

}  // namespace

ToolRegistry::ToolRegistry(ToolRegistry&& other) {
  std::lock_guard<std::mutex> lock(*other.mutex_);
  definitions_ = std::move(other.definitions_);
}

std::string ExtractCustomToolInput(const std::string& arguments) {
  size_t top_level_member_count = 0;
  auto count_top_level_members = [&top_level_member_count](int depth, nlohmann::json::parse_event_t event,
                                                           nlohmann::json&) {
    if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
      ++top_level_member_count;
    }

    return true;
  };
  auto parsed =
      nlohmann::json::parse(arguments, count_top_level_members, /*allow_exceptions=*/false);

  // Only the exact shape the synthesized schema asks for is unwrapped: a lone `input` member
  // holding a string. Anything else means the model did not produce what it was asked for, and the
  // raw bytes are the only faithful record of what it did produce. Count source members separately
  // because the parsed object collapses duplicate keys.
  if (!parsed.is_object() || top_level_member_count != 1 || parsed.size() != 1) {
    return arguments;
  }

  auto it = parsed.find(kCustomToolInputParameter);
  if (it == parsed.end() || !it->is_string()) {
    return arguments;
  }

  // get<std::string>() resolves JSON escapes, so the payload comes back exactly as the model meant
  // it: line endings, tabs, trailing spaces and Unicode all intact.
  return it->get<std::string>();
}

void ToolRegistry::Add(ToolDefinition tool_def) {
  // Validate and normalize before taking the lock — none of it touches shared state.
  if (tool_def.kind == ToolKind::kCustom) {
    if (tool_def.name.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "a custom tool definition requires a name");
    }

    if (!tool_def.json_schema.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "custom tool '", tool_def.name,
               "' must not supply a json_schema; the schema is synthesized");
    }

    tool_def.json_schema = kCustomToolInputSchema;
  } else if (!nlohmann::json::accept(tool_def.json_schema)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "ToolDefinition.json_schema is not valid JSON for tool: " + tool_def.name);
  }

  std::lock_guard<std::mutex> lock(*mutex_);

  // Unnamed entries are pre-serialized tools payloads rather than registrable tools, so uniqueness
  // does not apply to them. A session holds a handful of tools, so a scan beats a second index.
  if (!tool_def.name.empty()) {
    const auto existing = std::find_if(definitions_.begin(), definitions_.end(),
                                       [&](const ToolDefinition& td) { return td.name == tool_def.name; });
    if (existing != definitions_.end()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "a tool named '", tool_def.name,
               "' is already registered; remove it before registering it again");
    }
  }

  definitions_.push_back(std::move(tool_def));
}

bool ToolRegistry::Remove(const std::string& name) {
  // An empty name is not a registered name. Matching it positionally would silently drop the
  // unnamed pre-serialized entry a request had just installed.
  if (name.empty()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(*mutex_);

  const auto it = std::find_if(definitions_.begin(), definitions_.end(),
                               [&](const ToolDefinition& td) { return td.name == name; });
  if (it == definitions_.end()) {
    return false;
  }

  definitions_.erase(it);
  return true;
}

void ToolRegistry::Clear() {
  std::lock_guard<std::mutex> lock(*mutex_);
  definitions_.clear();
}

std::vector<ToolDefinition> ToolRegistry::Definitions() const {
  std::lock_guard<std::mutex> lock(*mutex_);
  return definitions_;
}

ToolDefinition ToolDefinitionFromC(const flToolDefinition& tool_def) {
  // Gate on the version before touching anything past the version 1 prefix — a caller built
  // against an older header allocated a struct that ends at `json_schema`.
  if (tool_def.version == 0 || tool_def.version > FOUNDRY_LOCAL_API_VERSION) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported flToolDefinition.version: ", tool_def.version,
             " (supported: 1 to ", static_cast<uint32_t>(FOUNDRY_LOCAL_API_VERSION), ")");
  }

  if (!tool_def.name || !tool_def.description) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "flToolDefinition.name and .description must not be null");
  }

  ToolKind kind = ToolKind::kFunction;

  if (tool_def.version >= kToolDefinitionKindVersion) {
    switch (tool_def.kind) {
      case FOUNDRY_LOCAL_TOOL_KIND_FUNCTION:
        kind = ToolKind::kFunction;
        break;
      case FOUNDRY_LOCAL_TOOL_KIND_CUSTOM:
        kind = ToolKind::kCustom;
        break;
      default:
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unknown flToolDefinition.kind: ", tool_def.kind);
    }
  }

  // A function tool's schema is required; a custom tool's must be absent, and null is how a caller
  // spells "absent".
  if (kind == ToolKind::kFunction && !tool_def.json_schema) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "flToolDefinition.json_schema must not be null for a function tool");
  }

  ToolDefinition definition;
  definition.name = tool_def.name;
  definition.description = tool_def.description;
  definition.json_schema = tool_def.json_schema ? tool_def.json_schema : "";
  definition.kind = kind;

  if (definition.kind == ToolKind::kFunction && definition.name.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "a public function tool definition requires a name");
  }

  return definition;
}

}  // namespace fl
