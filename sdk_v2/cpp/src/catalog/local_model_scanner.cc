// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_scanner.h"
#include "util/model_layout.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fl {

namespace fs = std::filesystem;

namespace {

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

bool HasCompletionMarker(const fs::path& dir) {
  std::error_code ec;
  const auto is_regular_file = fs::is_regular_file(dir / kInferenceModelFileName, ec);
  return !ec && is_regular_file;
}

bool IsCompleteModelDirectory(const fs::path& dir) {
  std::error_code ec;
  const auto download_in_progress = fs::exists(dir / kDownloadSignalFileName, ec);
  if (ec || download_in_progress) {
    return false;
  }

  return HasCompletionMarker(dir);
}

void AddModelDirectory(const fs::path& dir, std::map<std::string, std::string>& results) {
  auto model_name = ReadModelNameFromInferenceModel(dir);
  if (model_name.empty()) {
    return;
  }

  // If the model name doesn't contain a ':' version separator, append ":0".
  if (model_name.find(':') == std::string::npos) {
    model_name += ":0";
  }

  results[model_name] = dir.string();
}

/// Recursively scan a directory for valid model directories.
void ScanDirectory(const fs::path& dir,
                   std::map<std::string, std::string>& results,
                   ILogger& logger) {
  try {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      return;
    }

    const auto layout = ClassifyModelLayout(dir);
    if (layout == ModelLayout::ModelPackage) {
      if (IsCompleteModelDirectory(dir)) {
        AddModelDirectory(dir, results);
      }

      // Package children are implementation details, not independently loadable models.
      return;
    }

    if (layout == ModelLayout::FlatModel && IsCompleteModelDirectory(dir)) {
      AddModelDirectory(dir, results);
      return;
    }

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
