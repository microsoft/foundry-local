// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_bundle_manifest.h"

#include <filesystem>
#include <string_view>
#include <vector>

namespace fl {

class ILogger;

/// Prepend @p dir to the process `PATH` environment variable for the lifetime of the process.
///
/// EP provider libraries (CUDA, WebGPU) delay-load sibling dependency DLLs from their own directory,
/// and `RegisterExecutionProviderLibrary` loads the provider DLL eagerly. The directory must be on
/// `PATH` before registration so those dependencies are discoverable. This is a no-op on non-Windows
/// platforms.
///
/// @param dir Directory to prepend to `PATH`.
void PrependDirToProcessPath(const std::filesystem::path& dir);

/// Select the manifest-declared DLLs under @p bin_dir that should be preloaded before the EP provider
/// library is registered, excluding the provider library itself and the core ORT runtime libraries
/// (`onnxruntime.dll`, `onnxruntime-genai.dll`), matched case-insensitively.
///
/// This selection logic is platform-independent (pure path/string manipulation) so it can be unit
/// tested on any platform, even though the actual preloading only happens on Windows.
///
/// @param bin_dir   Directory containing the extracted bundle files.
/// @param manifest  Bundle manifest describing the extracted artifacts.
/// @return Absolute paths of the DLLs that should be preloaded, in manifest order.
std::vector<std::filesystem::path> SelectEpBundleDependenciesToPreload(const std::filesystem::path& bin_dir,
                                                                       const EpBundleManifest& manifest);

class EpBundleDependencyOwner {
 public:
  EpBundleDependencyOwner() = default;
  ~EpBundleDependencyOwner();

  EpBundleDependencyOwner(const EpBundleDependencyOwner&) = delete;
  EpBundleDependencyOwner& operator=(const EpBundleDependencyOwner&) = delete;

  bool Load(const std::filesystem::path& bin_dir, const EpBundleManifest& manifest, std::string_view ep_name,
            ILogger& logger);

 private:
  std::vector<void*> handles_;
};

}  // namespace fl
