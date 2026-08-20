// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "catalog/model_source.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "model_info.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

class ICatalogClient;

/// AzureModelSource — the Public (Azure) catalog source. Fetches from the Azure Foundry
/// catalog API and, short-term, also owns local-cache resolution: it scans the cache,
/// folds cached ids into the live fetch, attaches each cached entry's local path, and
/// synthesizes thin stub metadata (tagged `kLocal`) for disk-only ("BYOM") models.
///
/// Returns `ModelInfo` only — the aggregating ModelCatalog owns the ModelFactory and builds
/// the `Model` leaves. The dedicated BYOM-local source (future) will replace the inline
/// `kLocal` stub handling here.
///
/// Ported from the former AzureModelCatalog (fetch guts unchanged).
class AzureModelSource : public IModelSource {
 public:
  AzureModelSource(std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
                   std::string cache_dir,
                   const IEpDetector& ep_detector,
                   ILogger& logger,
                   bool cache_only = false,
                   std::string catalog_region = "",
                   bool disable_region_fallback = false);
  ~AzureModelSource() override;

  CatalogSource Source() const override { return CatalogSource::kPublic; }
  std::string Name() const override { return name_; }

  std::vector<ModelInfo> FetchModels() const override;
  std::vector<ModelInfo> FetchModelsByIds(const std::vector<std::string>& model_ids) const override;
  std::vector<ModelInfo> FetchModelVersions(const std::string& model_alias,
                                            const std::string& model_name = "") const override;

 protected:
  /// Test seam: construct the live catalog client. Overridden in tests to inject a fake.
  virtual std::unique_ptr<ICatalogClient> CreateCatalogClient(const std::string& url,
                                                              const std::string& filter) const;

 private:
  using LocalModels = std::map<std::string, std::string>;

  enum class FetchOrigin {
    kLive,
    kSnapshot,
  };

  struct CatalogResult {
    std::vector<ModelInfo> model_infos;
    FetchOrigin origin;
  };

  static constexpr const char* kDefaultCatalogUrl = "https://ai.azure.com/api/centralus/ux/v1.0";
  static constexpr const char* kDefaultCatalogFilter = "''";

  CatalogResult GetLiveCatalogOrLocalSnapshot(const std::vector<std::string>& cached_model_ids) const;

  /// Attach local paths to matched catalog infos and append synthesized `kLocal` stubs for
  /// disk-only models. Mutates `model_infos` in place (appends stubs).
  void AddLocalModels(std::vector<ModelInfo>& model_infos, const LocalModels& local_models) const;

  std::string name_;
  std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls_;
  std::string cache_dir_;
  const IEpDetector& ep_detector_;
  ILogger& logger_;
  bool cache_only_;
  // Configured Azure region: empty/"auto" → auto-detect, explicit → hard override.
  std::string catalog_region_;
  bool disable_region_fallback_;
};

}  // namespace fl
