// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_model_catalog.h"
#include "catalog/catalog_cache.h"
#include "catalog/catalog_client.h"
#include "catalog/local_model_scanner.h"
#include "model.h"
#include "model_info.h"
#include "utils.h"

#include <foundry_local/foundry_local_c.h>
#include <fmt/format.h>

#include <algorithm>
#include <utility>

namespace fl {

AzureModelCatalog::AzureModelCatalog(std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
                                     std::string cache_dir,
                                     ModelFactory model_factory,
                                     const IEpDetector& ep_detector,
                                     ILogger& logger,
                                     bool cache_only,
                                     std::string catalog_region,
                                     bool disable_region_fallback)
    : BaseModelCatalog(catalog_urls.empty() ? kDefaultCatalogUrl : catalog_urls.front().first, logger),
      catalog_urls_(std::move(catalog_urls)),
      cache_dir_(std::move(cache_dir)),
      model_factory_(std::move(model_factory)),
      ep_detector_(ep_detector),
      logger_(logger),
      cache_only_(cache_only),
      catalog_region_(std::move(catalog_region)),
      disable_region_fallback_(disable_region_fallback) {
  if (catalog_urls_.empty()) {
    catalog_urls_.emplace_back(kDefaultCatalogUrl, std::optional<std::string>(kDefaultCatalogFilter));
  }

  logger_.Log(LogLevel::Information,
              fmt::format("Created AzureModelCatalog. Cache directory: {}",
                          cache_dir_));
}

AzureModelCatalog::~AzureModelCatalog() = default;

std::vector<Model> AzureModelCatalog::FetchModels() const {
  // In cache-only mode, read only from the disk cache file — no network calls, no local model scanning.
  // The cache file already includes local models from the last full catalog refresh by the long-running service
  // process.
  // TODO: For our CLI usage the catalog file would be current as we use an ephemeral port for the web service and
  // therefore have to run FL first to acquire the external URL value, and that run would have updated the cached
  // catalog info.
  // If someone had a hardcoded web service URL they were using that sequence of events isn't guaranteed. If we care,
  // we could update 'cache_only_' mode to enable refreshing the cache info if it is old. The cache file has a
  // savedAtUnix timestamp property that can be used.
  if (cache_only_) {
    CatalogCache cache(cache_dir_, logger_);
    cache.Load();
    auto cached = cache.GetCachedModels();

    std::vector<Model> models;

    if (cached) {
      for (const auto& info : *cached) {
        models.push_back(model_factory_(ModelInfo(info), /*local_path=*/""));
      }
    }

    logger_.Log(LogLevel::Information,
                fmt::format("Cache-only mode: populated {} models from cache file.", models.size()));

    return models;
  }

  std::vector<Model> models;
  std::vector<ModelInfo> fetched_infos;
  const std::string& cache_dir = cache_dir_;

  logger_.Log(LogLevel::Information,
              "Getting latest info from the Azure catalog and for locally cached models.");

  // Discover locally cached models.
  auto local_models = ScanLocalModels(cache_dir, logger_);
  std::vector<std::string> cached_model_ids;
  cached_model_ids.reserve(local_models.size());
  for (const auto& [id, path] : local_models) {
    cached_model_ids.push_back(id);
  }

  logger_.Log(LogLevel::Information,
              fmt::format("Found {} locally cached models.", cached_model_ids.size()));

  auto fetch_from = [&](const std::string& url, const std::optional<std::string>& filter) {
    // Preserve byte-identical behavior for the "no override" case (previously stored as ""),
    // while letting callers explicitly request "" as a real filter override.
    auto client = MakeCatalogClient(url, filter.value_or(""), ep_detector_, logger_, cache_dir,
                                    catalog_region_, disable_region_fallback_);
    auto model_infos = FetchAllModelInfosWithCachedModels(*client, cached_model_ids, logger_);

    for (const auto& info : model_infos) {
      // Check if the model is locally cached and pass the path if so.
      std::string local_path;
      auto it = local_models.find(info.model_id);
      if (it != local_models.end()) {
        local_path = it->second;
      }

      fetched_infos.push_back(info);
      models.push_back(model_factory_(ModelInfo(info), std::move(local_path)));
    }
  };

  for (const auto& [url, filter] : catalog_urls_) {
    try {
      fetch_from(url, filter);
    } catch (const std::exception& ex) {
      // One failing URL shouldn't block others — skip and continue.
      logger_.Log(LogLevel::Error,
                  fmt::format("failed to fetch catalog from {}: {}", url, ex.what()));
    }
  }

  logger_.Log(LogLevel::Information,
              fmt::format("Populated model info for {} models.", models.size()));

  // Save the fetched catalog for cache-only mode. This is best-effort: Save handles
  // its own errors and freshness checks. If nothing was fetched, leave the existing
  // cache untouched.
  if (!fetched_infos.empty()) {
    CatalogCache cache(cache_dir_, logger_);
    cache.Save(fetched_infos);
  }

  return models;
}

std::vector<Model> AzureModelCatalog::FetchModelVersions(
    const std::string& model_alias,
    const std::string& model_name) const {
  std::vector<Model> out;
  if (cache_only_) {
    // In cache-only mode we have no remote source to query for older versions.
    logger_.Log(LogLevel::Debug,
                "FetchModelVersions skipped: catalog is in cache-only mode.");
    return out;
  }

  for (const auto& [url, filter] : catalog_urls_) {
    try {
      auto client = MakeCatalogClient(url, filter.value_or(""), ep_detector_, logger_, cache_dir_,
                                      catalog_region_, disable_region_fallback_);
      auto model_infos = client->FetchAllVersionsByAlias(model_alias, model_name);

      out.reserve(out.size() + model_infos.size());
      for (auto& info : model_infos) {
        out.push_back(model_factory_(std::move(info), /*local_path=*/""));
      }
    } catch (const std::exception& ex) {
      logger_.Log(LogLevel::Error,
                  fmt::format("FetchModelVersions: failed to query {} — {}", url, ex.what()));
    }
  }

  logger_.Log(LogLevel::Information,
              fmt::format("FetchModelVersions('{}') returned {} variant(s).",
                          model_alias, out.size()));

  return out;
}

std::vector<Model> AzureModelCatalog::FetchModelsByIds(const std::vector<std::string>& model_ids) const {
  if (model_ids.empty()) {
    return {};
  }

  if (cache_only_) {
    logger_.Log(LogLevel::Debug,
                "FetchModelsByIds skipped: catalog is in cache-only mode.");
    return {};
  }

  auto local_models = ScanLocalModels(cache_dir_, logger_);

  std::vector<Model> models;
  // Track which IDs are still unresolved so we can stop calling further
  // endpoints once everything has been found.
  std::vector<std::string> remaining(model_ids);

  for (const auto& [url, filter] : catalog_urls_) {
    if (remaining.empty()) {
      break;
    }

    try {
      auto client = MakeCatalogClient(url, filter.value_or(""), ep_detector_, logger_, cache_dir_,
                                      catalog_region_, disable_region_fallback_);
      auto model_infos = client->FetchModelsByIds(remaining);

      for (auto& info : model_infos) {
        std::string local_path;
        auto it = local_models.find(info.model_id);
        if (it != local_models.end()) {
          local_path = it->second;
        }

        // Drop this id from the remaining list now that it's resolved.
        auto rit = std::find(remaining.begin(), remaining.end(), info.model_id);
        if (rit != remaining.end()) {
          remaining.erase(rit);
        }

        models.push_back(model_factory_(std::move(info), std::move(local_path)));
      }
    } catch (const std::exception& ex) {
      logger_.Log(LogLevel::Error,
                  fmt::format("FetchModelsByIds: failed to query {} — {}", url, ex.what()));
    }
  }

  return models;
}

}  // namespace fl
