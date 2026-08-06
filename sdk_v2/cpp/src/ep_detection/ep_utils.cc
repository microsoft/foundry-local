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
namespace {

std::filesystem::path NormalizeDirectory(const std::filesystem::path& directory) {
  return std::filesystem::absolute(directory).lexically_normal();
}

}  // namespace

EpBundleSearchPathOwner::~EpBundleSearchPathOwner() {
  for (auto it = cookies_.rbegin(); it != cookies_.rend(); ++it) {
    if (*it != nullptr) {
      RemoveDllDirectory(*it);
    }
  }
}

bool EpBundleSearchPathOwner::Owns(const std::filesystem::path& directory) const {
  const auto absolute_directory = NormalizeDirectory(directory);
  return std::find(directories_.begin(), directories_.end(), absolute_directory) != directories_.end();
}

bool EpBundleSearchPathOwner::Add(const std::filesystem::path& directory, std::string_view ep_name, ILogger& logger) {
  const auto absolute_directory = NormalizeDirectory(directory);
  if (Owns(absolute_directory)) {
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
  return true;
}

void EpBundleSearchPathOwner::MergeFrom(EpBundleSearchPathOwner&& other) noexcept {
  for (size_t i = 0; i < other.directories_.size() && i < other.cookies_.size(); ++i) {
    if (other.cookies_[i] == nullptr) {
      continue;
    }

    if (Owns(other.directories_[i])) {
      RemoveDllDirectory(other.cookies_[i]);
    } else {
      directories_.push_back(std::move(other.directories_[i]));
      cookies_.push_back(other.cookies_[i]);
    }

    other.cookies_[i] = nullptr;
  }

  other.directories_.clear();
  other.cookies_.clear();
}
#endif

}  // namespace fl
