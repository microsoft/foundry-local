// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <map>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace fl {

enum class ChatBackendKind {
  kGenerator,
  kStaticEngine,
  kDynamicEngine,
};

/// Represents the parsed contents of a genai_config.json file.
/// Maps the C# GenAIConfig / OnnxModel / OnnxDecoder types.
struct GenAIConfig {
  struct OnnxModel {
    int context_length = 0;
    std::string type;
    std::map<std::string, std::string> prompt_templates;

    struct Decoder {
      struct SessionOptions {
        /// Each element is a map of provider name → options.
        /// e.g. [{"cuda": {...}}, {"dml": {...}}]
        std::vector<std::map<std::string, std::string>> provider_options;
      };
      std::optional<SessionOptions> session_options;
    };
    std::optional<Decoder> decoder;

    /// Returns true if the model type is multimodal (phi3v, whisper, phi4mm, fara,
    /// qwen2_5_vl, qwen3_vl, qwen3_5, gemma4, mistral3).
    bool IsMultiModal() const;
  };

  struct Search {
    int max_length = 0;
  };

  struct Engine {
    struct DynamicBatching {
      size_t max_batch_size = 16;
      size_t max_scheduled_tokens = 2048;
    };

    struct StaticBatching {
      size_t max_batch_size = 4;
    };

    std::optional<DynamicBatching> dynamic_batching;
    std::optional<StaticBatching> static_batching;
  };

  std::optional<OnnxModel> model;
  std::optional<Search> search;
  std::optional<Engine> engine;
  std::optional<int> hidden_size;  // embedding dimension from genai_config.json

  /// Returns the first provider key from decoder.session_options.provider_options,
  /// or empty string if not found.
  std::string DefaultProvider() const;

  /// Selects the chat inference backend declared by the model artifact.
  ChatBackendKind GetChatBackendKind() const;

  /// Returns the configured Engine batch capacity, or nullopt for Generator models.
  std::optional<size_t> EngineMaxBatchSize() const;

  /// Load and parse a genai_config.json file. Throws fl::Exception on failure.
  static GenAIConfig LoadFromFile(const std::string& path);
};

}  // namespace fl
