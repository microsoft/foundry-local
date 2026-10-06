// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_model_catalog.h"
#include "catalog/azure_catalog_client.h"
#include "catalog/catalog_cache.h"
#include "catalog/catalog_client.h"
#include "catalog/local_model_scanner.h"
#include "model.h"
#include "model_info.h"
#include "telemetry/telemetry.h"
#include "version.h"

#include <foundry_local/foundry_local_c.h>
#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fl {

namespace {

CatalogFetchInfo BuildCatalogFetchInfo(const std::string& url, const std::string& correlation_id) {
  CatalogFetchInfo info;
  info.user_agent = DefaultUserAgent();
  info.correlation_id = TelemetryInternal::SanitizeTelemetryIdentifier(correlation_id);
  if (url.size() > kMaxTelemetryInspectionLength) {
    info.endpoint = "custom";
    return info;
  }

  // Inspect only a bounded view; custom endpoint dimensions must not expose caller-controlled hosts or paths.
  std::string_view rest = url;
  if (auto scheme = rest.find("://"); scheme != std::string::npos) {
    rest.remove_prefix(scheme + 3);
  }
  if (auto query = rest.find_first_of("?#"); query != std::string::npos) {
    rest = rest.substr(0, query);
  }

  std::string_view path;
  if (auto slash = rest.find('/'); slash == std::string::npos) {
    info.endpoint = TelemetryInternal::SanitizeTelemetryValue(rest);
  } else {
    info.endpoint = TelemetryInternal::SanitizeTelemetryValue(rest.substr(0, slash));
    path = rest.substr(slash + 1);
  }
  if (auto at = info.endpoint.rfind('@'); at != std::string::npos) {
    info.endpoint = info.endpoint.substr(at + 1);
  }
  info.endpoint = ToLower(info.endpoint);
  if (info.endpoint != "api.catalog.azureml.ms" || path != "asset-gallery/v1.0/models") {
    info.endpoint = "custom";
    return info;
  }

  // Only the public Azure catalog contributes endpoint dimensions; custom hosts and paths stay private.
  info.format = TelemetryInternal::SanitizeTelemetryCatalogFormat(path);
  return info;
}

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

std::vector<ModelInfo> LimitVersionsPerName(std::vector<ModelInfo> model_infos,
                                            int max_versions) {
  if (max_versions <= 0) {
    return model_infos;
  }

  std::unordered_map<std::string, std::vector<std::size_t>> indices_by_name;
  for (std::size_t index = 0; index < model_infos.size(); ++index) {
    indices_by_name[model_infos[index].name].push_back(index);
  }

  std::vector<bool> selected(model_infos.size(), false);
  for (auto& entry : indices_by_name) {
    auto& indices = entry.second;
    std::sort(indices.begin(), indices.end(), [&model_infos](std::size_t left, std::size_t right) {
      const auto& left_info = model_infos[left];
      const auto& right_info = model_infos[right];
      if (left_info.version != right_info.version) {
        return left_info.version > right_info.version;
      }

      const auto left_created = left_info.GetPropertyWithDefault(
          FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, int64_t{0});
      const auto right_created = right_info.GetPropertyWithDefault(
          FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, int64_t{0});
      if (left_created != right_created) {
        return left_created > right_created;
      }

      return left_info.model_id < right_info.model_id;
    });

    const auto count = std::min(indices.size(), static_cast<std::size_t>(max_versions));
    for (std::size_t index = 0; index < count; ++index) {
      selected[indices[index]] = true;
    }
  }

  std::vector<ModelInfo> limited;
  limited.reserve(model_infos.size());
  for (std::size_t index = 0; index < model_infos.size(); ++index) {
    if (selected[index]) {
      limited.push_back(std::move(model_infos[index]));
    }
  }
  return limited;
}

void RemoveLegacyLocalEntries(std::vector<ModelInfo>& model_infos) {
  std::erase_if(model_infos, [](const auto& info) {
    const auto* provider = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR);
    return provider && *provider == "Local";
  });
}

void RemoveIncompatibleModels(std::vector<ModelInfo>& model_infos) {
  std::erase_if(model_infos, [](const ModelInfo& info) {
    const auto* minimum_version =
        info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR);
    return minimum_version && !minimum_version->empty() &&
           !IsFoundryLocalVersionCompatible(FOUNDRY_LOCAL_VERSION, *minimum_version);
  });
}

}  // namespace

AzureModelCatalog::AzureModelCatalog(std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
                                     std::string cache_dir,
                                     ModelFactory model_factory,
                                     const IEpDetector& ep_detector,
                                     ILogger& logger,
                                     bool cache_only,
                                     ITelemetry& telemetry)
    : BaseModelCatalog(catalog_urls.empty() ? kDefaultCatalogUrl : catalog_urls.front().first, logger),
      catalog_urls_(std::move(catalog_urls)),
      cache_dir_(std::move(cache_dir)),
      model_factory_(std::move(model_factory)),
      ep_detector_(ep_detector),
      logger_(logger),
      cache_only_(cache_only),
      telemetry_(telemetry) {
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
  return MakeCatalogClient(url, filter, ep_detector_, logger_, cache_dir_);
}

AzureModelCatalog::CatalogResult AzureModelCatalog::GetLiveCatalogOrLocalSnapshot(
    const std::vector<std::string>& cached_model_ids) const {
  if (!cache_only_) {
    std::vector<ModelInfo> live_model_infos;
    bool any_url_succeeded = false;
    const auto correlation_id = GenerateGuidV4();

    for (const auto& [url, filter] : catalog_urls_) {
      try {
        auto client = CreateCatalogClient(url, filter.value_or(""));
        const auto telemetry_info = BuildCatalogFetchInfo(url, correlation_id);
        auto model_infos =
            FetchAllModelInfosWithCachedModels(*client, cached_model_ids, logger_, telemetry_, telemetry_info);
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
  RemoveIncompatibleModels(catalog_result.model_infos);
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
    const std::string& model_name,
    int max_versions) const {
  std::vector<ModelInfo> model_infos;
  if (cache_only_) {
    // In cache-only mode we have no remote source to query for older versions.
    logger_.Log(LogLevel::Debug,
                "FetchModelVersions skipped: catalog is in cache-only mode.");
    return {};
  }

  for (const auto& [url, filter] : catalog_urls_) {
    try {
      auto client = CreateCatalogClient(url, filter.value_or(""));
      auto fetched = client->FetchAllVersionsByAlias(model_alias, model_name, max_versions);
      RemoveIncompatibleModels(fetched);
      model_infos.insert(model_infos.end(), std::make_move_iterator(fetched.begin()),
                         std::make_move_iterator(fetched.end()));
    } catch (const std::exception& ex) {
      logger_.Log(LogLevel::Error,
                  fmt::format("FetchModelVersions: failed to query {} — {}", url, ex.what()));
    }
  }

  model_infos = DeduplicateByModelId(std::move(model_infos));
  model_infos = LimitVersionsPerName(std::move(model_infos), max_versions);
  std::vector<Model> out;
  out.reserve(model_infos.size());
  for (auto& info : model_infos) {
    out.push_back(model_factory_(std::move(info), /*local_path=*/""));
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
      RemoveIncompatibleModels(model_infos);

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
