// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/inference_model_writer.h"
#include "exception.h"
#include "util/model_layout.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fl {

namespace {

const char* kInferenceModelFileName = "inference_model.json";

}  // anonymous namespace

void WriteInferenceModelJson(const std::string& directory,
                             const std::string& model_name,
                             const KeyValuePairs& prompt_templates) {
  nlohmann::json j;
  j["Name"] = model_name;

  if (prompt_templates.empty()) {
    j["PromptTemplate"] = nullptr;
  } else {
    nlohmann::json pt;
    for (const auto& [key, value] : prompt_templates) {
      pt[key] = value;
    }
    j["PromptTemplate"] = pt;
  }

  auto path = std::filesystem::path(directory) / kInferenceModelFileName;
  std::ofstream out(path);
  if (!out) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to create " + path.string());
  }
  out << j.dump(2);
  out.close();
  if (out.fail()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to write " + path.string());
  }
}

void FixVariantInferenceModelJson(const std::string& model_directory) {
  if (ClassifyModelLayout(model_directory) == ModelLayout::ModelPackage) {
    return;
  }

  auto inference_model_path = std::filesystem::path(model_directory) / kInferenceModelFileName;
  if (!std::filesystem::exists(inference_model_path)) {
    return;  // Nothing to fix
  }

  // Copy inference_model.json into each subdirectory that doesn't have one
  bool copied_to_subdir = false;
  for (const auto& entry : std::filesystem::directory_iterator(model_directory)) {
    if (!entry.is_directory()) {
      continue;
    }

    auto dest = entry.path() / kInferenceModelFileName;
    if (!std::filesystem::exists(dest)) {
      std::filesystem::copy_file(inference_model_path, dest);
    }
    copied_to_subdir = true;
  }

  // Only delete the root copy if at least one variant subdirectory exists
  // (and now has its own copy). For single-variant downloads where every blob
  // landed at the root, the root file IS the model — deleting it would leave
  // the cache empty and IsModelCached would report false on the next check.
  if (copied_to_subdir) {
    std::error_code ec;
    std::filesystem::remove(inference_model_path, ec);
    // Ignore deletion errors (matching C# behavior which logs but doesn't throw)
  }
}

}  // namespace fl
