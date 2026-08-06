// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "logger.h"

#include <map>
#include <string>

namespace fl {

/// Scan a model cache directory for locally cached (downloaded) models.
/// Returns a map of model_id -> local_path for each valid model found.
/// Flat models and model packages must have no download.tmp and must have a root
/// inference_model.json containing a model name.
std::map<std::string, std::string> ScanLocalModels(const std::string& cache_directory,
                                                   ILogger& logger);

}  // namespace fl
