// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/model_registry_client.h"

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "exception.h"
#include "http/http_client.h"
#include "logger.h"
#include "util/region_fallback.h"
#include "utils.h"

namespace fl {
namespace {

/// Build the model registry base URL for the given Azure region.
/// Format: https://{region}.api.azureml.ms/modelregistry/v1.0/registry/models/nonazureaccount?assetId=
std::string BuildBaseUrl(const std::string& region) {
  return "https://" + region + ".api.azureml.ms/modelregistry/v1.0/registry/models/nonazureaccount?assetId=";
}

/// URL-encode a string (percent-encoding).
std::string UrlEncode(const std::string& value) {
  std::string result;
  result.reserve(value.size() * 2);
  for (unsigned char c : value) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      result += static_cast<char>(c);
    } else {
      static const char hex[] = "0123456789ABCDEF";
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 0x0F];
    }
  }
  return result;
}

constexpr const char* kUserAgent = "AzureAiStudio";

}  // anonymous namespace

ModelRegistryClient::ModelRegistryClient(std::string region,
                                         ILogger& /*logger*/,
                                         std::unique_ptr<RegionFallback> fallback,
                                         HttpGetResponseFn http_get)
    : default_region_(std::move(region)), http_get_(std::move(http_get)), fallback_(std::move(fallback)) {
  // Default transport: status-aware GET (non-throwing) so the fallback engine can
  // classify region-health failures. Cross-region fallback provides resilience
  // in place of in-region retries when enabled.
  if (!http_get_) {
    http_get_ = [](const std::string& url) {
      http::HttpRequestOptions options;
      options.user_agent = kUserAgent;
      return http::HttpGetWithResponse(url, options);
    };
  }
}

ModelContainer ModelRegistryClient::ResolveModelContainer(const std::string& asset_id,
                                                          const std::string& region) {
  if (asset_id.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "cannot resolve model container: empty asset_id");
  }

  const bool has_per_call_region = !region.empty();
  const std::string resolved_region = ToLower(has_per_call_region ? region : default_region_);
  const std::string encoded = UrlEncode(asset_id);

  auto attempt = [&](const std::string& r) -> http::HttpResponse {
    return http_get_(BuildBaseUrl(r) + encoded);
  };

  // A per-call region comes from explicit config or the model's detected catalog region and must take precedence.
  // Sticky only biases calls that fall back to the client's default region.
  const std::string start = has_per_call_region ? resolved_region : fallback_->StickyRegion().value_or(resolved_region);
  http::HttpResponse response = fallback_->Execute(start, attempt).response;

  if (response.status == 0 || response.status < 200 || response.status >= 300) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK,
             "model registry API request failed for asset_id " + asset_id + ": " +
                 http::DescribeFailure(response));
  }

  return ParseContainer(response.body, asset_id);
}

ModelContainer ModelRegistryClient::ParseContainer(const std::string& body,
                                                   const std::string& asset_id) const {
  if (body.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "empty response from model registry API for asset_id: " + asset_id);
  }

  try {
    auto j = nlohmann::json::parse(body);
    ModelContainer result;

    if (j.contains("blobSasUri") && j["blobSasUri"].is_string()) {
      result.blob_sas_uri = j["blobSasUri"].get<std::string>();
    } else {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "model registry response missing blobSasUri for asset_id: " + asset_id);
    }

    if (j.contains("modelEntity") && j["modelEntity"].is_object()) {
      auto& me = j["modelEntity"];
      if (me.contains("description") && me["description"].is_string()) {
        result.description = me["description"].get<std::string>();
      }
    }

    return result;
  } catch (const nlohmann::json::exception& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             std::string("failed to parse model registry response: ") + e.what());
  }
}

}  // namespace fl
