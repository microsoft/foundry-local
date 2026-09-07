// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>
#include <utility>
#include <optional>

namespace fl {

/// The kind of inference a session performs.
enum class SessionType {
  kChat,
  kAudio,
  kPredictive,
  kEmbeddings,
};

/// How a tool's arguments are shaped, and therefore how a generated call is interpreted.
enum class ToolKind {
  /// Arguments are a JSON object conforming to the caller-supplied schema.
  kFunction,
  /// Arguments are raw text. The model is prompted with a synthesized single-string schema and the
  /// payload it produces is delivered verbatim.
  kCustom,
};

struct ToolDefinition {
  ToolDefinition() = default;

  ToolDefinition(std::string name_in, std::string description_in, std::string json_schema_in,
                 ToolKind kind_in = ToolKind::kFunction, bool include_description_in_prompt_in = true,
                 bool include_parameters_in_prompt_in = true, std::optional<bool> strict_in = std::nullopt)
      : name(std::move(name_in)),
        description(std::move(description_in)),
        json_schema(std::move(json_schema_in)),
        kind(kind_in),
        include_description_in_prompt(include_description_in_prompt_in),
        include_parameters_in_prompt(include_parameters_in_prompt_in),
        strict(strict_in) {}

  std::string name;
  std::string description;
  /// For kCustom definitions this holds the synthesized schema once the definition is registered —
  /// callers must not supply one.
  std::string json_schema;
  ToolKind kind = ToolKind::kFunction;
  bool include_description_in_prompt = true;
  bool include_parameters_in_prompt = true;
  /// Preserves an explicit supported false value for prompt serialization.
  std::optional<bool> strict;
};

}  // namespace fl
