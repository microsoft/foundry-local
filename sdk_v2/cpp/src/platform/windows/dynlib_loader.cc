// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "platform/dynlib_loader.h"
#include "logger.h"

#include <fmt/format.h>

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

}  // namespace fl::platform
