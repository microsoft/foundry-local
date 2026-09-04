// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_model_catalog.h"
#include "catalog/catalog_cache.h"
#include "catalog/catalog_client.h"
#include "catalog/local_model_scanner.h"
#include "model.h"
#include "model_info.h"

#include <foundry_local/foundry_local_c.h>
#include <fmt/format.h>

#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace fl {

namespace {

std::vector<ModelInfo> DeduplicateByModelId(std::vector<ModelInfo> model_infos) {
  std::vector<ModelInfo> deduplicated;
  deduplicated.reserve(model_infos.size());

  std::unordered_set<std::string> model_ids;
  for (auto& info : model_infos) {
    if (model_ids.insert(info.model_id).second) {
      deduplicated.push_back(std::move(info));
    }
  }

  return deduplicated;
}

void RemoveLegacyLocalEntries(std::vector<ModelInfo>& model_infos) {
  std::erase_if(model_infos, [](const auto& info) {
    const auto* provider = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR);
    return provider && *provider == "Local";
  });
}

}  // namespace

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

std::unique_ptr<ICatalogClient> AzureModelCatalog::CreateCatalogClient(const std::string& url,
                                                                       const std::string& filter) const {
  return MakeCatalogClient(url, filter, ep_detector_, logger_, cache_dir_, catalog_region_, disable_region_fallback_);
}

AzureModelCatalog::CatalogResult AzureModelCatalog::GetLiveCatalogOrLocalSnapshot(
    const std::vector<std::string>& cached_model_ids) const {
  if (!cache_only_) {
    std::vector<ModelInfo> live_model_infos;
    bool any_url_succeeded = false;

    for (const auto& [url, filter] : catalog_urls_) {
      try {
        auto client = CreateCatalogClient(url, filter.value_or(""));
        auto model_infos = FetchAllModelInfosWithCachedModels(*client, cached_model_ids, logger_);
        any_url_succeeded = true;

        live_model_infos.insert(live_model_infos.end(), std::make_move_iterator(model_infos.begin()),
                                std::make_move_iterator(model_infos.end()));
      } catch (const std::exception& ex) {
        logger_.Log(LogLevel::Error, fmt::format("failed to fetch catalog from {}: {}", url, ex.what()));
      } catch (...) {
        logger_.Log(LogLevel::Error, fmt::format("failed to fetch catalog from {}: unknown error", url));
      }
    }

    if (any_url_succeeded) {
      return {
          .model_infos = DeduplicateByModelId(std::move(live_model_infos)),
          .source = CatalogSource::kLive,
      };
    }
  }

  CatalogCache cache(cache_dir_, logger_);
  cache.Load();
  auto cached = cache.GetCachedModels();
  auto snapshot_model_infos = cached ? std::move(*cached) : std::vector<ModelInfo>{};
  RemoveLegacyLocalEntries(snapshot_model_infos);

  return {
      .model_infos = DeduplicateByModelId(std::move(snapshot_model_infos)),
      .source = CatalogSource::kSnapshot,
  };
}

std::vector<Model> AzureModelCatalog::CreateModelsWithLocalPaths(const std::vector<ModelInfo>& model_infos,
                                                                const LocalModels& local_models) const {
  std::vector<Model> models;
  models.reserve(model_infos.size());

  for (const auto& info : model_infos) {
    auto local_model = local_models.find(info.model_id);
    auto local_path = local_model != local_models.end() ? local_model->second : std::string{};
    models.push_back(model_factory_(ModelInfo(info), std::move(local_path)));
  }

  return models;
}

std::vector<Model> AzureModelCatalog::FetchModels() const {
  logger_.Log(LogLevel::Information, "Getting catalog metadata and locally cached models.");

  auto local_models = ScanLocalModels(cache_dir_, logger_);
  std::vector<std::string> cached_model_ids;
  cached_model_ids.reserve(local_models.size());
  for (const auto& local_model : local_models) {
    cached_model_ids.push_back(local_model.first);
  }

  logger_.Log(LogLevel::Information, fmt::format("Found {} locally cached models.", cached_model_ids.size()));

  auto catalog_result = GetLiveCatalogOrLocalSnapshot(cached_model_ids);
  auto models = CreateModelsWithLocalPaths(catalog_result.model_infos, local_models);

  logger_.Log(LogLevel::Information, fmt::format("Populated model info for {} models.", models.size()));

  if (catalog_result.source == CatalogSource::kLive && !catalog_result.model_infos.empty()) {
    CatalogCache cache(cache_dir_, logger_);
    cache.Save(catalog_result.model_infos);
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
      auto client = CreateCatalogClient(url, filter.value_or(""));
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
      auto client = CreateCatalogClient(url, filter.value_or(""));
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
