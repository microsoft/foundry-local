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

constexpr int kPageSize = 50;

constexpr const char* kAssetGalleryModelsUrl = "https://api.catalog.azureml.ms/asset-gallery/v1.0/models";
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
/// Empty override → {} so the caller can apply the default deployment option.
/// Otherwise split on ',', drop entries that are empty after whitespace-trimming,
/// then strip surrounding quotes.
std::vector<std::string> CreateModelFilter(const std::string& filter_override) {
  if (filter_override.empty()) {
    return {};
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

bool IsAssetGalleryUrl(const std::string& url) {
  return url.find("/asset-gallery/") != std::string::npos;
}

/// Detect the Azure region by POSTing a probe to the catalog gallery and reading
/// the `azureml-served-by-cluster` response header. Returns "centralus" on failure.
std::string DetectRegion(const AzureCatalogClient::HttpPostResponseFn& http_post_response, ILogger& logger) {
  http::HttpResponse response = http_post_response(kAssetGalleryModelsUrl, kRegionProbeBody);

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

std::string BuildRequestUrl(const std::string& base_url) {
  if (base_url.empty()) {
    return kAssetGalleryModelsUrl;
  }

  return base_url;
}

std::string BuildRequestBody(const std::vector<CatalogFilter>& filters,
                             const std::optional<int>& skip,
                             const std::optional<std::string>& continuation_token) {
  AzureCatalogRequest request;
  request.filters = filters;
  request.page_size = kPageSize;
  request.skip = skip;
  request.continuation_token = continuation_token;

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

  // Full parameter-driven filter sets (keep for easy rollback once catalog models are updated):
  // std::vector<std::vector<CatalogFilter>> filter_sets;
  // for (const auto& [device, eps] : ep_detector.GetAvailableDevicesToEPs()) {
  //   std::vector<CatalogFilter> filters;
  //
  //   std::vector<std::string> deployment_options = model_filter;
  //   if (deployment_options.empty()) {
  //     deployment_options.push_back("foundryLocalDevices");
  //   }
  //
  //   filters.push_back(MakeFilter("DeploymentOptions", std::move(deployment_options)));
  //   if (!model_alias.empty()) {
  //     filters.push_back(MakeFilter("Alias", {model_alias}));
  //   }
  //   if (!model_name.empty()) {
  //     filters.push_back(MakeFilter("Name", {model_name}));
  //   }
  //   filters.push_back(MakeFilter("VariantInformation/VariantMetadata/Device", {ToLower(device)}));
  //   filters.push_back(MakeFilter("VariantInformation/VariantMetadata/ExecutionProvider", eps));
  //
  //   if (!latest_only) {
  //     // Placeholder to keep the parameter part of the behavior contract.
  //     // Asset-gallery query currently does not require an extra field to fetch all versions.
  //   }
  //
  //   filter_sets.push_back(std::move(filters));
  // }
  // return filter_sets;

  // Temporary CPU-only path for catalog validation.
  (void)ep_detector;
  (void)model_filter;
  (void)latest_only;
  (void)model_alias;
  (void)model_name;

  std::vector<CatalogFilter> filters;
  filters.push_back(MakeFilter("VariantInformation/VariantMetadata/Device", {"cpu"}));

  std::vector<std::vector<CatalogFilter>> filter_sets;
  filter_sets.push_back(std::move(filters));
  return filter_sets;
}


std::vector<CatalogFilter> BuildModelIdFilters(const std::vector<std::string>& model_filter,
                                               const std::vector<std::string>& model_ids) {
  std::vector<CatalogFilter> filters;
  std::vector<std::string> deployment_options = model_filter;
  if (deployment_options.empty()) {
    deployment_options.push_back("foundryLocalDevices");
  }

  std::vector<std::string> names;
  names.reserve(model_ids.size());
  for (const auto& model_id : model_ids) {
    const auto colon = model_id.rfind(':');
    names.push_back(colon == std::string::npos ? model_id : model_id.substr(0, colon));
  }

  filters.push_back(MakeFilter("DeploymentOptions", deployment_options));
  filters.push_back(MakeFilter("Name", names));
  return filters;
}

}  // namespace

AzureCatalogClient::AzureCatalogClient(const std::string& base_url,
                                       const std::string& filter_override,
                                       const IEpDetector& ep_detector,
                                       ILogger& logger,
                                       HttpPostResponseFn http_post,
                                       std::string catalog_region)
    : base_url_(base_url),
      model_filter_(CreateModelFilter(filter_override)),
      ep_detector_(ep_detector),
      logger_(logger),
      http_post_response_(std::move(http_post)) {
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

  // An explicit region is a hard override. Empty/"auto" means detect the region
  // when using the asset-gallery catalog service.
  const auto normalized_catalog_region = ToLower(catalog_region);
  if (!normalized_catalog_region.empty() && normalized_catalog_region != "auto") {
    region_ = normalized_catalog_region;
  } else if (base_url_.empty() || IsAssetGalleryUrl(base_url_)) {
    region_ = DetectRegion(http_post_response_, logger_);
  }
}

std::optional<AzureCatalogClient::FetchedFilterSet> AzureCatalogClient::FetchFilterSet(
    const std::vector<CatalogFilter>& filters) {
  FetchedFilterSet result;
  result.region = region_;

  std::optional<int> skip;
  std::optional<std::string> continuation_token;
  const std::string request_url = BuildRequestUrl(base_url_);

  while (true) {
    const std::string body = BuildRequestBody(filters, skip, continuation_token);

    http::HttpResponse response = http_post_response_(request_url, body);

    if (response.status == 0 || response.status < 200 || response.status >= 300) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK,
               "catalog request to " + request_url + " failed: " + http::DescribeFailure(response));
    }

    const auto parsed = nlohmann::json::parse(response.body).get<AzureCatalogResponse>();
    if (parsed.models.empty()) {
      break;
    }

    result.models.insert(result.models.end(), parsed.models.begin(), parsed.models.end());

    // Advance pagination. A non-positive nextSkip and an empty token mean "done".
    skip = (parsed.next_skip && *parsed.next_skip > 0) ? parsed.next_skip : std::nullopt;
    continuation_token = (parsed.continuation_token && !parsed.continuation_token->empty())
                             ? parsed.continuation_token
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
    const std::string& catalog_region) {
  return std::make_unique<AzureCatalogClient>(base_url, filter_override, ep_detector, logger,
                                              AzureCatalogClient::HttpPostResponseFn{},
                                              catalog_region);
}

}  // namespace fl
