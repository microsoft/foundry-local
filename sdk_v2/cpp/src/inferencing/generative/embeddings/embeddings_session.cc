// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/embeddings/embeddings_session.h"

#include "contracts/embeddings.h"
#include "exception.h"
#include "inferencing/generative/embeddings/fp16.h"
#include "inferencing/generative/genai_model_instance.h"
#include "items/tensor_item.h"
#include "items/text_item.h"
#include "model.h"
#include "utils.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <numeric>
#include <ort_genai.h>

namespace fl {

EmbeddingsSession::EmbeddingsSession(const fl::Model& catalog_model, GenAIModelInstance& model,
                                     ILogger& logger, ITelemetry& telemetry)
    : Session(catalog_model, logger, telemetry, /*allow_concurrent_requests=*/true),
      logger_(logger),
      model_(model) {
  logger_.Log(LogLevel::Debug, fmt::format("Creating EmbeddingsSession for model: {}", model.ModelId()));
  // Last so a throw above does not leak a refcount; nothing below can throw.
  model_.AcquireSession();
}

EmbeddingsSession::~EmbeddingsSession() {
  model_.ReleaseSession();
}

SessionType EmbeddingsSession::Type() const {
  return SessionType::kEmbeddings;
}

void EmbeddingsSession::ProcessRequestImpl(const Request& request, Response& response) {
  // OpenAI embeddings JSON pass-through: a TEXT item tagged OPENAI_JSON. Routes to a
  // separate handler that produces an OPENAI_JSON response, parity with chat and audio.
  for (const auto* item : request.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
      const auto& text_item = static_cast<const TextItem&>(*item);

      if (text_item.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
        ProcessEmbeddingsJson(text_item.text, request, response);
        return;
      }
    }
  }

  // Collect input texts and validate item types.
  std::vector<std::string> inputs;
  inputs.reserve(request.items.size());

  for (const auto* item : request.items) {
    if (item->type != FOUNDRY_LOCAL_ITEM_TEXT) {
      FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                       fmt::format("Embeddings expects TEXT items, got type {}",
                                   static_cast<int>(item->type)));
    }

    inputs.push_back(static_cast<const TextItem&>(*item).text);
  }

  if (inputs.empty()) {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
    return;
  }

  // Single batched forward pass for all inputs.
  auto embeddings = GenerateEmbeddingsBatch(inputs);

  // Wrap each embedding as a TensorItem in the response. The vector is heap-allocated
  // and ownership is transferred to the TensorItem deleter via deleter_user_data_, so
  // the buffer is freed correctly without raw new[]/delete[].
  for (auto& embedding : embeddings) {
    auto tensor = std::make_unique<TensorItem>(FOUNDRY_LOCAL_TENSOR_FLOAT, nullptr,
                                               std::vector<int64_t>{static_cast<int64_t>(embedding.size())});
    auto buf = std::make_unique<std::vector<float>>(std::move(embedding));
    tensor->mutable_data = buf->data();
    tensor->data = buf->data();
    tensor->deleter_user_data_ = buf.release();
    tensor->deleter_ = [](const flTensorData*, void* ud) {
      delete static_cast<std::vector<float>*>(ud);
    };
    response.items.push_back(std::move(tensor));
  }

  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
}

void EmbeddingsSession::ProcessEmbeddingsJson(const std::string& request_json,
                                              const Request& /*original_request*/,
                                              Response& response) {
  // Parse the OpenAI embeddings request. Let nlohmann::json::parse_error propagate —
  // matches AudioSession::ProcessAudioTranscriptionJson behavior.
  auto req_json = nlohmann::json::parse(request_json);
  auto req = req_json.get<EmbeddingCreateRequest>();

  // Normalize "input" — variant<string, vector<string>> — into a single vector.
  std::vector<std::string> inputs;
  if (const auto* single = std::get_if<std::string>(&req.input)) {
    inputs.push_back(*single);
  } else {
    inputs = std::get<std::vector<std::string>>(req.input);
  }

  EmbeddingCreateResponse output;
  output.model = req.model;

  // Defend against an empty input list — skip the model layer entirely.
  // Reuse GenerateEmbeddingsBatch so the typed and JSON paths stay bit-for-bit equal
  // under future refactors (parity test relies on this).
  if (!inputs.empty()) {
    auto embeddings = GenerateEmbeddingsBatch(inputs);

    output.data.reserve(embeddings.size());
    for (size_t i = 0; i < embeddings.size(); ++i) {
      output.data.push_back(EmbeddingData{
          .object = "embedding",
          .embedding = std::move(embeddings[i]),
          .index = static_cast<int>(i),
      });
    }
  }

  // Token accounting parity with the typed/handler path is a separate TODO —
  // the legacy embeddings_handler.cc also leaves these at zero today.
  output.usage = EmbeddingUsage{0, 0};

  // encoding_format = "base64" is parsed but intentionally not honored here, matching
  // the existing handler behavior. Follow-up task.

  response.items.push_back(std::make_unique<TextItem>(nlohmann::json(output).dump(),
                                                      FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
}

std::vector<std::vector<float>> EmbeddingsSession::GenerateEmbeddingsBatch(
    const std::vector<std::string>& inputs) {
  // Process each input independently (batch_size=1 per forward pass).
  //
  // Embedding models like Qwen3-Embedding use bidirectional attention —
  // every position attends to every other position, including padding.
  // Batching multiple inputs with right-padding would pollute real-token
  // hidden states with pad-token attention, producing different (wrong)
  // results compared to processing each input alone.
  //
  // The performance cost of per-input forward passes is negligible for
  // embedding models — they're small and inference is fast.
  std::vector<std::vector<float>> results;
  results.reserve(inputs.size());

  for (const auto& input : inputs) {
    results.push_back(GenerateSingleEmbedding(input));
  }

  logger_.Log(LogLevel::Verbose,
              fmt::format("Embeddings: processed {} input(s)", inputs.size()));

  return results;
}

std::vector<float> EmbeddingsSession::GenerateSingleEmbedding(const std::string& input) {
  auto& oga_model = model_.GetOgaModel();

  // 1. Tokenize and append EOS. Encode is serialized on the model's shared tokenizer.
  const auto& eos_ids = model_.GetPreprocessor().GetEosTokenIds();
  auto sequences = model_.GetPreprocessor().Encode(input.c_str());
  if (!eos_ids.empty()) {
    sequences->Append(eos_ids[0], 0);
  }

  const size_t token_count = sequences->SequenceCount(0);

  // 2. Configure generator for a single input.
  auto gen_params = OgaGeneratorParams::Create(oga_model);
  gen_params->SetSearchOption("batch_size", 1.0);
  gen_params->SetSearchOption("max_length", static_cast<double>(token_count + 1));

  auto generator = OgaGenerator::Create(oga_model, *gen_params);
  generator->AppendTokenSequences(*sequences);

  // 3. Single forward pass.
  generator->GenerateNextToken();

  // 4. Extract hidden_states. Shape: [1, token_count, hidden_size]
  auto hidden_states = generator->GetOutput("hidden_states");
  auto shape = hidden_states->Shape();

  int64_t total_elements = 1;
  for (auto dim : shape) {
    total_elements *= dim;
  }

  // 5. Determine hidden_size.
  const auto& config = model_.GetGenAIConfig();
  int hidden_size = config.hidden_size.value_or(0);
  if (hidden_size <= 0) {
    int64_t denom = static_cast<int64_t>(token_count);
    if (denom > 0 && total_elements % denom == 0) {
      hidden_size = static_cast<int>(total_elements / denom);
    }
  }

  if (hidden_size <= 0) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     fmt::format("Embedding: cannot determine hidden_size. total_elements={}, token_count={}",
                                 total_elements, token_count));
  }

  // 6. Extract the embedding from the last token position (EOS).
  const size_t eos_offset = (token_count - 1) * static_cast<size_t>(hidden_size);
  std::vector<float> embedding(hidden_size);

  auto element_type = hidden_states->Type();
  if (element_type == OgaElementType_float32) {
    const auto* data = static_cast<const float*>(hidden_states->Data());
    std::copy(data + eos_offset, data + eos_offset + hidden_size, embedding.begin());
  } else if (element_type == OgaElementType_float16) {
    const auto* data = static_cast<const uint16_t*>(hidden_states->Data());
    for (int k = 0; k < hidden_size; k++) {
      embedding[k] = Fp16ToFp32(data[eos_offset + k]);
    }
  } else {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     fmt::format("Embedding: unsupported hidden_states element type: {}",
                                 static_cast<int>(element_type)));
  }

  // 7. L2 normalize.
  float norm_sq = 0.0f;
  for (float v : embedding) {
    norm_sq += v * v;
  }

  float norm = std::sqrt(norm_sq);
  if (norm > 0.0f) {
    for (float& v : embedding) {
      v /= norm;
    }
  }

  return embedding;
}

}  // namespace fl
