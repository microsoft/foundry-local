// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "catalog/base_model_catalog.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

class ICatalogClient;

/// Azure-specific catalog. Fetches from Azure Foundry catalog API,
/// scans local cache, merges results.
/// Maps to C# AzureModelCatalog.
class AzureModelCatalog : public BaseModelCatalog {
 public:
  using ModelFactory = std::function<Model(ModelInfo info, std::string local_path)>;

  AzureModelCatalog(std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
                    std::string cache_dir,
                    ModelFactory model_factory,
                    const IEpDetector& ep_detector,
                    ILogger& logger,
                    bool cache_only = false,
                    std::string catalog_region = "",
                    bool disable_region_fallback = false);
  ~AzureModelCatalog() override;

 protected:
  std::vector<Model> FetchModels() const override;
  std::vector<Model> FetchModelVersions(const std::string& model_alias,
                                        const std::string& model_name = "") const override;
  std::vector<Model> FetchModelsByIds(const std::vector<std::string>& model_ids) const override;
  virtual std::unique_ptr<ICatalogClient> CreateCatalogClient(const std::string& url,
                                                              const std::string& filter) const;

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

  static constexpr const char* kDefaultCatalogUrl = "https://ai.azure.com/api/centralus/ux/v1.0";
  static constexpr const char* kDefaultCatalogFilter = "''";

  CatalogResult GetLiveCatalogOrLocalSnapshot(const std::vector<std::string>& cached_model_ids) const;
  std::vector<Model> CreateModelsWithLocalPaths(const std::vector<ModelInfo>& model_infos,
                                                const LocalModels& local_models) const;

  std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls_;
  std::string cache_dir_;
  ModelFactory model_factory_;
  const IEpDetector& ep_detector_;
  ILogger& logger_;
  bool cache_only_;
  // Configured Azure region: empty/"auto" → auto-detect, explicit → hard override.
  std::string catalog_region_;
  bool disable_region_fallback_;
};

}  // namespace fl
