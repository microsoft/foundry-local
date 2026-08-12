// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_utils.h"

#include "logger.h"
#include "util/sha256.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fl {

bool VerifyEpPackage(
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
    if (!std::equal(hash.begin(), hash.end(), expected_hash.begin(), expected_hash.end(),
                    [](char a, char b) { return std::toupper(a) == std::toupper(b); })) {
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

bool PreloadAbsoluteDll([[maybe_unused]] const std::filesystem::path& dll_path,
                        [[maybe_unused]] ILogger& logger) {
#ifdef _WIN32
  HMODULE module = ::LoadLibraryExW(
      dll_path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

  if (!module) {
    DWORD load_err = ::GetLastError();
    logger.Log(LogLevel::Warning,
               fmt::format("PreloadAbsoluteDll: LoadLibraryExW failed for '{}' (GetLastError={})",
                           dll_path.string(), load_err));
    return false;
  }

  // Retain the handle for the lifetime of the process. Callers depend on the module staying
  // mapped long after this function returns (e.g. GenAI resolves it by name at model-load time),
  // so we deliberately never call FreeLibrary. Guard the static registry with a mutex since
  // bootstrappers can run preload calls from multiple threads.
  static std::mutex preloaded_modules_mutex;
  static std::vector<HMODULE> preloaded_modules;
  std::lock_guard<std::mutex> lock(preloaded_modules_mutex);
  preloaded_modules.push_back(module);

  return true;
#else
  return false;
#endif
}

}  // namespace fl