// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_utils.h"

#include "logger.h"
#include "util/string_utils.h"

#include <fmt/format.h>

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

std::vector<std::filesystem::path> SelectEpBundleDependenciesToPreload(const std::filesystem::path& bin_dir,
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

EpBundleDependencyOwner::~EpBundleDependencyOwner() {
#ifdef _WIN32
  for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) {
    FreeLibrary(static_cast<HMODULE>(*it));
  }
#endif
}

bool EpBundleDependencyOwner::Load([[maybe_unused]] const std::filesystem::path& bin_dir,
                                   [[maybe_unused]] const EpBundleManifest& manifest,
                                   [[maybe_unused]] std::string_view ep_name, [[maybe_unused]] ILogger& logger) {
#ifdef _WIN32
  const auto dependencies = SelectEpBundleDependenciesToPreload(bin_dir, manifest);
  handles_.reserve(handles_.size() + dependencies.size());

  std::vector<void*> loaded;
  loaded.reserve(dependencies.size());

  for (const auto& path : dependencies) {
    auto* handle =
        LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (handle == nullptr) {
      logger.Log(LogLevel::Warning,
                 fmt::format("{}: failed to load dependency '{}' ({})", ep_name, path.string(), GetLastError()));

      for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
        FreeLibrary(static_cast<HMODULE>(*it));
      }

      return false;
    }

    loaded.push_back(handle);
  }

  handles_.insert(handles_.end(), loaded.begin(), loaded.end());
#endif

  return true;
}

}  // namespace fl
