// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <filesystem>
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
  std::string provider_variant;
  std::vector<std::string> capabilities;
};

/// Normalize supported provider aliases to ORT GenAI names. CPU normalizes to
/// an empty string (the default CPU provider); unknown providers throw.
std::string NormalizeNonGenerativeProvider(std::string_view provider);

/// Return true when a directory contains any non-generative package signal:
/// the standard manifest file, ComponentManifest metadata, or a supported task.
bool DeclaresNonGenerativePackage(const std::filesystem::path& package_root);

/// Parse and validate a multi-component package root. Returns nullopt when the
/// directory is not a multi-component package; malformed packages throw.
std::optional<NonGenerativePackageMetadata> ReadNonGenerativePackage(
    const std::filesystem::path& package_root);

}  // namespace fl
