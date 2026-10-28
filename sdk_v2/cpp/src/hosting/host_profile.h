// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "configuration.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace fl::hosting {

struct RuntimeIdentity {
  std::string native_build;
  std::string runtime_identifier;
  uint32_t ipc_protocol_version = 0;
  std::string native_sha256;
  std::string ort_sha256;
  std::string genai_sha256;
  std::map<std::string, std::string> execution_dependencies;
};

struct HostProfile {
  Configuration configuration;
  nlohmann::json compatibility;
  std::string key;
};

// Identities describe the selected native assets. Installation paths and language-wrapper versions are not identities.
HostProfile BuildHostProfile(const Configuration& configuration, const RuntimeIdentity& runtime);

}  // namespace fl::hosting
