// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_scanner.h"
#include "catalog/non_generative_package.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fl {

namespace fs = std::filesystem;

namespace {

const char* kGenAIConfigFileName = "genai_config.json";
const char* kDownloadSignalFileName = "download.tmp";
const char* kInferenceModelFileName = "inference_model.json";

/// Try to read the model name from inference_model.json in the given directory.
/// Returns the model name string, or empty string on failure.
std::string ReadModelNameFromInferenceModel(const fs::path& dir) {
  auto path = dir / kInferenceModelFileName;
  if (!fs::exists(path)) {
    return {};
  }

  try {
    std::ifstream file(path);
    if (!file.is_open()) {
      return {};
    }

    auto j = nlohmann::json::parse(file);
    if (j.contains("Name") && j["Name"].is_string()) {
      return j["Name"].get<std::string>();
    }
  } catch (...) {
    // Ignore parse errors for individual files.
  }

  return {};
}

/// Check if a directory is a valid cached model directory.
/// Must have genai_config.json, no download.tmp, and an inference_model.json with a Name.
bool IsValidModelDirectory(const fs::path& dir) {
  if (!fs::exists(dir / kGenAIConfigFileName)) {
    return false;
  }

  if (fs::exists(dir / kDownloadSignalFileName)) {
    return false;  // Incomplete download.
  }

  if (!fs::exists(dir / kInferenceModelFileName)) {
    return false;
  }

  return true;
}

bool IsValidBundleDirectory(const fs::path& dir) {
  return ReadNonGenerativePackage(dir).has_value();
}

bool DeclaresBundleDirectory(const fs::path& dir) {
  const auto path = dir / kInferenceModelFileName;
  if (!fs::is_regular_file(path)) return false;
  try {
    std::ifstream file(path);
    const auto json = nlohmann::json::parse(file);
    return json.is_object() && json.contains("ComponentManifest");
  } catch (...) {
    return false;
  }
}

/// Recursively scan a directory for valid model directories.
void ScanDirectory(const fs::path& dir,
                   std::map<std::string, std::string>& results,
                   ILogger& logger) {
  try {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      return;
    }

    // Package-shaped directories must pass strict bundle validation and may
    // never fall back to the more permissive generative leaf check.
    const bool declares_bundle = DeclaresBundleDirectory(dir);
    if ((declares_bundle && IsValidBundleDirectory(dir)) ||
        (!declares_bundle && IsValidModelDirectory(dir))) {
      auto model_name = ReadModelNameFromInferenceModel(dir);
      if (!model_name.empty()) {
        // If the model name doesn't contain a ':' version separator, append ":0".
        if (model_name.find(':') == std::string::npos) {
          model_name += ":0";
        }

        results[model_name] = dir.string();
      }

      // Don't recurse into a valid model directory — it's a leaf.
      return;
    }
    if (declares_bundle) return;

    // Recurse into subdirectories.
    for (const auto& entry : fs::directory_iterator(dir)) {
      if (entry.is_directory()) {
        ScanDirectory(entry.path(), results, logger);
      }
    }
  } catch (const std::exception& ex) {
    logger.Log(LogLevel::Warning,
               fmt::format("local model scan: error scanning {} — {}", dir.string(), ex.what()));
  } catch (...) {
    logger.Log(LogLevel::Warning,
               fmt::format("local model scan: unknown error scanning {}", dir.string()));
  }
}

}  // anonymous namespace

std::map<std::string, std::string> ScanLocalModels(const std::string& cache_directory,
                                                   ILogger& logger) {
  std::map<std::string, std::string> results;

  if (cache_directory.empty()) {
    return results;
  }

  fs::path cache_path(cache_directory);
  if (!fs::exists(cache_path) || !fs::is_directory(cache_path)) {
    return results;
  }

  // Scan each top-level subdirectory (publisher directories).
  try {
    for (const auto& entry : fs::directory_iterator(cache_path)) {
      if (entry.is_directory()) {
        ScanDirectory(entry.path(), results, logger);
      }
    }
  } catch (const std::exception& ex) {
    logger.Log(LogLevel::Warning,
               fmt::format("local model scan: error iterating cache directory — {}", ex.what()));
  } catch (...) {
    logger.Log(LogLevel::Warning, "local model scan: unknown error iterating cache directory");
  }

  return results;
}

}  // namespace fl
