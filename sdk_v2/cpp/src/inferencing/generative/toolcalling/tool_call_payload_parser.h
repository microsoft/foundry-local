// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_utils.h"

#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

namespace fl {

enum class ToolCallPayloadDisposition {
  kNeedMore,
  kParsed,
  kRejected,
  /// A structurally tool-shaped Engine auto-mode candidate that must not become visible text.
  /// This is only a retry trigger; it does not mean the payload is valid or repairable.
  kMalformed,
};

struct ToolCallPayloadParseResult {
  ToolCallPayloadDisposition disposition = ToolCallPayloadDisposition::kNeedMore;
  size_t consumed_size = 0;
  std::vector<ParsedToolCall> calls;
};

/// Parses a candidate beginning with the configured tool-call start marker.
///
/// A parser must not return kParsed until the complete atomic batch is known. kRejected preserves the consumed
/// source bytes as visible text. end_of_stream allows a parser to resolve an otherwise ambiguous complete prefix.
using ToolCallPayloadParser =
    std::function<ToolCallPayloadParseResult(std::string_view source, bool end_of_stream)>;

}  // namespace fl
