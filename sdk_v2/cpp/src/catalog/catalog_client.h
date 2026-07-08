// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "model_info.h"

#include <memory>
#include <string>
#include <vector>

namespace fl {

/// Abstract catalog client. Implemented by the live Azure catalog client,
/// which queries the Azure Foundry catalog REST API.
class ICatalogClient {
 public:
  virtual ~ICatalogClient() = default;

  /// Fetch all model infos available from this catalog source. May filter on
  /// available execution providers.
  virtual std::vector<ModelInfo> FetchAllModelInfos() = 0;

  /// Look up specific model IDs (e.g., older versions present in the local
  /// cache that aren't in the latest catalog). Returns {} for IDs that cannot
  /// be resolved.
  virtual std::vector<ModelInfo> FetchModelsByIds(
      const std::vector<std::string>& model_ids) = 0;

  /// Fetch all known versions of a model (by alias), bypassing the "latest only"
  /// filter that `FetchAllModelInfos` applies. Maps to C#
  /// `IAzureFoundryApiService.FetchAllModelVersionsAsync`.
  ///
  /// `model_alias` must be non-empty.
  /// `model_name` optionally filters by variant name (default empty = no filter).
  /// `max_versions` is applied per variant name (latest X per variant;
  /// 0 or negative = no cap).
  /// Implementations that cannot list older versions return whatever they have
  /// locally.
  ///
  /// Provided with a default `{}` body so an implementation that has not yet
  /// overridden it still compiles.
  virtual std::vector<ModelInfo> FetchAllVersionsByAlias(
      const std::string& /*model_alias*/,
      const std::string& /*model_name*/ = "",
      int /*max_versions*/ = 0) {
    return {};
  }
};

/// Production helper that combines a catalog fetch with locally cached model
/// resolution and BYO synthesis.
std::vector<ModelInfo> FetchAllModelInfosWithCachedModels(
    ICatalogClient& client,
    const std::vector<std::string>& cached_model_ids,
    ILogger& logger);

/// Construct a client for the live Azure Foundry catalog.
/// - `ep_detector` limits results to models supported by this machine.
/// - `filter_override` sets the foundryLocal tag filter.
/// - `catalog_region` controls regional routing: empty/"auto" means detect it,
///   any other value is an explicit region.
/// - `disable_region_fallback` disables cross-region retries.
std::unique_ptr<ICatalogClient> MakeCatalogClient(
    const std::string& base_url,
    const std::string& filter_override,
    const IEpDetector& ep_detector,
    ILogger& logger,
    const std::string& cache_directory,
    const std::string& catalog_region = "",
    bool disable_region_fallback = false);

}  // namespace fl
