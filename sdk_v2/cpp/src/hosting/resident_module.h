// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <filesystem>
#include <string>

namespace fl::hosting {

struct ResidentModule {
  std::filesystem::path path;
  std::string image_identity;
};

// Inspect the resident image containing an address, without loading a library. Missing build identity fails closed.
ResidentModule ResidentModuleAt(const void* address);
std::string FileModuleIdentity(const std::filesystem::path& path);

}  // namespace fl::hosting
