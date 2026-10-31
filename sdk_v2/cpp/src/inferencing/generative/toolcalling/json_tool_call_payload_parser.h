// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "exception.h"
#include "inferencing/generative/toolcalling/tool_call_payload_parser.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"

#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace fl {

inline bool HasFoundryToolCallsFirstKey(std::string_view body) {
  const auto skip_whitespace = [&](size_t& offset) {
    while (offset < body.size() &&
           std::string_view(" \t\r\n").find(body[offset]) != std::string_view::npos) {
      ++offset;
    }
  };

  size_t offset = 0;
  skip_whitespace(offset);
  if (offset == body.size() || body[offset++] != '{') {
    return false;
  }
  skip_whitespace(offset);
  if (offset == body.size() || body[offset] != '"') {
    return false;
  }

  const auto key_start = offset++;
  bool escaped = false;
  while (offset < body.size()) {
    const auto byte = body[offset++];
    if (escaped) {
      escaped = false;
    } else if (byte == '\\') {
      escaped = true;
    } else if (byte == '"') {
      break;
    }
  }
  if (offset > body.size() || body[offset - 1] != '"') {
    return false;
  }

  const auto key = nlohmann::json::parse(body.substr(key_start, offset - key_start), nullptr, false);
  return key.is_string() && key.get_ref<const std::string&>() == "foundry_tool_calls";
}

inline bool JsonNestingWithinToolCallLimit(std::string_view source) {
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (const auto byte : source) {
    if (quoted) {
      if (escaped) {
        escaped = false;
      } else if (byte == '\\') {
        escaped = true;
      } else if (byte == '"') {
        quoted = false;
      }
    } else if (byte == '"') {
      quoted = true;
    } else if (byte == '{' || byte == '[') {
      if (++depth > ToolCallPlan::kMaxDepth) {
        return false;
      }
    } else if ((byte == '}' || byte == ']') && depth > 0) {
      --depth;
    }
  }
  return true;
}

inline ToolCallPayloadParser CreateJsonToolCallPayloadParser(std::shared_ptr<const ToolCallPlan> plan,
                                                             std::string start_marker,
                                                             std::string end_marker,
                                                             bool parallel_tool_calls,
                                                             bool require_foundry_wrapper = false,
                                                             bool reject_non_wrapper_as_text = false) {
  return [plan = std::move(plan), start = std::move(start_marker), end = std::move(end_marker),
          parallel_tool_calls, require_foundry_wrapper,
          reject_non_wrapper_as_text](std::string_view source, bool end_of_stream) {
    ToolCallPayloadParseResult result;
    auto reject = [&](std::string error) {
      result.disposition = ToolCallPayloadDisposition::kMalformed;
      result.error = std::move(error);
      result.calls.clear();
      return result;
    };

    size_t offset = 0;
    while (true) {
      if (!source.substr(offset).starts_with(start)) {
        return reject("Expected a tool-call opening marker");
      }

      const auto body_start = offset + start.size();
      const auto body_end = FindMarkerOutsideJsonString(source, end, body_start);
      if (body_end == std::string_view::npos) {
        if (!end_of_stream) {
          return result;
        }
        if (require_foundry_wrapper && reject_non_wrapper_as_text &&
            !HasFoundryToolCallsFirstKey(source.substr(body_start))) {
          result.disposition = ToolCallPayloadDisposition::kRejected;
          result.consumed_size = source.size();
          return result;
        }
        return reject("Incomplete tool-call JSON or missing closing marker");
      }

      const auto body = source.substr(body_start, body_end - body_start);
      if (require_foundry_wrapper) {
        const auto reject_as_text = [&] {
          result.disposition = ToolCallPayloadDisposition::kRejected;
          result.consumed_size = body_end + end.size();
          return result;
        };
        if (!JsonNestingWithinToolCallLimit(body)) {
          return reject_non_wrapper_as_text
                     ? reject_as_text()
                     : reject("Foundry tool-call envelope exceeds the JSON nesting limit");
        }
        try {
          const auto envelope = nlohmann::json::parse(body);
          if (!envelope.is_object() || envelope.size() != 1 ||
              !envelope.contains("foundry_tool_calls")) {
            return reject_as_text();
          }
        } catch (const nlohmann::json::exception&) {
          if (reject_non_wrapper_as_text) {
            return reject_as_text();
          }
          return reject("Malformed Foundry tool-call envelope");
        }
      }

      try {
        auto calls = plan->ParseCalls(body);
        result.calls.insert(result.calls.end(), std::make_move_iterator(calls.begin()),
                            std::make_move_iterator(calls.end()));
      } catch (const Exception& error) {
        if (require_foundry_wrapper && reject_non_wrapper_as_text) {
          result.disposition = ToolCallPayloadDisposition::kRejected;
          result.consumed_size = body_end + end.size();
          return result;
        }
        return reject(error.what());
      }

      if (!parallel_tool_calls && result.calls.size() > 1) {
        return reject("parallel_tool_calls=false permits exactly one tool call per turn");
      }

      offset = body_end + end.size();
      while (offset < source.size() && std::string_view(" \t\r\n").find(source[offset]) != std::string_view::npos) {
        ++offset;
      }

      const auto tail = source.substr(offset);
      if (tail.empty()) {
        if (!end_of_stream) {
          result.calls.clear();
          return result;
        }
      } else if (require_foundry_wrapper) {
        result.calls.clear();
        result.disposition = reject_non_wrapper_as_text
                                 ? ToolCallPayloadDisposition::kRejected
                                 : ToolCallPayloadDisposition::kMalformed;
        result.consumed_size = reject_non_wrapper_as_text ? source.size() : 0;
        result.error = "Foundry tool-call envelope must be the entire response";
        return result;
      } else if (tail.starts_with(start)) {
        continue;
      } else if (!end_of_stream && start.starts_with(tail)) {
        result.calls.clear();
        return result;
      }

      result.disposition = ToolCallPayloadDisposition::kParsed;
      result.consumed_size = offset;
      return result;
    }
  };
}

}  // namespace fl
