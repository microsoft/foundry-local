// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <filesystem>

namespace fl {

enum class ModelLayout {
  FlatModel,
  ModelPackage,
  Incomplete,
  Invalid,
};

/// Classify the model layout from its root identifying files.
///
/// A model package has a regular manifest.json and no genai_config.json at the root.
/// A root genai_config.json always identifies a flat model, even when manifest.json is also present.
ModelLayout ClassifyModelLayout(const std::filesystem::path& model_path);

}  // namespace fl
