// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Resolves the on-disk path of a shared test model by its id, reusing the production
// LocalModelScanner so discovery matches real catalog behaviour (any directory layout,
// matched on the id in each model's inference_model.json "Name" field).
//
// This header pulls in internal SDK headers (catalog/local_model_scanner.h), so it must
// only be included by the internal_api test target (foundry_local_tests), which has src/
// on its include path. Do NOT include it from sdk_api tests — that target validates the
// public API surface in isolation and has no access to internal headers.
#pragma once

#include "catalog/local_model_scanner.h"
#include "logger.h"
#include "utils/safe_getenv.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace fl::test {

/// Resolve the test model cache root directory.
/// FOUNDRY_TEST_DATA_DIR is required. Models may live anywhere beneath this root
/// (with or without a publisher subdirectory) — discovery is by the model id in each
/// model's inference_model.json, not by directory layout.
inline fs::path GetTestModelCacheDir() {
  std::string env_value = SafeGetEnv("FOUNDRY_TEST_DATA_DIR");
  if (env_value.empty()) {
    throw std::runtime_error(
        "FOUNDRY_TEST_DATA_DIR is not set. Set it to a local model cache directory before running tests.");
  }

  fs::path env_path(env_value);
  if (!fs::exists(env_path)) {
    throw std::runtime_error("FOUNDRY_TEST_DATA_DIR does not exist: " + env_path.string());
  }

  return fs::canonical(env_path);
}

/// Get the effective model path — the directory containing genai_config.json.
/// ModelLoadManager::LoadModel expects this resolved path. The model is located by scanning
/// the entire cache root via the production ScanLocalModels() and matching on the model id
/// stored in each model's inference_model.json "Name" field, so the directory layout
/// (publisher subfolder, version subfolder, etc.) does not matter — only the id has to match.
/// @param model_alias  e.g. "qwen2.5-0.5b-instruct-generic-cpu-4"
inline fs::path GetTestModelPath(const std::string& model_alias) {
  fs::path cache_dir = GetTestModelCacheDir();

  // Model ids use a ':' before the version (e.g. "...-generic-cpu:4"), whereas test aliases
  // use a trailing '-<version>'. Convert the last '-' to ':' so the alias matches the id
  // emitted by the scanner.
  std::string model_id = model_alias;
  if (auto pos = model_id.find_last_of('-'); pos != std::string::npos) {
    model_id[pos] = ':';
  }

  StderrLogger logger;
  auto models = ScanLocalModels(cache_dir.string(), logger);

  if (auto it = models.find(model_id); it != models.end()) {
    return fs::canonical(it->second);
  }

  throw std::runtime_error("Test model '" + model_id + "' (alias '" + model_alias +
                           "') not found anywhere under cache root: " + cache_dir.string());
}

}  // namespace fl::test
