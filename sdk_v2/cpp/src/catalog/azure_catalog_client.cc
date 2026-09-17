// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_catalog_client.h"

#include "http/http_client.h"
#include "utils.h"
#include "version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace fl {

namespace {

constexpr int kPageSize = 50;
constexpr const char* kDefaultDeploymentOption = "Foundry Local on Devices";

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

/// Build the deploymentOptions filter values from the override string.
/// An empty override means "use the default ('Foundry Local on Devices')".
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

CatalogFilter MakeFilter(std::string field,
                         std::vector<std::string> values,
                         std::string op = "eq") {
  CatalogFilter f;
  f.field = std::move(field);
  f.op = std::move(op);
  f.values = std::move(values);
  return f;
}

std::vector<std::string> ResolveDeploymentOptions(const std::vector<std::string>& model_filter) {
  if (!model_filter.empty()) {
    return model_filter;
  }

  return {kDefaultDeploymentOption};
}

std::string BuildRequestBody(const std::vector<CatalogFilter>& filters,
                             const std::optional<std::string>& continuation_token) {
  AzureCatalogRequest request;
  request.filters = filters;
  request.page_size = kPageSize;
  request.continuation_token = continuation_token;

  const nlohmann::json body = request;
  return body.dump();
}

struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
};

std::optional<SemVer> ParseSemVer(const std::string& version) {
  int major = 0;
  int minor = 0;
  int patch = 0;
  char first_dot = '\0';
  char second_dot = '\0';
  std::istringstream stream(version);
  if (!(stream >> major >> first_dot >> minor >> second_dot >> patch) ||
      first_dot != '.' || second_dot != '.' || !stream.eof() ||
      major < 0 || minor < 0 || patch < 0) {
    return std::nullopt;
  }
  return SemVer{major, minor, patch};
}

bool MeetsMinFlVersion(const CatalogLocalModel& model) {
  if (!model.min_fl_version || model.min_fl_version->empty()) {
    return true;
  }

  static const std::optional<SemVer> current = ParseSemVer(FOUNDRY_LOCAL_VERSION);
  const auto minimum = ParseSemVer(*model.min_fl_version);
  if (!current || !minimum) {
    return false;
  }

  return std::tie(current->major, current->minor, current->patch) >=
         std::tie(minimum->major, minimum->minor, minimum->patch);
}

std::vector<ModelInfo> ToModelInfos(const std::vector<CatalogLocalModel>& raw_models) {
  std::vector<ModelInfo> infos;
  for (const auto& model : raw_models) {
    if (!MeetsMinFlVersion(model)) {
      continue;
    }

    if (auto info = CatalogModelToModelInfo(model)) {
      infos.push_back(std::move(*info));
    }
  }

  return infos;
}

/// Build the full-service Asset Gallery filter sets used for catalog queries.
std::vector<std::vector<CatalogFilter>> BuildSearchFilters(const IEpDetector& ep_detector,
                                                           const std::vector<std::string>& model_filter,
                                                           bool latest_only = true) {
  std::vector<std::vector<CatalogFilter>> filter_sets;
  for (const auto& [device, eps] : ep_detector.GetAvailableDevicesToEPs()) {
    std::vector<CatalogFilter> filters{
        MakeFilter("type", {"models"}),
        MakeFilter("kind", {"Versioned"}),
        MakeFilter("annotations/systemCatalogData/deploymentOptions",
                   ResolveDeploymentOptions(model_filter)),
        MakeFilter("annotations/archived", {"true"}, "NotEquals"),
    };
    if (latest_only) {
      filters.push_back(MakeFilter("labels", {"latest"}));
    }
    filters.push_back(MakeFilter("variantInformation/variantMetadata/device", {ToLower(device)}));
    filters.push_back(MakeFilter("variantInformation/variantMetadata/executionProvider", eps));
    filter_sets.push_back(std::move(filters));
  }
  return filter_sets;
}

std::vector<CatalogFilter> BuildModelIdFilters(const std::vector<std::string>& model_filter,
                                               const std::vector<std::string>& model_ids) {
  std::vector<std::string> names;
  names.reserve(model_ids.size());
  for (const auto& model_id : model_ids) {
    const auto colon = model_id.rfind(':');
    names.push_back(colon == std::string::npos ? model_id : model_id.substr(0, colon));
  }

  std::vector<CatalogFilter> filters;
  filters.push_back(MakeFilter("type", {"models"}));
  filters.push_back(MakeFilter("kind", {"Versioned"}));
  filters.push_back(MakeFilter("annotations/systemCatalogData/deploymentOptions",
                               ResolveDeploymentOptions(model_filter)));
  filters.push_back(MakeFilter("annotations/archived", {"true"}, "NotEquals"));
  filters.push_back(MakeFilter("name", names));
  return filters;
}

}  // namespace

AzureCatalogClient::AzureCatalogClient(const std::string& base_url,
                                       const std::string& filter_override,
                                       const IEpDetector& ep_detector,
                                       ILogger& logger,
                                       HttpPostResponseFn http_post)
    : base_url_(base_url),
      model_filter_(CreateModelFilter(filter_override)),
      ep_detector_(ep_detector),
      logger_(logger),
      http_post_response_(std::move(http_post)) {
  if (!http_post_response_) {
    http_post_response_ = [](const std::string& url, const std::string& body) {
      http::HttpRequestOptions options;
      options.user_agent = kUserAgent;
      options.headers["x-ms-use-full-service-contracts"] = "true";
      return http::HttpPostWithResponse(url, body, options);
    };
  }
}

std::vector<CatalogLocalModel> AzureCatalogClient::FetchFilterSet(const std::vector<CatalogFilter>& filters) {
  std::vector<CatalogLocalModel> models;
  std::optional<std::string> continuation_token;

  while (true) {
    const std::string body = BuildRequestBody(filters, continuation_token);
    http::HttpResponse response = http_post_response_(base_url_, body);

    if (response.status == 0 || response.status < 200 || response.status >= 300) {
      logger_.Log(LogLevel::Warning,
                 "catalog request to " + base_url_ + " failed: " + http::DescribeFailure(response));
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK,
               "catalog request to " + base_url_ + " failed: " + http::DescribeFailure(response));
    }

    const auto parsed = nlohmann::json::parse(response.body).get<AzureCatalogResponse>();
    if (parsed.models.empty()) {
      break;
    }

    models.insert(models.end(), parsed.models.begin(), parsed.models.end());

    // A missing or empty continuation token means "done".
    if (parsed.continuation_token && !parsed.continuation_token->empty()) {
      continuation_token = parsed.continuation_token;
    } else {
      break;
    }
  }

  return models;
}

std::vector<CatalogLocalModel> AzureCatalogClient::FetchAllModels() {
  std::vector<CatalogLocalModel> models;
  for (const auto& filters : BuildSearchFilters(ep_detector_, model_filter_)) {
    auto page = FetchFilterSet(filters);
    models.insert(models.end(), std::make_move_iterator(page.begin()), std::make_move_iterator(page.end()));
  }

  return models;
}

std::vector<ModelInfo> AzureCatalogClient::FetchAllModelInfos() {
  return ToModelInfos(FetchAllModels());
}

std::vector<ModelInfo> AzureCatalogClient::FetchModelsByIds(
    const std::vector<std::string>& model_ids) {
  if (model_ids.empty()) {
    return {};
  }

  auto model_infos = ToModelInfos(FetchFilterSet(BuildModelIdFilters(model_filter_, model_ids)));
  std::erase_if(model_infos, [&model_ids](const ModelInfo& info) {
    return std::find(model_ids.begin(), model_ids.end(), info.model_id) == model_ids.end();
  });
  return model_infos;
}

std::vector<ModelInfo> AzureCatalogClient::FetchAllVersionsByAlias(
    const std::string& model_alias,
    const std::string& model_name,
    int /*max_versions*/) {
  // The catalog has no server-side alias field, so fetch every version for each
  // device/EP pair (labels=latest removed) and filter client-side by the alias
  // (and optionally variant name) that CatalogModelToModelInfo derived.
  std::vector<ModelInfo> result;

  for (const auto& filters : BuildSearchFilters(ep_detector_, model_filter_, /*latest_only=*/false)) {
    auto infos = ToModelInfos(FetchFilterSet(filters));

    for (auto& info : infos) {
      if (info.alias != model_alias) {
        continue;
      }

      if (!model_name.empty() && info.name != model_name) {
        continue;
      }

      result.push_back(std::move(info));
    }
  }

  return result;
}

std::unique_ptr<ICatalogClient> MakeCatalogClient(
    const std::string& base_url,
    const std::string& filter_override,
    const IEpDetector& ep_detector,
    ILogger& logger,
    const std::string& /*cache_directory*/) {
  return std::make_unique<AzureCatalogClient>(base_url, filter_override, ep_detector, logger);
}

}  // namespace fl

