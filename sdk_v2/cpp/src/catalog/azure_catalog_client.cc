// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_catalog_client.h"

#include "http/http_client.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace fl {

namespace {

constexpr const char* kEntitiesPath = "/entities/crossRegion";
constexpr int kPageSize = 50;

// Region detection probe.
constexpr const char* kRegionProbeUrl = "https://api.catalog.azureml.ms/asset-gallery/v1.0/models";
constexpr const char* kRegionProbeBody = R"({"filters":[],"pageSize":1})";
constexpr const char* kServedByClusterHeader = "azureml-served-by-cluster";
constexpr const char* kDefaultRegion = "centralus";

// The catalog and registry gateways reject requests without this User-Agent (HTTP 400).
constexpr const char* kUserAgent = "AzureAiStudio";

/// Strip all leading/trailing single quotes.
std::string TrimSingleQuotes(const std::string& s) {
  const auto begin = s.find_first_not_of('\'');
  if (begin == std::string::npos) {
    return {};
  }

  const auto end = s.find_last_not_of('\'');
  return s.substr(begin, end - begin + 1);
}

/// Build the values for the foundryLocal tag filter from the override string.
/// Empty override → {""} (public models). Otherwise split on ',', drop entries
/// that are empty after whitespace-trimming, then strip surrounding quotes so a
/// caller can request an explicit empty value via "''".
std::vector<std::string> CreateModelFilter(const std::string& filter_override) {
  if (filter_override.empty()) {
    return {std::string{}};
  }

  std::vector<std::string> values;
  std::size_t start = 0;
  while (start <= filter_override.size()) {
    const auto comma = filter_override.find(',', start);
    const auto count = (comma == std::string::npos) ? std::string::npos : comma - start;
    const auto entry = Trim(filter_override.substr(start, count));

    if (!entry.empty()) {
      values.push_back(TrimSingleQuotes(entry));
    }

    if (comma == std::string::npos) {
      break;
    }

    start = comma + 1;
  }

  return values;
}

CatalogFilter MakeFilter(std::string field, std::vector<std::string> values) {
  CatalogFilter f;
  f.field = std::move(field);
  f.op = "eq";
  f.values = std::move(values);
  return f;
}

/// Extract the region from an `azureml-served-by-cluster` header value such as
/// "vienna-eastus-01" → "eastus". Returns "" if the value doesn't match.
std::string ExtractRegionFromClusterHeader(const std::string& header_value) {
  static const std::regex pattern(R"(vienna-(\w+)-\d+)");
  std::smatch match;
  if (std::regex_search(header_value, match, pattern)) {
    return match[1].str();
  }

  return {};
}

/// Split a catalog URL of the form `https://{host}/api/{region}/{suffix}` into
/// its prefix ("https://{host}/api/") and suffix ("/{suffix}"). Returns false if
/// the URL doesn't match that shape, in which case it must be used verbatim.
bool TryParseRegionalCatalogUrl(const std::string& url, std::string* prefix, std::string* suffix) {
  static const std::string kApiMarker = "/api/";
  const auto api_pos = url.find(kApiMarker);
  if (api_pos == std::string::npos) {
    return false;
  }

  const auto region_start = api_pos + kApiMarker.size();
  const auto region_end = url.find('/', region_start);
  if (region_end == std::string::npos || region_end == region_start) {
    return false;
  }

  *prefix = url.substr(0, region_start);
  *suffix = url.substr(region_end);
  return true;
}

bool UsesRegionalRouting(bool regional_template, const std::string& region) {
  return regional_template && !region.empty();
}

/// Detect the Azure region by POSTing a probe to the catalog gallery and reading
/// the `azureml-served-by-cluster` response header. Returns "centralus" on failure.
std::string DetectRegion(const AzureCatalogClient::HttpPostResponseFn& http_post_response, ILogger& logger) {
  http::HttpResponse response = http_post_response(kRegionProbeUrl, kRegionProbeBody);

  std::string region = kDefaultRegion;
  if (response.status >= 200 && response.status < 300) {
    auto it = response.headers.find(kServedByClusterHeader);
    if (it != response.headers.end()) {
      auto parsed = ExtractRegionFromClusterHeader(it->second);
      if (!parsed.empty()) {
        region = parsed;
      }
    }
  } else {
    logger.Log(LogLevel::Warning,
               "Region detection probe failed (status " + std::to_string(response.status) + "); defaulting to '" +
                   kDefaultRegion + "'.");
  }

  logger.Log(LogLevel::Information, "Detected catalog region: '" + region + "'.");
  return region;
}

std::string BuildRegionalUrl(const std::string& url_prefix, const std::string& url_suffix, const std::string& region) {
  return url_prefix + region + url_suffix + kEntitiesPath;
}

std::string BuildRequestUrl(const std::string& base_url,
                            bool regional,
                            const std::string& region,
                            const std::string& url_prefix,
                            const std::string& url_suffix) {
  if (regional) {
    return BuildRegionalUrl(url_prefix, url_suffix, region);
  }

  return base_url + kEntitiesPath;
}

std::string BuildRequestBody(const std::vector<CatalogFilter>& filters,
                             const std::optional<int>& skip,
                             const std::optional<std::string>& continuation_token) {
  AzureCatalogRequest request;
  request.resource_ids.push_back({"azureml", "Registry"});
  request.index_entities_request.filters = filters;
  request.index_entities_request.page_size = kPageSize;
  request.index_entities_request.skip = skip;
  request.index_entities_request.continuation_token = continuation_token;

  const nlohmann::json body = request;
  return body.dump();
}

std::vector<ModelInfo> ToModelInfos(const std::vector<CatalogLocalModel>& raw_models, const std::string& region) {
  std::vector<ModelInfo> infos;
  for (const auto& model : raw_models) {
    if (auto info = CatalogModelToModelInfo(model)) {
      info->detected_region = region;
      infos.push_back(std::move(*info));
    }
  }

  return infos;
}

/// Build per-device filter sets for catalog queries.
/// `latest_only` controls whether to include the `labels=latest` filter (default true for latest models).
/// `model_alias` scopes results to a specific alias when non-empty; when empty, no alias filter is applied.
/// `model_name` scopes results to a specific model name when non-empty for server-side filtering.
/// Each filter set queries for variants on a specific device/EP pair; the catalog API matches on the
/// (device, execution provider) pair.
std::vector<std::vector<CatalogFilter>> BuildSearchFilters(
    const IEpDetector& ep_detector,
    const std::vector<std::string>& model_filter,
    bool latest_only = true,
    const std::string& model_alias = "",
    const std::string& model_name = "") {
  std::vector<std::vector<CatalogFilter>> filter_sets;

  for (const auto& [device, eps] : ep_detector.GetAvailableDevicesToEPs()) {
    std::vector<CatalogFilter> filters;
    filters.push_back(MakeFilter("type", {"models"}));
    filters.push_back(MakeFilter("kind", {"Versioned"}));
    if (latest_only) {
      filters.push_back(MakeFilter("labels", {"latest"}));
    }
    filters.push_back(MakeFilter("annotations/tags/foundryLocal", model_filter));
    if (!model_alias.empty()) {
      filters.push_back(MakeFilter("annotations/tags/alias", {model_alias}));
    }
    if (!model_name.empty()) {
      filters.push_back(MakeFilter("properties/name", {model_name}));
    }
    filters.push_back(MakeFilter("properties/variantInfo/variantMetadata/device", {ToLower(device)}));
    filters.push_back(MakeFilter("properties/variantInfo/variantMetadata/executionProvider", eps));
    filter_sets.push_back(std::move(filters));
  }

  return filter_sets;
}


std::vector<CatalogFilter> BuildModelIdFilters(const std::vector<std::string>& model_filter,
                                               const std::vector<std::string>& model_ids) {
  // Looking up specific IDs: no labels=latest (we want exact versions) and no
  // device/EP filters (the IDs already pin the variant).
  std::vector<CatalogFilter> filters;
  filters.push_back(MakeFilter("type", {"models"}));
  filters.push_back(MakeFilter("kind", {"Versioned"}));
  filters.push_back(MakeFilter("annotations/tags/foundryLocal", model_filter));
  filters.push_back(MakeFilter("properties/id", model_ids));
  return filters;
}

}  // namespace

AzureCatalogClient::AzureCatalogClient(const std::string& base_url,
                                       const std::string& filter_override,
                                       const IEpDetector& ep_detector,
                                       ILogger& logger,
                                       HttpPostResponseFn http_post,
                                       std::string catalog_region,
                                       bool region_fallback_enabled)
    : base_url_(base_url),
      model_filter_(CreateModelFilter(filter_override)),
      ep_detector_(ep_detector),
      logger_(logger),
      http_post_response_(std::move(http_post)),
      region_fallback_(logger, region_fallback_enabled) {
  if (!http_post_response_) {
    http_post_response_ = [](const std::string& url, const std::string& body) {
      http::HttpRequestOptions options;
      options.user_agent = kUserAgent;
      return http::HttpPostWithResponse(url, body, options);
    };
  }

  // Normalize away a single trailing slash so URL composition is predictable.
  if (!base_url_.empty() && base_url_.back() == '/') {
    base_url_.pop_back();
  }

  regional_template_ = TryParseRegionalCatalogUrl(base_url_, &url_prefix_, &url_suffix_);

  // An explicit region is a hard override. Empty/"auto" means detect the region,
  // but only for Azure URLs that can be rewritten per region.
  const auto normalized_catalog_region = ToLower(catalog_region);
  if (!normalized_catalog_region.empty() && normalized_catalog_region != "auto") {
    region_ = normalized_catalog_region;
  } else if (regional_template_) {
    region_ = DetectRegion(http_post_response_, logger_);
  }
}

std::optional<AzureCatalogClient::FetchedFilterSet> AzureCatalogClient::FetchFilterSet(
    const std::vector<CatalogFilter>& filters) {
  const bool regional = UsesRegionalRouting(regional_template_, region_);

  FetchedFilterSet result;
  result.region = region_;

  std::optional<int> skip;
  std::optional<std::string> continuation_token;
  std::optional<std::string> pinned_region;  // region that served page 1 (regional mode)

  while (true) {
    const std::string body = BuildRequestBody(filters, skip, continuation_token);

    http::HttpResponse response;
    if (regional && !pinned_region) {
      // Page 1: run through region fallback starting from the sticky region (last known-good) or the active region.
      // Exhaustion means every candidate had a retryable region-health failure, so fail just this filter set.
      const std::string start = region_fallback_.StickyRegion().value_or(region_);
      try {
        auto fallback_result = region_fallback_.Execute(start, [&](const std::string& r) {
          return http_post_response_(BuildRegionalUrl(url_prefix_, url_suffix_, r), body);
        });
        response = std::move(fallback_result.response);
        pinned_region = fallback_result.region;
        result.region = fallback_result.region;
        region_ = fallback_result.region;  // active region biases later filter sets
      } catch (const std::exception& ex) {
        logger_.Log(LogLevel::Warning,
                    std::string("catalog: filter set failed across all regions: ") + ex.what());
        return std::nullopt;
      }
    } else if (regional) {
      // Subsequent pages are pinned to the region that served page 1 — a filter set
      // never mixes regions (continuation tokens are region-specific).
      response = http_post_response_(BuildRegionalUrl(url_prefix_, url_suffix_, *pinned_region), body);
    } else {
      // Non-regional / custom URL: single verbatim attempt, no fallback.
      response = http_post_response_(BuildRequestUrl(base_url_, regional, region_, url_prefix_, url_suffix_), body);
    }

    if (response.status == 0 || response.status < 200 || response.status >= 300) {
      if (regional && IsRegionRetryableStatus(response.status)) {
        // Region-health failure (including mid-pagination on the pinned region): fail this filter set only. Because
        // models are committed atomically below, a later page failure cannot leak a partial filter-set result.
        logger_.Log(LogLevel::Warning,
                    "catalog: filter set failed (" + http::DescribeFailure(response) + "); skipping this filter set.");
        return std::nullopt;
      }

      const std::string url = regional && pinned_region
                                  ? BuildRegionalUrl(url_prefix_, url_suffix_, *pinned_region)
                                  : BuildRequestUrl(base_url_, regional, region_, url_prefix_, url_suffix_);
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK,
               "catalog request to " + url + " failed: " + http::DescribeFailure(response));
    }

    const auto parsed = nlohmann::json::parse(response.body).get<AzureCatalogResponse>();
    if (!parsed.index_entities_response) {
      break;
    }

    const auto& page = *parsed.index_entities_response;
    if (page.models.empty()) {
      break;
    }

    result.models.insert(result.models.end(), page.models.begin(), page.models.end());

    // Advance pagination. A non-positive nextSkip and an empty token mean "done".
    skip = (page.next_skip && *page.next_skip > 0) ? page.next_skip : std::nullopt;
    continuation_token = (page.continuation_token && !page.continuation_token->empty())
                             ? page.continuation_token
                             : std::nullopt;

    if (!skip && !continuation_token) {
      break;
    }
  }

  return result;
}

std::vector<AzureCatalogClient::FetchedFilterSet> AzureCatalogClient::FetchAllFilterSets() {
  std::vector<FetchedFilterSet> results;
  for (const auto& filters : BuildSearchFilters(ep_detector_, model_filter_)) {
    if (auto result = FetchFilterSet(filters)) {
      results.push_back(std::move(*result));
    }
  }

  return results;
}

std::vector<CatalogLocalModel> AzureCatalogClient::FetchAllModels() {
  std::vector<CatalogLocalModel> models;
  for (auto& set : FetchAllFilterSets()) {
    models.insert(models.end(), std::make_move_iterator(set.models.begin()),
                  std::make_move_iterator(set.models.end()));
  }

  return models;
}

std::vector<ModelInfo> AzureCatalogClient::FetchAllModelInfos() {
  std::vector<ModelInfo> infos;
  for (const auto& set : FetchAllFilterSets()) {
    auto batch = ToModelInfos(set.models, set.region);
    infos.insert(infos.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
  }

  return infos;
}

std::vector<ModelInfo> AzureCatalogClient::FetchModelsByIds(
    const std::vector<std::string>& model_ids) {
  if (model_ids.empty()) {
    return {};
  }

  auto result = FetchFilterSet(BuildModelIdFilters(model_filter_, model_ids));
  if (!result) {
    return {};
  }

  return ToModelInfos(result->models, result->region);
}

std::vector<ModelInfo> AzureCatalogClient::FetchAllVersionsByAlias(
    const std::string& model_alias,
    const std::string& model_name,
    int /*max_versions*/) {
  // Fetch all versions of the alias across per-device filter sets. Each filter set
  // queries for variants matching the alias on a specific device/EP pair; the results
  // are aggregated. The caller applies per-variant version caps (latest X per variant).
  const auto filter_sets = BuildSearchFilters(ep_detector_, model_filter_, /*latest_only=*/false,
                                              model_alias, model_name);

  std::vector<ModelInfo> result;

  for (const auto& filters : filter_sets) {
    auto walk = FetchFilterSet(filters);
    if (!walk) {
      continue;
    }

    auto batch = ToModelInfos(walk->models, walk->region);
    result.insert(result.end(),
                  std::make_move_iterator(batch.begin()),
                  std::make_move_iterator(batch.end()));
  }

  return result;
}

std::unique_ptr<ICatalogClient> MakeCatalogClient(
    const std::string& base_url,
    const std::string& filter_override,
    const IEpDetector& ep_detector,
    ILogger& logger,
    const std::string& /*cache_directory*/,
    const std::string& catalog_region,
    bool disable_region_fallback) {
  return std::make_unique<AzureCatalogClient>(base_url, filter_override, ep_detector, logger,
                                              AzureCatalogClient::HttpPostResponseFn{},
                                              catalog_region, !disable_region_fallback);
}

}  // namespace fl
