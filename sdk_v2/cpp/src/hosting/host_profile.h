// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "configuration.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace fl::hosting {

struct RuntimeIdentity {
  std::string native_build;
  std::string runtime_identifier;
  uint32_t ipc_protocol_version = 0;
};

struct HostProfile {
  Configuration configuration;
  nlohmann::json compatibility;
  std::string key;
};

// Compatibility follows the Foundry build and IPC contract, not dependency packaging or binary fingerprints.
HostProfile BuildHostProfile(const Configuration& configuration, const RuntimeIdentity& runtime);

}  // namespace fl::hosting
