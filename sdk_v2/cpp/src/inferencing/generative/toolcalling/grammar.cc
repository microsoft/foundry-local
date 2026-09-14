// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/grammar.h"
#include "exception.h"

#include <nlohmann/json.hpp>

#include <sstream>

namespace fl {

std::string EscapeLarkLiteral(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');

  for (const char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (const auto byte = static_cast<unsigned char>(c); byte < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[byte >> 4]);
          out.push_back(kHex[byte & 0x0f]);
        } else {
          out.push_back(c);
        }
        break;
    }
  }

  out.push_back('"');
  return out;
}

std::string RenderLarkMarker(const std::string& marker_text, std::optional<int32_t> token_id) {
  if (token_id.has_value() && *token_id >= 0) {
    return "<[" + std::to_string(*token_id) + "]>";
  }

  return EscapeLarkLiteral(marker_text);
}

std::string BuildToolJsonSchema(const ToolCallContext& ctx) {
  // Create a JSON schema from tools for use with ORT GenAI's SetGuidance.
  //
  // Output format:
  //
  // 1. Return JSON-formatted output for tool call
  //
  // {
  //   "type" : "array",
  //   "items" : {
  //     "anyOf" : [
  //       { <schemas for each tool> }
  //     ]
  //   }
  // }
  //
  // 2. Return JSON-formatted output for non tool call
  //
  // {}
  //
  // Example grammar schema for a tool:
  // [
  //     {
  //         "description": "How to get the weather for a city",
  //         "type": "object",
  //         "properties": {
  //             "name": {"const": "get_weather"},
  //             "parameters": {
  //                 "type": "object",
  //                 "properties": {
  //                     "location": {"type": "string"}
  //                 },
  //                 "required": ["location"],
  //                 "additionalProperties": false
  //             }
  //         },
  //         "required": ["name", "parameters"],
  //         "additionalProperties": false
  //     }
  // ]

  if (!ctx.tool_output || ctx.tools_json.empty()) {
    return "{}";
  }

  // Parse the tools JSON to extract function schemas
  nlohmann::json tools;
  try {
    tools = nlohmann::json::parse(ctx.tools_json);
  } catch (const nlohmann::json::parse_error&) {
    return "{}";
  }

  if (!tools.is_array() || tools.empty()) {
    return "{}";
  }

  // Build anyOf schemas — one entry per tool
  nlohmann::json schemas = nlohmann::json::array();

  for (const auto& tool : tools) {
    // Support both OpenAI-function style and direct-name style for tool definitions.
    //
    // OpenAI-function style:
    // {
    //     "type": "function",
    //     "function": {
    //         "name": "get_weather",
    //         "description": "How to get the weather for a city",
    //         "parameters": {
    //             "type": "object",
    //             "properties": {
    //                 "location": {"type": "string"}
    //             },
    //             "required": ["location"]
    //         }
    //     }
    // }
    //
    // Direct-name style (OpenAI-tool style):
    // {
    //     "type": "tool",
    //     "name": "get_weather",
    //     "description": "How to get the weather for a city",
    //     "parameters": {
    //         "type": "object",
    //         "properties": {
    //             "location": {"type": "string"}
    //         },
    //         "required": ["location"]
    //     }
    // }
    std::string name;
    std::string description;
    nlohmann::json parameters;

    if (tool.contains("function") && tool["function"].is_object()) {
      const auto& fn = tool["function"];
      name = fn.value("name", "");
      description = fn.value("description", "");

      if (fn.contains("parameters") && fn["parameters"].is_object()) {
        parameters = fn["parameters"];
      }
    } else {
      name = tool.value("name", "");
      description = tool.value("description", "");

      if (tool.contains("parameters") && tool["parameters"].is_object()) {
        parameters = tool["parameters"];
      }
    }

    if (name.empty()) {
      continue;
    }

    // Build the grammar schema for this tool
    // Create `properties` object for tool
    nlohmann::json properties;
    properties["name"] = {{"const", name}};

    std::vector<std::string> required_fields = {"name"};

    // Only add `parameters` to `properties` object if it exists in the original tool
    // and if type has been set (since type is required if providing parameters)
    bool has_params = parameters.is_object() &&
                      parameters.contains("type") &&
                      !parameters["type"].get<std::string>().empty();

    if (has_params) {
      nlohmann::json param_schema;
      param_schema["type"] = parameters.value("type", "object");

      if (parameters.contains("properties")) {
        param_schema["properties"] = parameters["properties"];
      }

      if (parameters.contains("required")) {
        param_schema["required"] = parameters["required"];
      }

      param_schema["additionalProperties"] = false;
      properties["parameters"] = param_schema;
      required_fields.push_back("parameters");
    }

    // Create `schema` for tool
    nlohmann::json schema = {
        {"description", description},
        {"type", "object"},
        {"properties", properties},
        {"required", required_fields},
        {"additionalProperties", false},
    };

    schemas.push_back(std::move(schema));
  }

  if (schemas.empty()) {
    return "{}";
  }

  // Construct grammar for guidance
  nlohmann::json grammar = {
      {"x-guidance", {{"whitespace_flexible", false}, {"key_separator", ": "}, {"item_separator", ", "}}},
      {"type", "array"},
      {"items", {{"anyOf", schemas}}},
      {"minItems", ctx.tool_output ? 1 : 0},
  };

  return grammar.dump();
}

std::string BuildLarkGrammar(const ToolCallContext& ctx,
                             const std::string& json_schema,
                             bool prompt_opens_reasoning) {
  // Legend:
  //
  // 1. cot = chain-of-thought output with newline at the end
  // 2. THINK_TEXT = chain-of-thought text output
  // 3. output = output row (text and/or tool call)
  // 4. TEXT = text output
  // 5. toolcall = tool call output (with configured boundary markers)
  // 6. functioncall = JSON schemas for each registered tool
  //
  // Cases:
  //
  // | Case | Description                                                                                        |
  // |------|----------------------------------------------------------------------------------------------------|
  // |  1   | Return text only                                                                                   |
  // |  2   | Return tool call only (configured tool-call markers)                                               |
  // |  3   | Return tool call only (no configured tool-call markers)                                            |
  // |  4   | Return text or tool call (configured tool-call markers)                                            |
  // |  5   | Return text or tool call (no configured tool-call markers)                                         |
  // |  6   | Return chain-of-thought + text only (configured reasoning markers)                                 |
  // |  7   | Return chain-of-thought + text only (no configured reasoning markers)                              |
  // |  8   | Return chain-of-thought + tool call only (both marker pairs configured)                            |
  // |  9   | Return chain-of-thought + tool call only (only tool-call markers configured)                       |
  // |  10  | Return chain-of-thought + tool call only (only reasoning markers configured)                       |
  // |  11  | Return chain-of-thought + tool call only (no marker pairs configured)                              |
  // |  12  | Return chain-of-thought + text or tool call (both marker pairs configured)                         |
  // |  13  | Return chain-of-thought + text or tool call (only tool-call markers configured)                    |
  // |  14  | Return chain-of-thought + text or tool call (only reasoning markers configured)                    |
  // |  15  | Return chain-of-thought + text or tool call (no marker pairs configured)                           |
  //
  // Grammar patterns for each case:
  //
  // 1. Return text only
  //
  // start: TEXT
  // TEXT: /[^{<](.|\\n)*/
  //
  // 2. Return tool call only (configured tool-call markers)
  //
  // start: toolcall
  // toolcall: <[123]> functioncall <[124]>
  // functioncall: %json { <schemas for each tool> }
  //
  // 3. Return tool call only (no configured tool-call markers)
  //
  // start: functioncall
  // functioncall: %json { <schemas for each tool> }
  //
  // 4. Return text or tool call (configured tool-call markers)
  //
  // start: TEXT | toolcall
  // TEXT: /[^{<](.|\\n)*/
  // toolcall: "<tool_call>" functioncall "</tool_call>"
  // functioncall: %json { <schemas for each tool> }
  //
  // 5. Return text or tool call (no configured tool-call markers)
  //
  // start: TEXT | functioncall
  // TEXT: /[^{<](.|\\n)*/
  // functioncall: %json { <schemas for each tool> }
  //
  // 6. Return chain-of-thought + text only (configured reasoning markers)
  //
  // start: cot TEXT
  // cot: <[125]> THINK_TEXT <[126]> "\\n"
  // THINK_TEXT: /[^<]+/
  // TEXT: /[^{<](.|\\n)*/
  //
  // 7. Return chain-of-thought + text only (no configured reasoning markers)
  //
  // start: cot TEXT
  // cot: "<think>" THINK_TEXT "</think>" "\\n"
  // THINK_TEXT: /[^<]+/
  // TEXT: /[^{<](.|\\n)*/
  //
  // 8. Return chain-of-thought + tool call only (both marker pairs configured)
  //
  // start: cot toolcall
  // cot: <[125]> THINK_TEXT <[126]> "\\n"
  // THINK_TEXT: /[^<]+/
  // toolcall: <[123]> functioncall <[124]>
  // functioncall: %json { <schemas for each tool> }
  //
  // 9. Return chain-of-thought + tool call only (only tool-call markers configured)
  //
  // start: cot toolcall
  // cot: "<think>" THINK_TEXT "</think>" "\\n"
  // THINK_TEXT: /[^<]+/
  // toolcall: "<tool_call>" functioncall "</tool_call>"
  // functioncall: %json { <schemas for each tool> }
  //
  // 10. Return chain-of-thought + tool call only (only reasoning markers configured)
  //
  // start: cot functioncall
  // cot: <[125]> THINK_TEXT <[126]> "\\n"
  // THINK_TEXT: /[^<]+/
  // functioncall: %json { <schemas for each tool> }
  //
  // 11. Return chain-of-thought + tool call only (no marker pairs configured)
  //
  // start: cot functioncall
  // cot: "<think>" THINK_TEXT "</think>" "\\n"
  // THINK_TEXT: /[^<]+/
  // functioncall: %json { <schemas for each tool> }
  //
  // 12. Return chain-of-thought + text or tool call (both marker pairs configured)
  //
  // start: cot output
  // cot: <[125]> THINK_TEXT <[126]> "\\n"
  // THINK_TEXT: /[^<]+/
  // output: TEXT | toolcall
  // TEXT: /[^{<](.|\\n)*/
  // toolcall: <[123]> functioncall <[124]>
  // functioncall: %json { <schemas for each tool> }
  //
  // 13. Return chain-of-thought + text or tool call (only tool-call markers configured)
  //
  // start: cot output
  // cot: "<think>" THINK_TEXT "</think>" "\\n"
  // THINK_TEXT: /[^<]+/
  // output: TEXT | toolcall
  // TEXT: /[^{<](.|\\n)*/
  // toolcall: "<tool_call>" functioncall "</tool_call>"
  // functioncall: %json { <schemas for each tool> }
  //
  // 14. Return chain-of-thought + text or tool call (only reasoning markers configured)
  //
  // start: cot output
  // cot: <[125]> THINK_TEXT <[126]> "\\n"
  // THINK_TEXT: /[^<]+/
  // output: TEXT | functioncall
  // TEXT: /[^{<](.|\\n)*/
  // functioncall: %json { <schemas for each tool> }
  //
  // 15. Return chain-of-thought + text or tool call (no marker pairs configured)
  //
  // start: cot output
  // cot: "<think>" THINK_TEXT "</think>" "\\n"
  // THINK_TEXT: /[^<]+/
  // output: TEXT | functioncall
  // TEXT: /[^{<](.|\\n)*/
  // functioncall: %json { <schemas for each tool> }
  //
  // Note: The THINK_TEXT rule is currently restrictive. It does not allow the model to produce a < token.
  // For now, the models will have to find other tokens in the distribution to use when making a comparison
  // (e.g. is less than). While the rule does not permit empty thinking, that is a rare occurrence.
  // There is usually some reasoning done even if it is little. Without such a restrictive grammar, models
  // such as Phi-4 mini reasoning fall out of distribution.
  //
  // Marker IDs use llguidance's exact numeric-token syntax (`<[ID]>`). A marker without one authoritative token
  // ID uses quoted literal bytes, which may tokenize to multiple IDs. Named `<token_name>` syntax is only for
  // tokenizer special tokens and is not inferred from marker text.
  if (!ctx.text_output && !ctx.tool_output) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "neither text output nor tool calling output are enabled — "
             "enable one via tool_choice");
  }

  bool known_tool_tokens = ctx.HasToolCallTokens();
  bool known_think_tokens = ctx.HasReasoningTokens();
  bool reasoning_enabled = ctx.supports_reasoning;

  // Known boundaries use an exact numeric token ID when available; all others use literal bytes.
  std::string reasoning_start =
      known_think_tokens ? RenderLarkMarker(ctx.reasoning_start, ctx.reasoning_start_token_id) : "\"<think>\"";
  std::string reasoning_end =
      known_think_tokens ? RenderLarkMarker(ctx.reasoning_end, ctx.reasoning_end_token_id) : "\"</think>\"";

  // Set rows for grammar
  std::ostringstream grammar;

  // start rule — determines top-level alternatives
  std::string output_row;

  if (ctx.text_output && !ctx.tool_output) {
    // Set grammar option of only generating text output
    grammar << (reasoning_enabled ? "start: cot TEXT" : "start: TEXT") << "\n";
  } else if (!ctx.text_output && ctx.tool_output) {
    // Set grammar option of only generating tool output
    const auto* tool_start = known_tool_tokens ? "toolcall" : "functioncall";
    grammar << (reasoning_enabled ? "start: cot " : "start: ") << tool_start << "\n";
  } else {
    // Set grammar option of generating text output or tool output
    if (reasoning_enabled) {
      grammar << "start: cot output\n";
      output_row = std::string("output: TEXT | ") + (known_tool_tokens ? "toolcall" : "functioncall");
    } else {
      grammar << "start: TEXT | " << (known_tool_tokens ? "toolcall" : "functioncall") << "\n";
    }
  }

  // Add grammar for chain-of-thought output
  if (reasoning_enabled) {
    if (prompt_opens_reasoning) {
      // The rendered prompt already ends with the reasoning opener (see PromptOpensReasoning in
      // reasoning_stream_splitter.h), so generation starts inside reasoning. The model neither emits nor can be
      // asked to emit the opener again — omitting it here is required, not just an optimization: a grammar that
      // still demanded the opener as the first production would be unsatisfiable against this prompt.
      grammar << "cot: THINK_TEXT " << reasoning_end << " \"\\n\"\n";
    } else {
      grammar << "cot: " << reasoning_start << " THINK_TEXT " << reasoning_end << " \"\\n\"\n";
    }
    grammar << "THINK_TEXT: /[^<]+/\n";
  }

  // Output alternation rule (only when both text+tool with reasoning)
  if (!output_row.empty()) {
    grammar << output_row << "\n";
  }

  // Add grammar for text output
  if (ctx.text_output) {
    grammar << "TEXT: /[^{<](.|\\n)*/\n";
  }

  // Add grammar for tool output
  if (ctx.tool_output) {
    if (known_tool_tokens) {
      grammar << "toolcall: " << RenderLarkMarker(ctx.tool_call_start, ctx.tool_call_start_token_id)
              << " functioncall " << RenderLarkMarker(ctx.tool_call_end, ctx.tool_call_end_token_id) << "\n";
    }

    grammar << "functioncall: %json " << json_schema << "\n";
  }

  // Create combined grammar
  return grammar.str();
}

}  // namespace fl
