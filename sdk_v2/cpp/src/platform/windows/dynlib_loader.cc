// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "platform/dynlib_loader.h"
#include "logger.h"

#include <fmt/format.h>

#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fl::platform {

bool SetDynamicLibrarySearchDirectory(const std::filesystem::path& directory, ILogger& logger) {
  const auto absolute_directory = std::filesystem::absolute(directory).lexically_normal();
  if (SetDllDirectoryW(absolute_directory.c_str())) {
    return true;
  }

  logger.Log(LogLevel::Warning,
             fmt::format("Failed to set DLL search directory '{}' ({})", absolute_directory.string(), GetLastError()));
  return false;
}

std::shared_ptr<void> LoadSharedLibrary(const std::filesystem::path& path, fl::ILogger& logger) {
  const auto absolute_path = std::filesystem::absolute(path).lexically_normal();
  // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR lets the library resolve its sibling dependencies (the CUDA
  // runtime + provider DLLs staged alongside it) from its own directory; DEFAULT_DIRS keeps System32
  // and the process directories (where onnxruntime.dll is already resident) on the search path.
  HMODULE handle = ::LoadLibraryExW(absolute_path.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
  if (handle == nullptr) {
    logger.Log(LogLevel::Warning,
               fmt::format("EP: failed to load '{}' ({})", absolute_path.string(), GetLastError()));
    return {};
  }

  return std::shared_ptr<void>(handle, [](void* h) {
    if (h != nullptr) {
      ::FreeLibrary(static_cast<HMODULE>(h));
    }
  });
}

}  // namespace fl::platform
