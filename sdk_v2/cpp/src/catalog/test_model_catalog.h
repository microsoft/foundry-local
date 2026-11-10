// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "catalog/azure_model_catalog.h"
#include "catalog/azure_ml_registry_client.h"
#include "download/download_manager.h"

namespace fl {

class ModelLoadManager;

class TestModelCatalog final : public AzureModelCatalog {
 public:
  static constexpr const char* kDefaultRegistry = "prathikrao-pmc-test";
  static constexpr const char* kRegistryProperty = "test.registry";

  TestModelCatalog(std::string registry, const Configuration& config, const IEpDetector& ep_detector,
                   ModelLoadManager& load_manager, ILogger& logger, ITelemetry& telemetry,
                   std::unique_ptr<AzureMlRegistryClient> client = nullptr);

  std::vector<Model*> ListModels() const override;
  Model* GetModel(const std::string& alias) const override;
  Model* GetModelVariant(const std::string& model_id) const override;
  std::vector<Model*> GetCachedModels() const override;
  std::vector<Model*> GetLoadedModels() const override;

 protected:
  std::vector<Model> FetchModels() const override;
  std::optional<std::vector<Model>> FetchLookupModels() const override;
  std::vector<Model> FetchModelVersions(const std::string& alias, const std::string& name,
                                       int max_versions) const override;
  std::vector<Model> FetchModelsByIds(const std::vector<std::string>& ids) const override;
  bool IsAuthoritativeSnapshot() const override { return true; }
  bool RefreshOnEveryAccess() const override { return false; }
  VariantEligibility MakeVariantEligibility() const override;
  void RefreshRetainedVariantCache(Model& model) const override;

 private:
  std::optional<ModelInfo> Adapt(const nlohmann::json& model) const;
  std::vector<Model> FetchAvailableModels() const;
  std::vector<Model> CreateModels(const std::vector<ModelInfo>& infos) const;
  AzureMlRegistryClient& Client() const;

  std::string registry_;
  std::string cache_dir_;
  bool cache_only_;
  std::string credential_mode_;
  ILogger& logger_;
  ModelLoadManager& load_manager_;
  mutable std::mutex snapshot_mutex_;
  mutable std::mutex client_mutex_;
  mutable std::unique_ptr<AzureMlRegistryClient> client_;
  std::unique_ptr<DownloadManager> downloader_;
  mutable std::mutex exact_models_mutex_;
  // Exact handles remain usable independently of latest-only listing reconciliation.
  mutable std::map<std::string, std::unique_ptr<Model>> exact_models_;
};

}  // namespace fl
