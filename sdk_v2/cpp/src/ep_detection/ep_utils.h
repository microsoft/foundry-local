// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace fl {

class ILogger;

/// Verify a set of binaries in @p dir all exist and match their expected SHA-256 hashes.
///
/// @param dir            Directory containing the extracted EP binaries.
/// @param expected       List of (filename, expected_sha256_hex) pairs.
/// @param ep_name        EP name used in warning log messages (e.g. "CUDA EP").
/// @param logger         Logger for diagnostic output.
/// @return true if every file exists and its hash matches; false otherwise.
bool VerifyEpPackage(
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

/// Explicitly preload a DLL from an absolute path on Windows via `LoadLibraryExW` with
/// `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`.
///
/// `PrependDirToProcessPath` only changes the *default* DLL search order — it helps callers that
/// do an unqualified `LoadLibrary("name.dll")` or rely on delay-load imports, but it does not help
/// every loader. In particular, onnxruntime-genai resolves its native runtime DLL by name at
/// model-load time (well after EP registration finishes), and that lookup can still fail with
/// Win32 error 126 ("module not found") even though the file is present on disk and its directory
/// is on `PATH` — the search order the OS ends up using for that specific call is not guaranteed
/// to include a `PATH`-prepended directory. Preloading the DLL up front with explicit search flags
/// sidesteps that ambiguity: once the module is mapped into the process, any later lookup by name
/// finds the already-loaded module instead of re-resolving it from scratch.
///
/// The returned module handle is intentionally leaked (never passed to `FreeLibrary`) — the DLL
/// must stay mapped for the lifetime of the process, since model loading can happen long after
/// this call returns.
///
/// @param dll_path Absolute path to the DLL to preload.
/// @param logger   Logger used to report the path and Win32 error code if the load fails.
/// @return true if the DLL is now loaded (freshly loaded or already resident). Always false on
///         non-Windows platforms.
bool PreloadAbsoluteDll(const std::filesystem::path& dll_path, ILogger& logger);

}  // namespace fl