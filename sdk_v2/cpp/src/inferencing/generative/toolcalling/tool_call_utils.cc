// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "items/tool_call_item.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <random>

namespace fl {

namespace {

/// Generate a random alphanumeric string of the given length.
std::string RandomAlphanumeric(int length) {
  static constexpr char kChars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, sizeof(kChars) - 2);

  std::string result;
  result.reserve(length);

  for (int i = 0; i < length; ++i) {
    result += kChars[dist(rng)];
  }

  return result;
}

std::optional<ParsedToolCall> ParseOneToolCall(const nlohmann::json& call) {
  if (!call.is_object()) {
    return std::nullopt;
  }

  auto name_it = call.find("name");
  if (name_it == call.end() || !name_it->is_string() || name_it->get<std::string>().empty()) {
    return std::nullopt;
  }

  ParsedToolCall tc;
  tc.name = name_it->get<std::string>();

  if (auto args_it = call.find("arguments"); args_it != call.end()) {
    tc.arguments = args_it->is_string() ? args_it->get<std::string>() : args_it->dump();
  } else if (auto params_it = call.find("parameters"); params_it != call.end()) {
    tc.arguments = params_it->is_string() ? params_it->get<std::string>() : params_it->dump();
  }

  return tc;
}

/// Try to parse a JSON string as a list of tool calls.
/// Handles both array and single-object formats:
///   [{"name": "fn", "arguments": {...}}]
///   {"name": "fn", "arguments": {...}}
std::vector<ParsedToolCall> DeserializeToolCalls(const std::string& json_text) {
  try {
    auto json = nlohmann::json::parse(json_text);
    std::vector<ParsedToolCall> results;

    if (json.is_array()) {
      results.reserve(json.size());

      for (const auto& item : json) {
        auto parsed = ParseOneToolCall(item);
        if (!parsed) {
          return {};
        }

        results.push_back(std::move(*parsed));
      }
    } else if (json.is_object()) {
      auto parsed = ParseOneToolCall(json);
      if (!parsed) {
        return {};
      }

      results.push_back(std::move(*parsed));
    }

    for (auto& result : results) {
      result.id = GenerateToolCallId();
    }

    return results;
  } catch (const nlohmann::json::exception&) {
    return {};
  }
}

}  // namespace

std::string GenerateToolCallId() {
  return "call_" + RandomAlphanumeric(9);
}

std::vector<ParsedToolCall> ParseToolCalls(const std::string& text,
                                           const std::string& tool_call_start,
                                           const std::string& tool_call_end) {
  std::vector<ParsedToolCall> all_calls;

  if (tool_call_start.empty() || tool_call_end.empty()) {
    return all_calls;
  }

  // Find all occurrences of tool_call_start ... tool_call_end in the text
  size_t search_pos = 0;

  while (search_pos < text.size()) {
    size_t start_pos = text.find(tool_call_start, search_pos);
    if (start_pos == std::string::npos) {
      break;
    }

    size_t content_start = start_pos + tool_call_start.size();
    size_t end_pos = text.find(tool_call_end, content_start);
    if (end_pos == std::string::npos) {
      break;
    }

    std::string content = text.substr(content_start, end_pos - content_start);
    auto calls = DeserializeToolCalls(content);

    for (auto& call : calls) {
      all_calls.push_back(std::move(call));
    }

    search_pos = end_pos + tool_call_end.size();
  }

  return all_calls;
}

std::vector<std::unique_ptr<Item>> ToolCallsToItems(const std::vector<ParsedToolCall>& calls) {
  std::vector<std::unique_ptr<Item>> items;
  items.reserve(calls.size());

  for (const auto& call : calls) {
    items.push_back(std::make_unique<ToolCallItem>(call.id, call.name, call.arguments));
  }

  return items;
}

}  // namespace fl
