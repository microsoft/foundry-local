// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "catalog/remote_model_catalog.h"
#include "configuration.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

class ICatalogClient;
class ITelemetry;

/// Azure-specific catalog. Fetches from Azure Foundry catalog API,
/// scans local cache, merges results.
/// Maps to C# AzureModelCatalog.
class AzureModelCatalog : public RemoteModelCatalog {
 public:
  AzureModelCatalog(std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
                    std::string cache_dir,
                    ModelFactory model_factory,
                    const IEpDetector& ep_detector,
                    ILogger& logger,
                    bool cache_only,
                    ITelemetry& telemetry);
  ~AzureModelCatalog() override;

 protected:
  std::vector<ModelInfo> FetchModelInfos() const override;
  std::vector<ModelInfo> FetchModelVersionInfos(const std::string& model_alias,
                                        const std::string& model_name,
                                        int max_versions) const override;
  std::vector<ModelInfo> FetchModelInfosByIds(const std::vector<std::string>& model_ids) const override;
  virtual std::unique_ptr<ICatalogClient> CreateCatalogClient(const std::string& url,
                                                              const std::string& filter) const;
  std::string GetModelPath(const ModelInfo& info, Query query) const override;
  void RefreshRetainedVariantCache(Model& model) const override;

 private:
  using LocalModels = std::map<std::string, std::string>;

  enum class CatalogSource {
    kLive,
    kSnapshot,
  };

  struct CatalogResult {
    std::vector<ModelInfo> model_infos;
    CatalogSource source;
  };

  static constexpr const char* kDefaultCatalogUrl = Configuration::kDefaultCatalogUrl;
  static constexpr const char* kDefaultCatalogFilter = Configuration::kDefaultCatalogFilter;

  CatalogResult GetLiveCatalogOrLocalSnapshot(const std::vector<std::string>& cached_model_ids) const;
  std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls_;
  std::string cache_dir_;
  mutable std::mutex paths_mutex_;
  mutable LocalModels scanned_local_models_;
  ILogger& logger_;
  bool cache_only_;
  ITelemetry& telemetry_;
};

}  // namespace fl
