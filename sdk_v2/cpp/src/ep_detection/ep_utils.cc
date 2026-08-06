// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_utils.h"

#ifdef _WIN32
#include "logger.h"

#include <fmt/format.h>

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fl {

#ifdef _WIN32
EpBundleSearchPathOwner::~EpBundleSearchPathOwner() {
  for (auto it = cookies_.rbegin(); it != cookies_.rend(); ++it) {
    RemoveDllDirectory(*it);
  }
}

bool EpBundleSearchPathOwner::Add(const std::filesystem::path& directory, std::string_view ep_name, ILogger& logger) {
  const auto absolute_directory = std::filesystem::absolute(directory).lexically_normal();
  if (std::find(directories_.begin(), directories_.end(), absolute_directory) != directories_.end()) {
    return true;
  }

  auto* cookie = AddDllDirectory(absolute_directory.c_str());
  if (cookie == nullptr) {
    logger.Log(LogLevel::Warning, fmt::format("{}: failed to add DLL search directory '{}' ({})", ep_name,
                                              absolute_directory.string(), GetLastError()));
    return false;
  }
  directories_.push_back(absolute_directory);
  cookies_.push_back(cookie);
  cookies_.push_back(cookie);
  return true;
}
#endif

}  // namespace fl
