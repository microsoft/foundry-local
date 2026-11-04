// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct NonGenerativePackageMetadata {
  std::string model_id;
  std::string alias;
  std::string task;
  std::filesystem::path component_manifest;
  std::string execution_provider;
  std::vector<std::string> capabilities;
  uint64_t package_bytes{};
  uint64_t estimated_resident_bytes{};
};

/// Return true when package entries or metadata declare a non-generative
/// package, even when the declared manifest is missing or invalid.
bool DeclaresNonGenerativePackage(const std::filesystem::path& package_root);

/// Parse and validate a multi-component package root. Returns nullopt when the
/// directory is not a multi-component package; malformed packages throw.
std::optional<NonGenerativePackageMetadata> ReadNonGenerativePackage(
    const std::filesystem::path& package_root);

}  // namespace fl
