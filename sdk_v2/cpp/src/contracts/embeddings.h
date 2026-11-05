// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fl {

// ========================================================================
// OpenAI Embeddings API — Request/Response contract types.
// Mirrors POST /v1/embeddings endpoint.
// ========================================================================

// --- Request types ---

/// The embeddings request. "input" can be a single string or array of strings.
struct EmbeddingCreateRequest {
  std::string model;
  std::variant<std::string, std::vector<std::string>> input;
  std::optional<std::string> encoding_format;  // "float" (default) or "base64"
};

// --- Response types ---

struct EmbeddingData {
  std::string object = "embedding";
  std::vector<float> embedding;
  int index = 0;
};

struct EmbeddingUsage {
  int64_t prompt_tokens = 0;
  int64_t total_tokens = 0;
};

struct EmbeddingCreateResponse {
  std::string object = "list";
  std::string model;
  std::vector<EmbeddingData> data;
  EmbeddingUsage usage;
};

// ========================================================================
// JSON serialization (nlohmann ADL)
// ========================================================================

void from_json(const nlohmann::json& j, EmbeddingCreateRequest& r);
void to_json(nlohmann::json& j, const EmbeddingData& d);
void to_json(nlohmann::json& j, const EmbeddingUsage& u);
void to_json(nlohmann::json& j, const EmbeddingCreateResponse& r);

}  // namespace fl
