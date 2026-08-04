// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_utils.h"

#include "logger.h"
#include "util/sha256.h"
#include "util/string_utils.h"

#include <fmt/format.h>

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fl {

namespace {

// The provider library itself and the core ORT runtime are handled separately from bundle
// dependencies: the provider is loaded by `RegisterExecutionProviderLibrary`, and the core runtime is
// already loaded by the host process, so neither should be preloaded again here.
bool IsCoreRuntimeLibrary(const std::filesystem::path& filename) {
  const auto name = filename.string();
  return CompareCaseInsensitive(name, "onnxruntime.dll") == 0 ||
         CompareCaseInsensitive(name, "onnxruntime-genai.dll") == 0;
}

}  // namespace

bool VerifyEpArchive(
    const std::filesystem::path& archive_path,
    std::string_view expected_hash,
    std::string_view ep_name,
    ILogger& logger) {
  if (!std::filesystem::exists(archive_path)) {
    logger.Log(LogLevel::Warning,
               fmt::format("{}: archive missing: {}", ep_name, archive_path.string()));
    return false;
  }

  if (expected_hash.empty()) {
    logger.Log(LogLevel::Warning,
               fmt::format("{}: archive hash missing for {}", ep_name, archive_path.string()));
    return false;
  }

  auto hash = Sha256File(archive_path);
  if (CompareCaseInsensitive(hash, std::string(expected_hash)) != 0) {
    logger.Log(LogLevel::Warning,
               fmt::format("{}: archive hash mismatch for {}: got {}, expected {}",
                           ep_name,
                           archive_path.filename().string(),
                           hash,
                           expected_hash));
    return false;
  }

  return true;
}

bool VerifyEpBinaries(
    const std::filesystem::path& dir,
    std::initializer_list<std::pair<std::string_view, std::string_view>> expected,
    std::string_view ep_name,
    ILogger& logger) {

  for (const auto& [filename, expected_hash] : expected) {
    auto file_path = dir / filename;

    if (!std::filesystem::exists(file_path)) {
      return false;
    }

    auto hash = Sha256File(file_path);

    // Case-insensitive hex comparison
    if (CompareCaseInsensitive(hash, std::string(expected_hash)) != 0) {
      logger.Log(LogLevel::Warning,
                 fmt::format("{}: hash mismatch for {}: got {}, expected {}",
                             ep_name, filename, hash, expected_hash));
      return false;
    }
  }

  return true;
}

void PrependDirToProcessPath([[maybe_unused]] const std::filesystem::path& dir) {
#ifdef _WIN32
  DWORD len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  std::wstring prev_path;
  if (len > 0) {
    prev_path.resize(len);
    GetEnvironmentVariableW(L"PATH", prev_path.data(), len);
    prev_path.resize(len - 1);  // remove trailing null
  }

  std::wstring new_path = dir.wstring() + L";" + prev_path;
  SetEnvironmentVariableW(L"PATH", new_path.c_str());
#endif
}

std::vector<std::filesystem::path> SelectEpBundleDependenciesToPreload(
    const std::filesystem::path& bin_dir,
    const EpBundleManifest& manifest) {
  std::vector<std::filesystem::path> dependencies;
  const auto provider_filename = std::filesystem::path(manifest.provider_relative_path).filename().string();

  for (const auto& artifact : manifest.artifacts) {
    for (const auto& file : artifact.extracted_files) {
      const auto path = std::filesystem::absolute(bin_dir / file.relative_path);

      if (CompareCaseInsensitive(path.extension().string(), ".dll") != 0 ||
          CompareCaseInsensitive(path.filename().string(), provider_filename) == 0 ||
          IsCoreRuntimeLibrary(path.filename())) {
        continue;
      }

      dependencies.push_back(path);
    }
  }

  return dependencies;
}

bool LoadEpBundleDependencies(
    [[maybe_unused]] const std::filesystem::path& bin_dir,
    [[maybe_unused]] const EpBundleManifest& manifest,
    [[maybe_unused]] std::string_view ep_name,
    [[maybe_unused]] ILogger& logger) {
#ifdef _WIN32
  for (const auto& path : SelectEpBundleDependenciesToPreload(bin_dir, manifest)) {
    if (!LoadLibraryExW(path.c_str(), nullptr,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      logger.Log(LogLevel::Warning,
                 fmt::format("{}: failed to load dependency '{}' ({})",
                             ep_name, path.string(), GetLastError()));
      return false;
    }
  }
#endif
  // Preloading is a Windows-specific concern (LoadLibraryExW search-path flags); other platforms rely
  // on RPATH/PATH-style resolution and don't need this step.

  return true;
}

}  // namespace fl
