// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "download/blob_downloader.h"
#include "download/model_registry_client.h"
#include "model_info.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace fl {

class ILogger;

/// Orchestrates the full model download flow:
/// 1. Compute local cache path
/// 2. Resolve SAS URI from model registry
/// 3. Download blobs from Azure Storage
/// 4. Write inference_model.json
/// 5. Fix variant download (move inference_model.json into subdirs)
class DownloadManager {
 public:
  /// Construct with the model cache directory path.
  /// @param cache_directory Local directory where models are cached.
  /// @param catalog_region Explicit Azure region override for the model registry endpoint,
  ///        or "auto"/empty to use each model's detected region (falling back to the default registry region).
  /// @param max_concurrency Per-blob chunk parallelism (default 64).
  /// @param logger Logger forwarded to the registry client for retry diagnostics.
  /// @param disable_region_fallback When true, the registry uses a single region attempt
  ///        with no cross-region fallback.
  DownloadManager(std::string cache_directory,
                  std::string_view catalog_region,
                  int max_concurrency,
                  ILogger& logger,
                  bool disable_region_fallback = false);
  ~DownloadManager();

  /// Override the model registry client (for testing).
  void SetModelRegistryClient(std::unique_ptr<ModelRegistryClient> client);

  /// Override the blob downloader (for testing).
  void SetBlobDownloader(std::unique_ptr<IBlobDownloader> downloader);

  /// Download a model to the local cache.
  /// progress_cb reports 0.0 to 100.0 percentage.
  /// Returns the local path where the model was downloaded.
  /// Throws fl::Exception on failure.
  std::string DownloadModel(const ModelInfo& info, std::function<int(float)> progress_cb = nullptr);

  /// Check if a model is cached locally (directory exists and download is complete).
  bool IsModelCached(const ModelInfo& info) const;

  /// Get the local cache path for a model. Returns empty string if not determinable.
  std::string GetModelCachePath(const ModelInfo& info) const;

  /// Get the cache directory.
  const std::string& GetCacheDirectory() const { return cache_directory_; }

  /// Set the cache directory.
  void SetCacheDirectory(const std::string& path) { cache_directory_ = path; }

 private:
  /// Compute the local directory path for a model in the cache.
  /// Uses {cache_dir}/{publisher}/{model_id_with_version_fix}
  std::string ComputeModelPath(const ModelInfo& info) const;

  std::string cache_directory_;
  // Explicit registry region override. Empty (or "auto") means "use the model's
  // detected_region, falling back to default registry region" — set at construction
  // from config.
  std::string config_region_;
  int max_concurrency_;
  ILogger& logger_;
  std::unique_ptr<ModelRegistryClient> registry_client_;
  std::unique_ptr<IBlobDownloader> blob_downloader_;

  /// Serializes all model downloads in this process: only one runs at a time, so
  /// each gets the full network/disk instead of competing with another download.
  /// Cross-process serialization is handled separately by CrossProcessFileLock.
  std::mutex download_mutex_;
};

}  // namespace fl
