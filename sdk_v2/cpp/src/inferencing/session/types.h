// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>

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
  std::string name;
  std::string description;
  /// For kCustom definitions this holds the synthesized schema once the definition is registered —
  /// callers must not supply one.
  std::string json_schema;
  ToolKind kind = ToolKind::kFunction;
};

}  // namespace fl
