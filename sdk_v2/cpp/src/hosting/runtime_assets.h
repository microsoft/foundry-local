// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "hosting/bootstrap_descriptor.h"
#include "hosting/host_profile.h"
#include "hosting/resident_module.h"

namespace fl::hosting {

struct RuntimeAssets {
  std::filesystem::path executable;
  BootstrapDescriptor bootstrap;
  RuntimeIdentity identity;
};

RuntimeAssets ResolveRuntimeAssets();
RuntimeAssets ResolveRuntimeAssets(const ResidentModule& native);
// The explicit form also permits testing discovery against a separately loaded, public-API-only runtime.
RuntimeAssets ResolveRuntimeAssets(const ResidentModule& native, const ResidentModule& ort,
                                   const ResidentModule& genai);

}  // namespace fl::hosting
