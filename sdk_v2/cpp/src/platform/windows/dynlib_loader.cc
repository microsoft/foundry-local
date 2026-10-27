// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "platform/dynlib_loader.h"
#include "logger.h"

#include <fmt/format.h>

#include <memory>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fl::platform {
namespace {

constexpr DWORD kLibrarySearchFlags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;

std::shared_ptr<void> OwnLibrary(HMODULE handle) {
  return std::shared_ptr<void>(handle, [](void* h) { ::FreeLibrary(static_cast<HMODULE>(h)); });
}

std::string PathForLog(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

std::shared_ptr<void> LoadSharedLibraryImpl(const std::filesystem::path& path, ILogger& logger, bool path_fallback) {
  const auto absolute_path = std::filesystem::absolute(path).lexically_normal();
  HMODULE handle = ::LoadLibraryExW(absolute_path.c_str(), nullptr, kLibrarySearchFlags);
  if (handle == nullptr && path_fallback && ::GetLastError() == ERROR_MOD_NOT_FOUND) {
    handle = ::LoadLibraryExW(absolute_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  }

  if (handle == nullptr) {
    const DWORD error = ::GetLastError();
    logger.Log(LogLevel::Warning,
               fmt::format("EP: failed to load '{}' ({})", PathForLog(absolute_path), error));
    return {};
  }

  return OwnLibrary(handle);
}

}  // namespace

bool SetDynamicLibrarySearchDirectory(const std::filesystem::path& directory, ILogger& logger) {
  const auto absolute_directory = std::filesystem::absolute(directory).lexically_normal();
  if (SetDllDirectoryW(absolute_directory.c_str())) {
    return true;
  }

  logger.Log(LogLevel::Warning,
             fmt::format("Failed to set DLL search directory '{}' ({})", absolute_directory.string(), GetLastError()));
  return false;
}

std::shared_ptr<void> LoadSharedLibrary(const std::filesystem::path& path, ILogger& logger) {
  return LoadSharedLibraryImpl(path, logger, false);
}

std::shared_ptr<void> LoadSharedLibraryWithPathFallback(const std::filesystem::path& path, ILogger& logger) {
  return LoadSharedLibraryImpl(path, logger, true);
}

}  // namespace fl::platform
