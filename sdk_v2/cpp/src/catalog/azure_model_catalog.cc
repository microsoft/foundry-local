// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_model_catalog.h"
#include "catalog/catalog_cache.h"
#include "catalog/catalog_client.h"
#include "catalog/local_model_scanner.h"
#include "model.h"
#include "model_info.h"
#include "telemetry/invocation_context.h"
#include "telemetry/telemetry.h"
#include "utils.h"

#include <foundry_local/foundry_local_c.h>
#include <fmt/format.h>

#include <algorithm>
#include <utility>

namespace fl {

namespace {

/// Split a catalog URL into structured telemetry dimensions. The Azure Foundry
/// catalog URL looks like "https://ai.azure.com/api/<region>/<format...>", e.g.
/// "https://ai.azure.com/api/eastus/ux/v1.0" -> {ai.azure.com, eastus, ux/v1.0}.
/// Custom URLs that don't follow the "/api/<region>/" convention keep an empty
/// region and put the whole path in `format`. The embedded snapshot is "static".
struct ParsedCatalogUrl {
  std::string endpoint;
  std::string region;
  std::string format;
};

ParsedCatalogUrl ParseCatalogUrl(const std::string& url) {
  if (url == "static") {
    return {"static", "", ""};
  }

  std::string rest = url;
  if (auto scheme = rest.find("://"); scheme != std::string::npos) {
    rest = rest.substr(scheme + 3);
  }

  ParsedCatalogUrl out;
  std::string path;
  if (auto slash = rest.find('/'); slash == std::string::npos) {
    out.endpoint = rest;
  } else {
    out.endpoint = rest.substr(0, slash);
    path = rest.substr(slash + 1);
  }

  if (auto q = path.find_first_of("?#"); q != std::string::npos) {
    path = path.substr(0, q);
  }

  std::vector<std::string> segments;
  size_t pos = 0;
  while (pos < path.size()) {
    auto next = path.find('/', pos);
    if (next == std::string::npos) {
      next = path.size();
    }
    if (next > pos) {
      segments.push_back(path.substr(pos, next - pos));
    }
    pos = next + 1;
  }

  size_t format_start = 0;
  if (segments.size() >= 2 && segments[0] == "api") {
    out.region = segments[1];
    format_start = 2;
  }
  for (size_t i = format_start; i < segments.size(); ++i) {
    if (!out.format.empty()) {
      out.format += '/';
    }
    out.format += segments[i];
  }

  return out;
}

}  // namespace

AzureModelCatalog::AzureModelCatalog(std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
                                     std::string cache_dir,
                                     ModelFactory model_factory,
                                     const IEpDetector& ep_detector,
                                     ILogger& logger,
                                     bool cache_only,
                                     std::string catalog_region,
                                     bool disable_region_fallback,
                                     ITelemetry* telemetry)
    : BaseModelCatalog(catalog_urls.empty() ? kDefaultCatalogUrl : catalog_urls.front().first, logger),
      catalog_urls_(std::move(catalog_urls)),
      cache_dir_(std::move(cache_dir)),
      model_factory_(std::move(model_factory)),
      ep_detector_(ep_detector),
      logger_(logger),
      cache_only_(cache_only),
      catalog_region_(std::move(catalog_region)),
      disable_region_fallback_(disable_region_fallback),
      telemetry_(telemetry) {
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

  // One correlation id groups every catalog access made by this refresh.
  const std::string correlation_id = MakeGuidV4Hex();

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

    auto parsed = ParseCatalogUrl(url);
    CatalogFetchInfo base_info;
    base_info.endpoint = parsed.endpoint;
    base_info.region = parsed.region;
    base_info.format = parsed.format;
    base_info.correlation_id = correlation_id;

    auto model_infos = FetchAllModelInfosWithCachedModels(*client, cached_model_ids, logger_,
                                                          telemetry_, &base_info);

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
