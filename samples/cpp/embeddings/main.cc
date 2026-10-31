// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_cpp.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "samples/cpp/sample_common.h"

namespace {

float CosineSimilarity(const std::vector<float>& left, const std::vector<float>& right) {
  if (left.size() != right.size()) {
    throw std::invalid_argument("Embedding dimensions must match");
  }

  float dot = 0.0F;
  for (size_t index = 0; index < left.size(); ++index) {
    dot += left[index] * right[index];
  }

  // EmbeddingsSession returns L2-normalized vectors, so their dot product is cosine similarity.
  return dot;
}

}  // namespace

int main() {
  using namespace foundry_local;

  try {
    Configuration configuration{std::string(sample::kAppName)};
    Manager manager(std::move(configuration));
    sample::RegisterExecutionProviders(manager);

    auto model = sample::GetModel(manager.GetCatalog(), "qwen3-embedding-0.6b");
    sample::LoadedModel loaded_model(*model);
    EmbeddingsSession session(*model);

    const std::vector<std::string> inputs{
        "A kitten is sleeping on a rug.",
        "A cat rests on a mat.",
        "Saturn has a prominent ring system.",
    };
    const auto embeddings = session.Embed(inputs);
    if (embeddings.size() != inputs.size() || embeddings.empty()) {
      throw std::runtime_error("The embeddings response did not match the input batch");
    }

    std::cout << "Model: " << model->GetInfo().Id() << "\n";
    std::cout << "Dimensions: " << embeddings.front().size() << "\n";
    std::cout << "Similarity (cat, cat): " << CosineSimilarity(embeddings[0], embeddings[1]) << "\n";
    std::cout << "Similarity (cat, Saturn): " << CosineSimilarity(embeddings[0], embeddings[2]) << "\n";
  } catch (const Error& error) {
    std::cerr << "Foundry Local error [" << error.Code() << "]: " << error.what() << "\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
