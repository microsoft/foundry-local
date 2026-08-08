// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// POSIX shared-library loader — covers Linux and macOS.
// Android is excluded at the build-system level (see CMakeLists.txt), so this
// translation unit is never compiled for Android targets.
#include "platform/dynlib_loader.h"
#include "logger.h"

#include <dlfcn.h>
#include <fmt/format.h>

namespace fl::platform {

std::shared_ptr<void> LoadSharedLibrary(const std::filesystem::path& path, fl::ILogger& logger) {
  dlerror();
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    const char* error = dlerror();
    logger.Log(LogLevel::Warning,
               fmt::format("EP: failed to load '{}' ({})", path.string(), error ? error : "unknown error"));
    return {};
  }

  return std::shared_ptr<void>(handle, [](void* h) {
    if (h != nullptr) {
      dlclose(h);
    }
  });
}

}  // namespace fl::platform
