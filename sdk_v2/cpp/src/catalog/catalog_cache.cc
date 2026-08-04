// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/catalog_cache.h"

#include <fmt/format.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

#include "utils.h"

#ifdef _WIN32
#include <process.h>
#define FL_GETPID() ::_getpid()
#else
#include <unistd.h>
#define FL_GETPID() ::getpid()
#endif

namespace fl {

namespace fs = std::filesystem;

// Must match CatalogCache::kCacheVersion — kept in sync via static_assert below.
namespace {
constexpr int kSnapshotVersion = 1;
}

std::optional<std::vector<ModelInfo>> ParseCatalogSnapshot(
    std::string_view json,
    std::string_view source_description,
    ILogger& logger) {
  try {
    auto root = nlohmann::json::parse(json);

    if (!root.contains("version") || !root["version"].is_number_integer() ||
        root["version"].get<int>() != kSnapshotVersion) {
      logger.Log(LogLevel::Warning,
                 fmt::format("catalog cache: unsupported version in {}", source_description));
      return std::nullopt;
    }

    if (!root.contains("models") || !root["models"].is_array()) {
      logger.Log(LogLevel::Warning,
                 fmt::format("catalog cache: missing or invalid 'models' in {}", source_description));
      return std::nullopt;
    }

    std::vector<ModelInfo> models;
    models.reserve(root["models"].size());

    for (const auto& item : root["models"]) {
      models.push_back(ModelInfoFromJson(item));
    }

    return models;
  } catch (const std::exception& ex) {
    logger.Log(LogLevel::Warning, fmt::format("catalog cache: failed to load — {}", ex.what()));
    return std::nullopt;
  } catch (...) {
    logger.Log(LogLevel::Warning, "catalog cache: failed to load — unknown error");
    return std::nullopt;
  }
}

CatalogCache::CatalogCache(std::string cache_directory, ILogger& logger, std::string cache_file_name)
    : cache_directory_(std::move(cache_directory)),
      cache_file_name_(std::move(cache_file_name)),
      logger_(logger) {
  static_assert(kSnapshotVersion == CatalogCache::kCacheVersion,
                "snapshot parser version must match cache writer version");
}

std::string CatalogCache::CacheFilePath() const {
  return (fs::path(cache_directory_) / cache_file_name_).string();
}

void CatalogCache::Load() {
  auto path = CacheFilePath();
  if (!fs::exists(path)) {
    return;
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    logger_.Log(LogLevel::Warning, fmt::format("catalog cache: could not open {}", path));
    return;
  }

  // Slurp file contents so the parser sees a string_view — keeps the parsing
  // logic in one place and reusable for embedded snapshots.
  std::string contents((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

  cached_models_ = ParseCatalogSnapshot(contents, path, logger_);
}

void CatalogCache::Save(const std::vector<ModelInfo>& models) {
  try {
    auto path = CacheFilePath();

    // Freshness check: skip save if existing cache file is less than 6 hours old.
    if (fs::exists(path)) {
      try {
        std::ifstream existing_file(path);
        if (existing_file.is_open()) {
          auto existing = nlohmann::json::parse(existing_file);

          if (existing.contains("savedAtUnix") && existing["savedAtUnix"].is_number_integer()) {
            auto saved_at = std::chrono::system_clock::time_point(
                std::chrono::seconds(existing["savedAtUnix"].get<int64_t>()));
            auto now = std::chrono::system_clock::now();

            if ((now - saved_at) < kFreshnessThreshold) {
              return;  // Cache is still fresh — skip save.
            }
          }
        }
      } catch (...) {
        // If we can't read the existing file's timestamp, proceed with save.
        logger_.Log(LogLevel::Information,
                    fmt::format("catalog cache: could not read existing cache timestamp in {}, proceeding with save", path));
      }
    }

    Utils::EnsureDirectoryExists(cache_directory_);

    nlohmann::json models_array = nlohmann::json::array();
    for (const auto& info : models) {
      models_array.push_back(ModelInfoToJson(info));
    }

    auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

    nlohmann::json root;
    root["version"] = kCacheVersion;
    root["savedAtUnix"] = now_unix;
    root["models"] = std::move(models_array);

    // Atomic write: stream to a sibling temp file, then rename over the destination.
    // This prevents a crash mid-write from leaving a truncated/corrupt cache that
    // would fail to parse on next startup.
    auto temp_path = path + ".tmp." + std::to_string(FL_GETPID());

    {
      std::ofstream file(temp_path, std::ios::trunc | std::ios::binary);
      if (!file.is_open()) {
        logger_.Log(LogLevel::Warning, fmt::format("catalog cache: could not write {}", temp_path));
        return;
      }

      file << root.dump(2);
      file.close();

      if (file.fail()) {
        std::error_code rm_ec;
        fs::remove(temp_path, rm_ec);
        logger_.Log(LogLevel::Warning,
                    fmt::format("catalog cache: failed to write {} fully", temp_path));
        return;
      }
    }

    std::error_code rename_ec;
    fs::rename(temp_path, path, rename_ec);
    if (rename_ec) {
      std::error_code rm_ec;
      fs::remove(temp_path, rm_ec);
      logger_.Log(LogLevel::Warning,
                  fmt::format("catalog cache: failed to rename {} -> {}: {}",
                              temp_path, path, rename_ec.message()));
      return;
    }

    // Update in-memory cache to match what we just wrote.
    cached_models_ = models;
  } catch (const std::exception& ex) {
    logger_.Log(LogLevel::Warning, fmt::format("catalog cache: failed to save — {}", ex.what()));
  } catch (...) {
    logger_.Log(LogLevel::Warning, "catalog cache: failed to save — unknown error");
  }
}

std::optional<std::vector<ModelInfo>> CatalogCache::GetCachedModels() const {
  return cached_models_;
}

}  // namespace fl
