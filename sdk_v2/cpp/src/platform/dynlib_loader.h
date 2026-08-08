// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <filesystem>
#include <memory>

namespace fl {
class ILogger;
}

namespace fl::platform {

#ifdef _WIN32
/// Set the directory used for subsequent bare-name dynamic library loads.
bool SetDynamicLibrarySearchDirectory(const std::filesystem::path& directory, ILogger& logger);
#else
/// Load one shared library and keep it resident for the lifetime of the returned handle.
std::shared_ptr<void> LoadSharedLibrary(const std::filesystem::path& path, fl::ILogger& logger);
#endif

}  // namespace fl::platform
