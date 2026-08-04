// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_bundle_manifest.h"

#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace fl {

class ILogger;

/// Verify an EP archive file matches the expected SHA-256 hash.
///
/// @param archive_path   Archive file path to verify.
/// @param expected_hash  Expected SHA-256 hash for @p archive_path.
/// @param ep_name        EP name used in warning log messages.
/// @param logger         Logger for diagnostic output.
/// @return true if archive exists and hash matches; false otherwise.
bool VerifyEpArchive(
    const std::filesystem::path& archive_path,
    std::string_view expected_hash,
    std::string_view ep_name,
    ILogger& logger);

/// Verify a set of binaries in @p dir all exist and match their expected SHA-256 hashes.
///
/// @param dir            Directory containing the extracted EP binaries.
/// @param expected       List of (filename, expected_sha256_hex) pairs.
/// @param ep_name        EP name used in warning log messages (e.g. "CUDA EP").
/// @param logger         Logger for diagnostic output.
/// @return true if every file exists and its hash matches; false otherwise.
bool VerifyEpBinaries(
    const std::filesystem::path& dir,
    std::initializer_list<std::pair<std::string_view, std::string_view>> expected,
    std::string_view ep_name,
    ILogger& logger);

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
std::vector<std::filesystem::path> SelectEpBundleDependenciesToPreload(
    const std::filesystem::path& bin_dir,
    const EpBundleManifest& manifest);

/// Preload the non-provider, non-core-runtime DLLs declared by @p manifest from @p bin_dir.
///
/// EP provider libraries (CUDA, WebGPU) can implicitly or delay-load sibling dependency DLLs, and
/// `RegisterExecutionProviderLibrary` loads the provider DLL eagerly. Preloading those dependencies by
/// absolute path with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32` ensures they
/// resolve correctly regardless of the process `PATH`, before the provider DLL is registered.
///
/// This is a no-op that returns true on non-Windows platforms.
///
/// @param bin_dir  Directory containing the extracted bundle files.
/// @param manifest Bundle manifest describing the extracted artifacts.
/// @param ep_name  EP name used in warning log messages (e.g. "CUDA EP").
/// @param logger   Logger for diagnostic output.
/// @return true if every selected dependency loaded successfully (or none needed loading); false
///         otherwise.
bool LoadEpBundleDependencies(
    const std::filesystem::path& bin_dir,
    const EpBundleManifest& manifest,
    std::string_view ep_name,
    ILogger& logger);

}  // namespace fl
