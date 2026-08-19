// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Example: Text embeddings with the Foundry Local C++ SDK.
// Demonstrates: create manager, find an embeddings model, download/load it,
// generate embeddings for single and batch inputs, and compute cosine similarity.

#include <foundry_local/foundry_local_cpp.h>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace foundry_local;

/// Compute cosine similarity between two vectors of the same length.
float CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
  float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }

  if (norm_a == 0.0f || norm_b == 0.0f) {
    return 0.0f;
  }

  return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

int main() {
  try {
    // 1. Create a configuration and manager.
    Configuration config("embeddings_example");
    Manager manager(std::move(config));

    // 2. Find an embeddings model in the catalog and select the CPU variant.
    auto& catalog = manager.GetCatalog();
    ModelList all_models = catalog.GetModels();

    std::string embeddings_alias;
    for (const auto& m : all_models) {
      if (m->GetInfo().Task() == "embeddings") {
        embeddings_alias = m->GetInfo().Alias();
        break;
      }
    }

    if (embeddings_alias.empty()) {
      std::cerr << "No embeddings model found in catalog.\n";
      return 1;
    }

    // Get a mutable handle via the catalog lookup.
    auto model = catalog.GetModel(embeddings_alias);
    if (!model) {
      std::cerr << "Failed to retrieve embeddings model.\n";
      return 1;
    }

    ModelInfo info = model->GetInfo();
    std::cout << "Using model: " << info.Name() << " (alias: " << info.Alias() << ")\n";

    // 3. Download if not already cached.
    if (!model->IsCached()) {
      std::cout << "Downloading...\n";
      model->Download([](float progress) -> bool {
        std::cout << "\r  " << static_cast<int>(progress) << "%" << std::flush;
        return true;
      });
      std::cout << "\n";
    }

    // 4. Load the model.
    if (!model->IsLoaded()) {
      std::cout << "Loading model...\n";
      model->Load();
    }

    // 5. Create an embeddings session and use it.
    {
      EmbeddingsSession session(*model);

      // 6. Generate a single embedding.
      std::cout << "\n--- Single embedding ---\n";
      std::vector<float> embedding = session.Embed("The quick brown fox jumps over the lazy dog.");
      std::cout << "Dimensions: " << embedding.size() << "\n";
      std::cout << "First 5 values: [";
      for (size_t i = 0; i < 5 && i < embedding.size(); ++i) {
        std::cout << (i > 0 ? ", " : "") << embedding[i];
      }
      std::cout << "]\n";

      // 7. Generate embeddings for a batch and compute similarities.
      std::cout << "\n--- Batch embeddings + cosine similarity ---\n";
      std::vector<std::string> sentences = {
          "The cat sat on the mat.",
          "A kitten rested on the rug.",
          "The stock market crashed yesterday.",
      };

      std::vector<std::vector<float>> embeddings = session.Embed(sentences);

      if (embeddings.empty()) {
        std::cerr << "No embeddings returned for batch input.\n";
        return 1;
      }

      std::cout << "Generated " << embeddings.size() << " embeddings of dimension "
                << embeddings[0].size() << "\n\n";

      // Compare all pairs.
      for (size_t i = 0; i < sentences.size(); ++i) {
        for (size_t j = i + 1; j < sentences.size(); ++j) {
          float sim = CosineSimilarity(embeddings[i], embeddings[j]);
          std::cout << "  similarity(\"" << sentences[i] << "\",\n"
                    << "             \"" << sentences[j] << "\") = " << sim << "\n\n";
        }
      }
    }  // session destroyed before unload

    // 8. Unload when done.
    model->Unload();
  } catch (const Error& ex) {
    std::cerr << "Foundry Local error [" << ex.Code() << "]: " << ex.what() << "\n";
    return 1;
  }

  return 0;
}
