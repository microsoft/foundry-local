// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "util/key_value_pairs.h"

#include <string>

namespace fl {

/// Write inference_model.json to the given directory.
/// Contains the model name and prompt template (matching C# InferenceModelFileName format).
void WriteInferenceModelJson(const std::string& directory,
                             const std::string& model_name,
                             const KeyValuePairs& prompt_templates);

/// Fix the location of inference_model.json for AzureFoundryLocal variants.
/// The model blobs download into a sub-directory for the variant, but we don't know the
/// name ahead of time. This copies inference_model.json into any sub-directory that
/// doesn't already have it, then deletes the root copy. Model packages keep the marker
/// at the package root.
/// Matches C# FixAzureFoundryLocalVariantDownload.
void FixVariantInferenceModelJson(const std::string& model_directory);

}  // namespace fl
