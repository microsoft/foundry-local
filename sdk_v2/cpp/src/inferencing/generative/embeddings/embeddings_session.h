// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/model_session_lease.h"
#include "inferencing/session/session.h"
#include "inferencing/session/session_runtime.h"
#include "logger.h"

#include <string>
#include <utility>
#include <vector>

namespace fl {

class GenAIModelInstance;

/// Executable body of an embeddings session — stateless, concurrent-safe.
/// Each ProcessRequestImpl call creates a fresh generator, runs one forward pass,
/// extracts hidden_states, does last-token pooling + L2 normalization.
///
/// Dual input contract (parity with ChatRuntime and AudioRuntime):
/// - Typed path: a Request with one or more TEXT items. Response contains one TENSOR item
///   per input carrying the L2-normalized embedding vector.
/// - OPENAI_JSON path: a Request with a single TEXT item tagged OPENAI_JSON containing an
///   OpenAI EmbeddingCreateRequest payload. Response is a single TEXT item tagged OPENAI_JSON
///   containing the EmbeddingCreateResponse.
///
/// Both paths — including the validation-only empty-input case — build complete pending responses before
/// OperationContext::TrySeal(). Success publishes by noexcept swap; cancellation publishes an empty
/// FINISH_NONE response, never a partial batch.
class EmbeddingsRuntime : public SessionRuntime {
 public:
  EmbeddingsRuntime(const fl::Model& catalog_model, ModelSessionLease lease, ILogger& logger,
                    ITelemetry& telemetry);
  ~EmbeddingsRuntime() noexcept override;

  SessionType Type() const override;

 protected:
  void ProcessRequestImpl(const OperationContext& operation, const Request& request, Response& response) override;

 private:
  /// Generate L2-normalized embedding vectors for a list of inputs.
  /// Each input is processed independently (batch_size=1) to avoid
  /// padding artifacts with bidirectional-attention embedding models.
  ///
  /// Stops early if `operation` was stopped (cancel or deadline), so the returned vector may be shorter
  /// than `inputs`. Callers must check operation.ShouldStop() before treating the result as complete.
  std::vector<std::vector<float>> GenerateEmbeddingsBatch(const OperationContext& operation,
                                                          const std::vector<std::string>& inputs);

  /// Generate a single L2-normalized embedding vector for one input string.
  /// Returns an empty vector if the forward pass was interrupted by cancellation or a timeout.
  std::vector<float> GenerateSingleEmbedding(const OperationContext& operation, const std::string& input);

  /// Process a request whose first item is a TEXT item tagged OPENAI_JSON containing an
  /// OpenAI EmbeddingCreateRequest payload. Parses the JSON, runs generation via the
  /// shared GenerateEmbeddingsBatch path, and produces an OPENAI_JSON-tagged TextItem
  /// wrapping the EmbeddingCreateResponse.
  void ProcessEmbeddingsJson(const OperationContext& operation, const std::string& request_json,
                             Response& response);

  ILogger& logger_;
};

/// Embedding session facade — see Session. Concurrent by design: several operations may run against the
/// same runtime at once, each with its own cancellation slots.
class EmbeddingsSession : public Session {
 public:
  /// Compatibility construction against an already-pinned model. The caller must exclude a concurrent
  /// unload until construction returns; production paths should pass a manager-acquired ModelSessionLease.
  EmbeddingsSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger,
                    ITelemetry& telemetry)
      : EmbeddingsSession(catalog_model, ModelSessionLease::Adopt(model), logger, telemetry) {}

  /// Construction from a lease acquired atomically against unload (Session::Create).
  EmbeddingsSession(const fl::Model& catalog_model, ModelSessionLease lease, ILogger& logger,
                    ITelemetry& telemetry)
      : Session(MakeSessionRuntime<EmbeddingsRuntime>(catalog_model, std::move(lease), logger, telemetry)) {}

  EmbeddingsSession(EmbeddingsSession&&) noexcept = default;
  EmbeddingsSession& operator=(EmbeddingsSession&&) = delete;
};

}  // namespace fl
