// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "catalog/azure_catalog_models.h"
#include "catalog/catalog_client.h"
#include "ep_detection/ep_detector.h"
#include "http/http_client.h"
#include "logger.h"
#include "model_info.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fl {

/// Returns whether a current Semantic Version meets a catalog minimum version.
/// Invalid versions are incompatible.
bool IsFoundryLocalVersionCompatible(const std::string& current_version,
                                     const std::string& minimum_version);

/// Live Azure Foundry catalog client. Queries the Asset Gallery API
/// (`asset-gallery/v1.0/models`) for the models available to the local
/// hardware, paginating through results and converting each entry to ModelInfo.
///
/// The Asset Gallery service load-balances and routes each request to a healthy
/// regional backend on its own, so this client does not do any region
/// detection or cross-region fallback.
///
/// Uses one filter set per detected device, page size 50, and pagination via
/// continuationToken.
class AzureCatalogClient : public ICatalogClient {
 public:
  /// Response-aware HTTP POST. Used for catalog fetches.
  using HttpPostResponseFn =
      std::function<http::HttpResponse(const std::string& url, const std::string& body)>;

  /// @param base_url Catalog endpoint, e.g. "https://api.catalog.azureml.ms/asset-gallery/v1.0/models".
  /// @param filter_override Deployment-option filter override. Empty means `Foundry Local on Devices`.
  /// @param ep_detector Reports available device and execution-provider pairs.
  /// @param logger Logger.
  /// @param http_post HTTP POST implementation. The default uses `http::HttpPostWithResponse`.
  /// @param retry_config Bounded retry policy for transport, throttling, and server failures.
  AzureCatalogClient(const std::string& base_url,
                     const std::string& filter_override,
                     const IEpDetector& ep_detector,
                     ILogger& logger,
                     HttpPostResponseFn http_post = {},
                     http::RetryConfig retry_config = {});

  /// Fetch every catalog model entry visible to the local hardware (raw form,
  /// before conversion to ModelInfo). One filter set per device, fully paginated.
  std::vector<CatalogLocalModel> FetchAllModels();

  std::vector<ModelInfo> FetchAllModelInfos() override;

  std::vector<ModelInfo> FetchModelsByIds(const std::vector<std::string>& model_ids) override;

  /// Fetch every usable version of `model_alias` from the live catalog. The
  /// query retains device/EP filters, uses the legacy tags alias, and omits
  /// latest, archived, and deployment-option filters so historical records
  /// with older metadata schemas are included. Optionally filters by `model_name`.
  std::vector<ModelInfo> FetchAllVersionsByAlias(const std::string& model_alias,
                                                 const std::string& model_name = "",
                                                 int max_versions = 0) override;

 private:
  /// Run all pages of one filter set.
  std::vector<CatalogLocalModel> FetchFilterSet(const std::vector<CatalogFilter>& filters);
  http::HttpResponse PostWithRetry(const std::string& body);

  std::string base_url_;
  std::vector<std::string> model_filter_;  // deploymentOptions filter values
  const IEpDetector& ep_detector_;
  ILogger& logger_;
  HttpPostResponseFn http_post_response_;
  http::RetryConfig retry_config_;
};

}  // namespace fl
