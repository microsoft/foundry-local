// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "logger.h"
#include "model_info.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

/// Parse a catalog snapshot JSON document (the same format produced by
/// `CatalogCache::Save`) into a vector of `ModelInfo`. Returns nullopt if the
/// document is missing, has an unsupported version, or is malformed. Logs a
/// warning on failure but does not throw.
///
/// `source_description` is included in log messages to help identify which
/// snapshot source failed (e.g., a file path or "embedded snapshot").
std::optional<std::vector<ModelInfo>> ParseCatalogSnapshot(
    std::string_view json,
    std::string_view source_description,
    ILogger& logger);

/// Best-effort disk cache for catalog model information.
/// Caches the model list to `foundry.modelinfo.json` (or a per-catalog variant) in the
/// specified directory. All operations are no-throw — cache failures are logged and silently ignored.
class CatalogCache {
 public:
  /// Default metadata snapshot file name, used for the built-in default ("public") catalog.
  static constexpr const char* kDefaultCacheFileName = "foundry.modelinfo.json";

  /// Construct with the directory where the cache file will be stored. Separately addressable
  /// catalogs share one model-blob cache directory but must not share their metadata snapshot,
  /// so callers pass a per-catalog `cache_file_name` to keep the snapshots isolated.
  explicit CatalogCache(std::string cache_directory, ILogger& logger,
                        std::string cache_file_name = kDefaultCacheFileName);

  /// Load cached models from disk into memory. Silently handles missing/corrupt files.
  void Load();

  /// Save models to disk. Skips if the existing cache was saved less than 4 hours ago.
  void Save(const std::vector<ModelInfo>& models);

  /// Return cached models, or nullopt if no cache is available.
  std::optional<std::vector<ModelInfo>> GetCachedModels() const;

 private:
  std::string CacheFilePath() const;

  std::string cache_directory_;
  std::string cache_file_name_;
  std::optional<std::vector<ModelInfo>> cached_models_;
  ILogger& logger_;

  static constexpr auto kFreshnessThreshold = std::chrono::hours(4);
  static constexpr int kCacheVersion = 1;
};

}  // namespace fl
