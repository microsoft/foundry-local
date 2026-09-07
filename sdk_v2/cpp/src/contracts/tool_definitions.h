// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/types.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {
namespace tools {

// ========================================================================
// Wire tool declarations → core tool definitions.
//
// Chat Completions and Responses spell a tool differently — Chat nests the declaration under
// "function" / "custom", Responses inlines it next to "type" — but both describe the same two
// kinds of tool, and the runtime has exactly one representation of them: fl::ToolDefinition,
// carrying a name, a description, a schema and a ToolKind. Every HTTP surface converts to that
// representation here and registers it on the session; the session's registry is the authority on
// what kind each name is.
//
// Nothing downstream of these helpers knows which HTTP surface a tool arrived on. The separate
// legacy C ABI path may still supply an unnamed pre-serialized tools array.
// ========================================================================

/// Parse and validate a text custom-tool `format`.
///
/// A custom tool takes free-form text: the model is prompted with the synthesized single-string
/// schema and whatever it produces comes back verbatim. Grammar-constrained tools require raw
/// envelope handling that this transport does not implement, so they are rejected.
///
/// Accepted: a null value (normalized to text) and `{"type":"text"}`.
///
/// @throws fl::Exception (FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) for a non-object format, a missing
///         or non-string `type`, grammar/unknown formats, or unexpected members.
nlohmann::json ParseCustomToolFormat(const nlohmann::json& format, const std::string& tool_name);

/// Parse a function tool's `strict` value.
///
/// Omitted values are handled by the caller. Null is treated as unspecified, false is preserved,
/// and true is rejected until the runtime can enforce it with constrained decoding.
std::optional<bool> ParseFunctionStrict(const nlohmann::json& strict);

/// Core definition for a function tool declared on the wire.
///
/// An empty `json_schema` becomes `{}`: the registry requires every function tool to carry a
/// schema, and `{}` is how "declares no parameters" is spelled in JSON Schema. A schema the caller
/// did supply is passed through unchanged, so what the model is prompted with stays JSON-equivalent
/// to what was requested. `strict` is likewise retained only to preserve the prior prompt JSON
/// behavior for the supported false value.
ToolDefinition MakeFunctionTool(std::string name, std::string description, std::string json_schema,
                                bool description_present = true, bool parameters_present = true,
                                std::optional<bool> strict = std::nullopt);

/// Core definition for a text custom tool declared on the wire.
///
/// The schema is left empty on purpose: the registry synthesizes the single required string `input`
/// schema, and a caller-supplied schema is rejected. This is what keeps raw custom input out of
/// function-argument handling — a custom tool never carries a function schema anywhere.
ToolDefinition MakeCustomTool(std::string name, std::string description, bool description_present = true);

/// Name-to-kind index over one immutable snapshot of tool definitions.
///
/// A turn takes the snapshot once and derives this index from it, so the kinds used to normalize a replayed call and
/// to read a produced one are the same kinds that shaped the prompt — the registry stays mutable from other threads,
/// and re-reading it mid-turn could resolve a turn's own output against a tool set that never prompted it.
std::unordered_map<std::string, ToolKind> KindsByName(const std::vector<ToolDefinition>& definitions);

/// Narrow a declared tool set to the single tool a forced `tool_choice` names, matching on both
/// name and kind so a function and a custom tool sharing a name can never be swapped for each
/// other.
///
/// A forced tool that was not declared with the requested kind is rejected.
void NarrowToForcedTool(std::vector<ToolDefinition>& definitions, const std::string& name, ToolKind kind);

/// Keep only the definitions whose name appears in the backward-compatible top-level
/// `allowed_tools` extension, compared case-insensitively to preserve its existing behavior.
///
/// Order is preserved, and `allowed_names` is a filter rather than a selection: repeated entries
/// keep a tool once, and entries naming a tool that was never declared match nothing.
void RetainAllowedTools(std::vector<ToolDefinition>& definitions, const std::vector<std::string>& allowed_names);

}  // namespace tools
}  // namespace fl
