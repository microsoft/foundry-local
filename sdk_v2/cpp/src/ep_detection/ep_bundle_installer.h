// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_bootstrapper.h"
#include "ep_detection/ep_bundle_manifest.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace fl {

class FileLock;
class ILogger;

using EpArtifactDownloadFn = std::function<bool(const std::string& url,
                                                const std::filesystem::path& destination,
                                                uint64_t max_bytes,
                                                std::atomic<bool>* cancel_flag,
                                                const std::function<void(float)>& progress_cb,
                                                ILogger& logger)>;

enum class EpBundleInstallPolicy {
  ReuseVerified,
  ForceDownload,
};

class EpInstallTransaction {
 public:
  ~EpInstallTransaction();

  EpInstallTransaction(const EpInstallTransaction&) = delete;
  EpInstallTransaction& operator=(const EpInstallTransaction&) = delete;
  EpInstallTransaction(EpInstallTransaction&&) = delete;
  EpInstallTransaction& operator=(EpInstallTransaction&&) = delete;

  const std::filesystem::path& bin_dir() const { return bin_dir_; }

  const std::string& bundle_id() const { return manifest_.bundle_id; }

  bool CommitActive(ILogger& logger);

 private:
  friend class EpBundleInstaller;

  EpInstallTransaction(std::unique_ptr<FileLock> lock, std::filesystem::path root_dir,
                       std::string ep_display_name, EpBundleManifest manifest, std::string generation_id,
                       std::filesystem::path bin_dir);

  std::unique_ptr<FileLock> lock_;
  std::filesystem::path root_dir_;
  std::string ep_display_name_;
  EpBundleManifest manifest_;
  std::string generation_id_;
  std::filesystem::path bin_dir_;
  bool committed_ = false;
};

class EpBundleInstaller {
 public:
  EpBundleInstaller(std::filesystem::path root_dir,
                    std::string lock_file_name,
                    std::string ep_display_name,
                    EpArtifactDownloadFn download_fn = nullptr);

  std::unique_ptr<EpInstallTransaction> EnsureInstalled(const EpBundleManifest& manifest,
                                                        const IEpBootstrapper::ProgressCallback& progress_cb,
                                                        ILogger& logger,
                                                        EpBundleInstallPolicy policy =
                                                            EpBundleInstallPolicy::ReuseVerified);

 private:
  std::filesystem::path root_dir_;
  std::string lock_file_name_;
  std::string ep_display_name_;
  EpArtifactDownloadFn download_fn_;
};

}  // namespace fl
