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

}  // namespace fl
