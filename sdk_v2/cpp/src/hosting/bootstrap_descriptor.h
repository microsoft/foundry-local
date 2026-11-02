// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "configuration.h"
#include "hosting/child_host_role.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace fl::hosting {

struct BootstrapArtifact {
  std::filesystem::path path;
  std::string sha256;
};

struct BootstrapDescriptor {
  BootstrapArtifact native;
  BootstrapArtifact ort;
  BootstrapArtifact genai;
  std::filesystem::path manifest;
  std::string native_build;
  std::string runtime_identifier;
  nlohmann::json compatibility;
  Configuration configuration;
  ChildHostRole child;
};

// Paths locate selected assets; the compatibility document contains identities, not installation directories.
nlohmann::json EncodeBootstrapDescriptor(const BootstrapDescriptor& descriptor);
BootstrapDescriptor DecodeBootstrapDescriptor(const nlohmann::json& value);

// This verifies the actual packaged executable, its paired native runtime, and the selected dependency bytes.
// It loads no libraries and does not change process-wide loader search paths.
void VerifyBootstrapArtifacts(const BootstrapDescriptor& descriptor, const std::filesystem::path& executable,
                              std::string_view expected_build, std::string_view expected_rid);
nlohmann::json ReadBootstrapJson(const std::filesystem::path& path);

}  // namespace fl::hosting
