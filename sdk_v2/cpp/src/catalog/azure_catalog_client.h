// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "catalog/azure_catalog_models.h"
#include "catalog/catalog_client.h"
#include "ep_detection/ep_detector.h"
#include "http/http_client.h"
#include "logger.h"
#include "model_info.h"
#include "util/region_fallback.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fl {

/// Live Azure Foundry catalog client. Queries the catalog REST API
/// (`{base_url}/entities/crossRegion`) for the models available to the local
/// hardware, paginating through results and converting each entry to ModelInfo.
///
/// Uses one filter set per detected device, page size 50, and pagination via
/// skip + continuationToken.
class AzureCatalogClient : public ICatalogClient {
 public:
  /// Response-aware HTTP POST. Used for region detection, catalog fetches, and regional fallback.
  using HttpPostResponseFn =
      std::function<http::HttpResponse(const std::string& url, const std::string& body)>;

  /// @param base_url Catalog base URL, e.g. "https://ai.azure.com/api/centralus/ux/v1.0".
  /// @param filter_override Foundry Local tag filter. "" means public models; "''" means a single empty value.
  /// @param ep_detector Reports available device and execution-provider pairs.
  /// @param logger Logger.
  /// @param http_post HTTP POST implementation. The default uses `http::HttpPostWithResponse`.
  /// @param catalog_region Catalog region. Empty or "auto" detects from Azure headers; any other value is explicit.
  /// @param region_fallback_enabled Enables retries through nearby regions when a regional endpoint is unhealthy.
  AzureCatalogClient(const std::string& base_url,
                     const std::string& filter_override,
                     const IEpDetector& ep_detector,
                     ILogger& logger,
                     HttpPostResponseFn http_post = {},
                     std::string catalog_region = "",
                     bool region_fallback_enabled = true);

  /// Fetch every catalog model entry visible to the local hardware (raw form,
  /// before conversion to ModelInfo). One filter set per device, fully paginated.
  std::vector<CatalogLocalModel> FetchAllModels();

  std::vector<ModelInfo> FetchAllModelInfos() override;

  std::vector<ModelInfo> FetchModelsByIds(const std::vector<std::string>& model_ids) override;

  /// Fetch every version of `model_alias` from the live catalog by issuing the
  /// per-device search with `labels=latest` removed and an alias filter added.
  /// `model_alias` must be non-empty. Optionally filters by `model_name`.
  /// The client fetches all matching versions, while the caller applies
  /// `max_versions` semantics (latest X per variant).
  std::vector<ModelInfo> FetchAllVersionsByAlias(const std::string& model_alias,
                                                 const std::string& model_name = "",
                                                 int max_versions = 0) override;

 private:
  struct FetchedFilterSet {
    std::vector<CatalogLocalModel> models;
    std::string region;
  };

  /// Run all pages of one filter set. In regional mode the first page goes through region fallback and later pages are
  /// pinned to the serving region; a retryable region-health failure fails just this filter set (others continue).
  std::optional<FetchedFilterSet> FetchFilterSet(const std::vector<CatalogFilter>& filters);

  /// Fetch every device filter set, dropping the ones that failed their region-health checks.
  /// Used for unbounded "latest only" / by-id queries.
  std::vector<FetchedFilterSet> FetchAllFilterSets();

  std::string base_url_;
  std::vector<std::string> model_filter_;  // foundryLocal tag values
  const IEpDetector& ep_detector_;
  ILogger& logger_;
  HttpPostResponseFn http_post_response_;
  RegionFallback region_fallback_;

  // Region state. region_ is the active region (empty = use base_url verbatim).
  // When base_url matches the https://{host}/api/{region}/{suffix} template,
  // url_prefix_/url_suffix_ let us synthesize per-region URLs.
  std::string region_;
  std::string url_prefix_;
  std::string url_suffix_;
  bool regional_template_ = false;
};

}  // namespace fl
