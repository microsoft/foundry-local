// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "inferencing/session/types.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

/// Immutable, offline authority for nonempty canonical [{"name": ..., "parameters": {...}}] tool-call batches.
/// Supports Draft 7 and 2020-12 (default) with local JSON Pointer references. Resource IDs, anchors,
/// recursive references, custom vocabularies, reserved controls and unsupported content features are rejected.
/// Unknown non-reserved keywords and format are annotations; original schemas are preserved without format assertions.
/// Draft 7 tuple items/dependencies are not supported. No defaults, coercion or repair are performed.
/// Omitted function.parameters declares a parameterless tool accepting only {}; supplied schemas are never defaulted.
/// Grammar is a structural mask, NOT a semantic schema mask. It constrains JSON transport, envelopes, names and count.
/// ParseCalls/ValidateCall enforce the original schemas before ANY call publication, without schema projection.
class ToolCallPlan final {
 public:
  // Four MiB accommodates source-code/custom-tool arguments; the batch cap bounds aggregate parsing memory.
  static constexpr std::size_t kMaxSchemaBytes = 1024 * 1024;
  static constexpr std::size_t kMaxArgumentBytes = 4 * 1024 * 1024;
  static constexpr std::size_t kMaxBatchBytes = 16 * 1024 * 1024;
  static constexpr int kMaxDepth = 64;

  static std::shared_ptr<const ToolCallPlan> Create(
      const std::string& tools_json, bool parallel_tool_calls = true,
      const std::optional<ForcedToolChoice>& forced_tool = std::nullopt);

  ~ToolCallPlan();
  ToolCallPlan(const ToolCallPlan&) = delete;
  ToolCallPlan& operator=(const ToolCallPlan&) = delete;

  /// Structural Lark body grammar with explicit ASCII JSON whitespace outside strings and numeric terminals.
  /// Internal rules use tool_json_/TOOL_JSON_ prefixes for embedding.
  /// root_rule must be a lowercase Lark rule identifier outside the reserved tool_json_ prefix.
  std::string Grammar(std::string_view root_rule = "start") const;
  std::vector<ParsedToolCall> ParseCalls(std::string_view canonical_json) const;
  void ValidateCall(const ParsedToolCall& call) const;

 private:
  struct Impl;
  explicit ToolCallPlan(std::unique_ptr<Impl> impl);
  const std::unique_ptr<Impl> impl_;
};

}  // namespace fl
