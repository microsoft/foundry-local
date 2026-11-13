// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/genai_config.h"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fl {

struct NonGenerativePackageMetadata {
  std::string model_id;
  std::string alias;
  std::string task;
  std::string execution_provider;
  uint64_t estimated_resident_bytes{};
  GenAIConfig config;
};

/// Return true when package entries or metadata declare a non-generative
/// package, even when the declared manifest is missing or invalid.
bool DeclaresNonGenerativePackage(const std::filesystem::path& package_root);

/// Parse and validate a multi-component package root. Returns nullopt when the
/// directory is not a multi-component package; malformed packages throw.
/// Only the downloader may allow its own in-progress marker during final validation.
std::optional<NonGenerativePackageMetadata> ReadNonGenerativePackage(
    const std::filesystem::path& package_root, bool allow_in_progress = false);

}  // namespace fl
