// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_bundle_installer.h"
#include "util/sha256.h"
#include "utils/temp_path.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fl::test {

inline std::vector<uint8_t> AsBytes(std::string_view text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

inline std::string HashOf(const std::vector<uint8_t>& bytes) {
  auto tmp = TempPath::CreateTempFile("fl_ep_bundle_hash_");
  std::ofstream out(tmp.path(), std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  return Sha256File(tmp.path());
}

inline std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class FakeDownloads {
 public:
  void SetSequence(const std::string& url, std::vector<std::vector<uint8_t>> payloads) {
    payloads_[url] = std::move(payloads);
  }

  int CallCount(const std::string& url) const {
    const auto it = call_counts_.find(url);
    return it == call_counts_.end() ? 0 : it->second;
  }

  EpArtifactDownloadFn AsFn() {
    return [this](const std::string& url, const std::filesystem::path& destination, uint64_t /*max_bytes*/,
                  std::atomic<bool>* cancel_flag, const std::function<void(float)>& progress_cb,
                  ILogger& /*logger*/) -> bool {
      const auto it = payloads_.find(url);
      if (it == payloads_.end() || it->second.empty()) {
        return false;
      }

      if (progress_cb) {
        progress_cb(0.0f);
      }

      if (cancel_flag && cancel_flag->load()) {
        return false;
      }

      int& count = call_counts_[url];
      const size_t index = std::min(static_cast<size_t>(count), it->second.size() - 1);
      const auto& bytes = it->second[index];
      count++;

      std::filesystem::create_directories(destination.parent_path());
      std::ofstream out(destination, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      out.close();

      if (progress_cb) {
        progress_cb(100.0f);
      }

      return true;
    };
  }

 private:
  std::map<std::string, std::vector<std::vector<uint8_t>>> payloads_;
  std::map<std::string, int> call_counts_;
};

inline std::optional<std::filesystem::path> InstallAndFinalize(EpBundleInstaller& installer,
                                                               const EpBundleManifest& manifest, ILogger& logger) {
  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  if (!txn) {
    return std::nullopt;
  }

  const auto bin_dir = txn->bin_dir();
  if (!txn->Activate()) {
    return std::nullopt;
  }

  txn->Finalize();
  return bin_dir;
}

inline std::optional<std::filesystem::path> InstallAndFinalize(const std::filesystem::path& root,
                                                               std::string lock_file_name, std::string display_name,
                                                               const EpBundleManifest& manifest,
                                                               EpArtifactDownloadFn download_fn, ILogger& logger) {
  EpBundleInstaller installer(root, std::move(lock_file_name), std::move(display_name), std::move(download_fn));
  return InstallAndFinalize(installer, manifest, logger);
}

inline std::optional<std::string> FindGenerationWithPrefix(const std::filesystem::path& bundles_dir,
                                                           std::string_view prefix) {
  for (const auto& entry : std::filesystem::directory_iterator(bundles_dir)) {
    const auto generation = entry.path().filename().string();
    if (generation.starts_with(prefix)) {
      return generation;
    }
  }

  return std::nullopt;
}

}  // namespace fl::test
