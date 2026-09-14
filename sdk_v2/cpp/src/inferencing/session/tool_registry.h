// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <foundry_local/foundry_local_c.h>

#include "inferencing/session/types.h"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// ========================================================================
// Tool registry
//
// The authoritative record of which tools a session exposes and what kind each one is. Names are
// compared exactly (case-sensitive) and are unique across kinds, so a generated call's name always
// resolves to exactly one definition. Remove a definition to free its name for re-registration.
//
// A custom tool takes a single free-form text payload rather than a JSON argument object. Models
// and chat templates only understand function-shaped tools, so a custom definition is normalized on
// registration into a function-shaped schema with exactly one required string parameter (`input`),
// and the payload the model produces is unwrapped back to raw text on the way out.
//
// The registry is internally synchronized and hands out snapshots by value: a caller may register
// or remove tools from another thread while a request generates. Generation never consults the
// registry — it resolves kinds through the immutable snapshot taken into the request's
// ToolCallContext.
// ========================================================================

/// The single synthesized parameter a custom tool exposes to the model.
inline constexpr const char* kCustomToolInputParameter = "input";

/// The schema synthesized for every custom tool. Callers may not supply their own. Kept as a
/// literal so the exact bytes handed to the prompt are visible here and pinned by tests.
inline constexpr const char* kCustomToolInputSchema =
    R"({"type":"object","properties":{"input":{"type":"string"}},"required":["input"],"additionalProperties":false})";

/// Recover a custom tool's raw payload from the arguments a model generated.
///
/// Unwraps exactly one shape: a JSON object whose only member is `input` with a string value — the
/// shape the synthesized schema asks for. The result is that member's string value with JSON
/// escapes resolved, so line endings, tabs, trailing spaces and Unicode survive byte for byte.
/// Everything else is returned verbatim, because anything else is a model defect and the raw bytes
/// are the only faithful record of it.
std::string ExtractCustomToolInput(const std::string& arguments);

/// Validate text crossing a NUL-terminated tool-call ABI boundary.
/// @throws fl::Exception (FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) for embedded NUL or invalid UTF-8.
void ValidateToolCallText(std::string_view text, std::string_view field);

/// Ordered set of the tool definitions registered on a session. Thread-safe.
class ToolRegistry {
 public:
  ToolRegistry() = default;

  // The mutex protects this registry object rather than the vector it happens to contain, so moving a registry moves
  // the definitions under the source lock while each object keeps its own usable mutex.
  ToolRegistry(ToolRegistry&& other);
  ToolRegistry& operator=(ToolRegistry&&) = delete;

  /// Register a definition, validating and normalizing it. An unnamed definition carries a
  /// pre-serialized tools payload assembled elsewhere: it is not registrable by name, so uniqueness
  /// does not apply to it and no generated call resolves against it.
  /// @throws fl::Exception (FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) if the name is already registered,
  ///         a function definition's schema is not valid JSON text, or a custom definition supplies
  ///         a schema or has no name. Empty function names remain accepted for the internal
  ///         pre-serialized path.
  void Add(ToolDefinition tool_def);

  /// Remove the definition registered under this exact name. Returns whether one was removed.
  /// An empty name removes nothing: unnamed entries are not registered by name (see Add).
  bool Remove(const std::string& name);

  /// Drop every definition. Needed when a session is reused across requests and the new request
  /// brings its own tools.
  void Clear();

  /// Validate, normalize, and atomically replace every definition.
  void Replace(std::vector<ToolDefinition> tool_definitions);

  /// Snapshot of the registered definitions, in registration order.
  std::vector<ToolDefinition> Definitions() const;

 private:
  std::vector<ToolDefinition> definitions_;

  mutable std::mutex mutex_;
};

/// Translate a caller-supplied flToolDefinition into the internal representation, reading only the
/// fields the stamped `version` guarantees to exist.
///
/// This lives beside the registry (rather than in the C API glue) so the version and kind gate is
/// exercisable by tests that hand it a struct allocated at an older, smaller size.
///
/// @throws fl::Exception (FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) for version 0, versions newer than
///         FOUNDRY_LOCAL_API_VERSION, unknown kinds, and null strings the kind requires.
ToolDefinition ToolDefinitionFromC(const flToolDefinition& tool_def);

}  // namespace fl
