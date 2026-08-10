// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>
#include <vector>
#include <regex>

namespace fl {

/// Data-driven configuration for tool-call parsing.
/// All format-specific knowledge is captured as data properties — no model-specific
/// code branches. A single accumulator interprets the config at runtime.
struct ToolCallConfig {
  enum class Mode {
    kSimple,            // ChatML: bot → JSON body (has name+args) → eot
    kHeaderInspection   // Harmony: bot → header → message_token → args JSON → end_token
  };

  Mode mode = Mode::kSimple;

  // --- Header-inspection mode properties ---

  /// Token that separates the header from the message body (e.g., "<|message|>")
  std::string message_token;

  /// Regex to match the header and extract the function name.
  /// Capture group 1 = function name (e.g., "to=functions\\.(.+)" captures "get_weather")
  std::string header_regex;

  /// Token between header routing and channel info (e.g., "<|channel|>")
  /// Used to trim the header before regex matching.
  std::string channel_token;

  /// All tokens that terminate a tool-call block.
  /// For Harmony: both <|end|> (intermediate) and <|call|> (final/EOS) terminate.
  /// For Simple mode: this is just {eot_marker} (populated from context).
  std::vector<std::string> end_tokens;

  // --- Factory methods ---

  /// Default config for ChatML-style models (Phi, Qwen).
  /// Markers come from ToolCallContext; this just sets mode = kSimple.
  static ToolCallConfig Simple() {
    return ToolCallConfig{Mode::kSimple};
  }

  /// Config for GPT-OSS (Harmony) models.
  static ToolCallConfig Harmony() {
    ToolCallConfig cfg;
    cfg.mode = Mode::kHeaderInspection;
    cfg.message_token = "<|message|>";
    cfg.header_regex = R"(to=functions\.(.+))";
    cfg.channel_token = "<|channel|>";
    cfg.end_tokens = {"<|end|>", "<|call|>"};
    return cfg;
  }

  /// Infer config from model type string (from genai_config.json "model.type").
  static ToolCallConfig FromModelType(const std::string& model_type) {
    if (model_type == "gptoss") return Harmony();
    return Simple();
  }
};

}  // namespace fl
