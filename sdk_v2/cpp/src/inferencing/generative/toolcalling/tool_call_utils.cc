// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "items/tool_call_item.h"

#include <nlohmann/json.hpp>

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

bool HasAdvertisedTool(const nlohmann::json& tools, const std::string& name) {
  if (!tools.is_array()) {
    return false;
  }

  for (const auto& tool : tools) {
    if (!tool.is_object()) {
      continue;
    }
    if (tool.value("name", std::string{}) == name ||
        (tool.contains("function") && tool["function"].is_object() &&
         tool["function"].value("name", std::string{}) == name)) {
      return true;
    }
  }
  return false;
}

std::string CleanToolName(std::string name) {
  auto trim = [](std::string value) {
    const size_t start = value.find_first_not_of(" \t\r\n\"'");
    if (start == std::string::npos) {
      return std::string{};
    }
    const size_t end = value.find_last_not_of(" \t\r\n\"'");
    return value.substr(start, end - start + 1);
  };

  name = trim(std::move(name));
  if (name.starts_with("function=")) {
    name = trim(name.substr(sizeof("function=") - 1));
  }
  return name;
}

/// Try to parse a JSON string as a list of tool calls.
/// Handles both array and single-object formats:
///   [{"name": "fn", "arguments": {...}}]
///   {"name": "fn", "arguments": {...}}
std::vector<ParsedToolCall> DeserializeToolCalls(const std::string& json_text,
                                                 const nlohmann::json& advertised_tools) {
  std::vector<ParsedToolCall> results;

  try {
    const size_t content_start = json_text.find_first_not_of(" \t\r\n");
    if (content_start == std::string::npos) {
      return results;
    }
    const size_t content_end = json_text.find_last_not_of(" \t\r\n");
    const std::string normalized_text =
        json_text.substr(content_start, content_end - content_start + 1);

    auto json = nlohmann::json::parse(normalized_text, nullptr, false);
    bool repaired = false;
    bool repaired_missing_name_prefix = false;

    // Some models finish a complete object one outer brace early. Repair
    // exactly one unmatched object brace; leave other truncation untouched.
    if (json.is_discarded() && normalized_text.starts_with('{')) {
      int object_depth = 0;
      int array_depth = 0;
      bool in_string = false;
      bool escaped = false;
      for (char ch : normalized_text) {
        if (in_string) {
          if (escaped) {
            escaped = false;
          } else if (ch == '\\') {
            escaped = true;
          } else if (ch == '"') {
            in_string = false;
          }
        } else if (ch == '"') {
          in_string = true;
        } else if (ch == '{') {
          ++object_depth;
        } else if (ch == '}') {
          --object_depth;
        } else if (ch == '[') {
          ++array_depth;
        } else if (ch == ']') {
          --array_depth;
        }
      }
      if (!in_string && object_depth == 1 && array_depth == 0) {
        json = nlohmann::json::parse(normalized_text + "}", nullptr, false);
        repaired = !json.is_discarded();
      }
    }

    // Some models omit {"name":" and start with <tool_name","arguments".
    // Reconstruct the canonical prefix and still require valid JSON.
    if (json.is_discarded() && normalized_text.starts_with('<')) {
      const size_t name_end = normalized_text.find("\",\"");
      if (name_end != std::string::npos && name_end > 1) {
        const std::string tool_name = normalized_text.substr(1, name_end - 1);
        std::string repaired_text =
            R"({"name":)" + nlohmann::json(tool_name).dump() +
            normalized_text.substr(name_end + 1);
        json = nlohmann::json::parse(repaired_text, nullptr, false);

        // A wrapped arguments object closes itself but may omit the closing
        // brace for the reconstructed outer call object.
        if (json.is_discarded() &&
            (repaired_text.find(R"(,"arguments":{)") != std::string::npos ||
             repaired_text.find(R"(,"parameters":{)") != std::string::npos ||
             repaired_text.find(R"(,"args":{)") != std::string::npos)) {
          repaired_text += "}";
          json = nlohmann::json::parse(repaired_text, nullptr, false);
        }
        repaired_missing_name_prefix = !json.is_discarded();
        repaired = repaired || repaired_missing_name_prefix;
      }
    }

    // The missing-name form may place argument members directly after the
    // tool name. Preserve recognized wrappers; otherwise collect the direct
    // members into the canonical arguments object.
    if (repaired_missing_name_prefix && json.is_object() && json.contains("name") &&
        !json.contains("arguments") && !json.contains("parameters") && !json.contains("args")) {
      nlohmann::json arguments = json;
      arguments.erase("name");
      json = {{"name", json["name"]}, {"arguments", std::move(arguments)}};
      repaired = true;
    }

    // Some models emit {"tool_name","arg":value} with argument members
    // directly after the tool name. Wrap those members as arguments.
    if (json.is_discarded() && normalized_text.starts_with("{\"")) {
      const size_t name_end = normalized_text.find('"', 2);
      const size_t closing_brace = normalized_text.find_last_of('}');
      if (name_end != std::string::npos && name_end + 1 < normalized_text.size() &&
          normalized_text[name_end + 1] == ',' && closing_brace > name_end + 1) {
        const std::string tool_name = normalized_text.substr(2, name_end - 2);
        const std::string repaired_text =
            R"({"name":)" + nlohmann::json(tool_name).dump() + R"(,"arguments":{)" +
            normalized_text.substr(name_end + 2, closing_brace - name_end - 2) + "}}";
        json = nlohmann::json::parse(repaired_text, nullptr, false);
        if (!json.is_discarded()) {
          repaired = true;
        }
      }
    }

    // Some models emit {"tool_name": "arg": value} instead of wrapping the
    // arguments in an object. Repair only that narrow shape, then require the
    // result to pass the normal JSON parser.
    if (json.is_discarded()) {
      const size_t colon = normalized_text.find(':');
      const size_t closing_brace = normalized_text.find_last_of('}');
      if (colon != std::string::npos && closing_brace != std::string::npos && colon < closing_brace) {
        std::string repaired_text =
          normalized_text.substr(0, colon + 1) + "{" +
          normalized_text.substr(colon + 1, closing_brace - colon - 1) + "}" +
          normalized_text.substr(closing_brace);
        json = nlohmann::json::parse(repaired_text, nullptr, false);
        if (!json.is_discarded()) {
          repaired = true;
        }
      }
    }

    if (json.is_discarded()) {
      return results;
    }

    auto parse_one = [&](const nlohmann::json& call) -> std::optional<ParsedToolCall> {
      if (!call.is_object()) {
        return std::nullopt;
      }

      ParsedToolCall tc;
      bool call_repaired = repaired;

      if (call.contains("name") && call["name"].is_string()) {
        tc.name = call["name"].get<std::string>();
      } else if (call.contains("function") && call["function"].is_string()) {
        tc.name = call["function"].get<std::string>();
        call_repaired = true;
      } else if (call.size() == 1 && !call.contains("name") &&
             !call.contains("function") && !call.contains("arguments") &&
             !call.contains("parameters") && !call.contains("args")) {
        const auto& [name, arguments] = *call.items().begin();
        tc.name = CleanToolName(name);
        tc.arguments = arguments.is_string() ? arguments.get<std::string>() : arguments.dump();
        call_repaired = true;
      } else {
        return std::nullopt;
      }

      const std::string original_name = tc.name;
      tc.name = CleanToolName(std::move(tc.name));
      call_repaired = call_repaired || tc.name != original_name;
      if (tc.name.empty()) {
        return std::nullopt;
      }

      // Arguments can be under "arguments", "parameters", or "args".
      if (call.contains("arguments")) {
        if (call["arguments"].is_string()) {
          tc.arguments = call["arguments"].get<std::string>();
        } else {
          tc.arguments = call["arguments"].dump();
        }
      } else if (call.contains("parameters")) {
        call_repaired = true;
        if (call["parameters"].is_string()) {
          tc.arguments = call["parameters"].get<std::string>();
        } else {
          tc.arguments = call["parameters"].dump();
        }
      } else if (call.contains("args")) {
        call_repaired = true;
        if (call["args"].is_string()) {
          tc.arguments = call["args"].get<std::string>();
        } else {
          tc.arguments = call["args"].dump();
        }
      }

      if (call_repaired && !HasAdvertisedTool(advertised_tools, tc.name)) {
        return std::nullopt;
      }

      return tc;
    };

    if (json.is_array()) {
      results.reserve(json.size());
      for (const auto& item : json) {
        auto parsed = parse_one(item);
        if (!parsed) {
          return {};
        }
        results.push_back(std::move(*parsed));
      }
    } else if (json.is_object()) {
      auto parsed = parse_one(json);
      if (!parsed) {
        return {};
      }
      results.push_back(std::move(*parsed));
    } else {
      return {};
    }

    for (auto& result : results) {
      result.id = GenerateToolCallId();
    }
  } catch (const nlohmann::json::exception&) {
    return {};
  }

  return results;
}

}  // namespace

size_t FindMarkerOutsideJsonString(std::string_view text,
                                   std::string_view marker,
                                   size_t start_pos) {
  if (marker.empty() || start_pos >= text.size()) {
    return std::string_view::npos;
  }

  bool in_string = false;
  bool escaped = false;
  size_t unmatched_closing_quote = std::string_view::npos;
  const size_t payload_start = text.find_first_not_of(" \t\r\n", start_pos);
  if (payload_start != std::string_view::npos && text[payload_start] == '<') {
    const size_t first_quote = text.find('"', payload_start + 1);
    if (first_quote != std::string_view::npos && first_quote + 1 < text.size() &&
        text[first_quote + 1] == ',') {
      unmatched_closing_quote = first_quote;
    }
  }

  for (size_t pos = start_pos; pos < text.size(); ++pos) {
    if (!in_string && text.substr(pos).starts_with(marker)) {
      return pos;
    }

    const char ch = text[pos];
    if (pos == unmatched_closing_quote) {
      continue;
    }
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
    } else if (ch == '"') {
      in_string = true;
    }
  }

  return std::string_view::npos;
}

std::string GenerateToolCallId() {
  return "call_" + RandomAlphanumeric(9);
}

std::vector<ParsedToolCall> ParseToolCalls(const std::string& text,
                                           const std::string& tool_call_start,
                                           const std::string& tool_call_end,
                                           const std::string& tools_json) {
  std::vector<ParsedToolCall> all_calls;

  const auto advertised_tools = nlohmann::json::parse(tools_json, nullptr, false);

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
    size_t end_pos = FindMarkerOutsideJsonString(text, tool_call_end, content_start);
    if (end_pos == std::string::npos) {
      break;
    }

    std::string content = text.substr(content_start, end_pos - content_start);
    auto calls = DeserializeToolCalls(content, advertised_tools);

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
