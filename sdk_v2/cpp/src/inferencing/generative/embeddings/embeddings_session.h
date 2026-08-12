// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/session.h"
#include "logger.h"

namespace fl {

class GenAIModelInstance;

/// Embedding session — stateless, concurrent-safe.
/// Each ProcessRequestImpl call creates a fresh generator, runs one forward pass,
/// extracts hidden_states, does last-token pooling + L2 normalization.
///
/// Dual input contract (parity with ChatSession and AudioSession):
/// - Typed path: a Request with one or more TEXT items. Response contains one TENSOR item
///   per input carrying the L2-normalized embedding vector.
/// - OPENAI_JSON path: a Request with a single TEXT item tagged OPENAI_JSON containing an
///   OpenAI EmbeddingCreateRequest payload. Response is a single TEXT item tagged OPENAI_JSON
///   containing the EmbeddingCreateResponse.
class EmbeddingsSession : public Session {
 public:
  EmbeddingsSession(const fl::Model& catalog_model, GenAIModelInstance& model,
                    ILogger& logger, ITelemetry& telemetry);
  ~EmbeddingsSession();

  EmbeddingsSession(EmbeddingsSession&&) = delete;
  EmbeddingsSession& operator=(EmbeddingsSession&&) = delete;

  SessionType Type() const override;

 protected:
  void ProcessRequestImpl(const Request& request, Response& response) override;

 private:
  /// Generate L2-normalized embedding vectors for a list of inputs.
  /// Each input is processed independently (batch_size=1) to avoid
  /// padding artifacts with bidirectional-attention embedding models.
  ///
  /// Stops early if `request` is cancelled or its deadline expires, so the returned
  /// vector may be shorter than `inputs`. Callers must check the request state before
  /// treating the result as complete.
  std::vector<std::vector<float>> GenerateEmbeddingsBatch(const std::vector<std::string>& inputs,
                                                          const Request& request);

  /// Generate a single L2-normalized embedding vector for one input string.
  /// Returns an empty vector if the forward pass was interrupted by cancellation or a timeout.
  std::vector<float> GenerateSingleEmbedding(const std::string& input, const Request& request);

  /// Process a request whose first item is a TEXT item tagged OPENAI_JSON containing an
  /// OpenAI EmbeddingCreateRequest payload. Parses the JSON, runs generation via the
  /// shared GenerateEmbeddingsBatch path, and produces an OPENAI_JSON-tagged TextItem
  /// wrapping the EmbeddingCreateResponse.
  void ProcessEmbeddingsJson(const std::string& request_json,
                             const Request& original_request, Response& response);

  ILogger& logger_;
  GenAIModelInstance& model_;
};

}  // namespace fl
