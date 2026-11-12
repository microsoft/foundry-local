// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "catalog/base_model_catalog.h"
#include "ep_detection/ep_detector.h"

#include <functional>
#include <map>
#include <memory>

namespace fl {

// Remote sources return normalized metadata; all paths then use the same admission and construction pipeline.
class RemoteModelCatalog : public BaseModelCatalog {
 public:
  using ModelFactory = std::function<Model(ModelInfo, std::string)>;

  ~RemoteModelCatalog() override;
  void SetRuntimeDevices(std::map<std::string, std::vector<std::string>> devices);

 protected:
  enum class Query { Listing, Lookup, Exact, History };

  RemoteModelCatalog(std::string name, CatalogType type, ModelFactory factory,
                     const IEpDetector& ep_detector, ILogger& logger);

  std::vector<Model> FetchModels() const final;
  std::optional<std::vector<Model>> FetchLookupModels() const final;
  std::vector<Model> FetchModelsByIds(const std::vector<std::string>& ids) const final;
  std::vector<Model> FetchModelVersions(const std::string& alias, const std::string& name,
                                       int max_versions) const final;
  VariantEligibility MakeVariantEligibility() const final;

  virtual std::vector<ModelInfo> FetchModelInfos() const = 0;
  virtual std::optional<std::vector<ModelInfo>> FetchLookupModelInfos() const { return std::nullopt; }
  virtual std::vector<ModelInfo> FetchModelInfosByIds(const std::vector<std::string>& ids) const = 0;
  virtual std::vector<ModelInfo> FetchModelVersionInfos(const std::string& alias, const std::string& name,
                                                       int max_versions) const = 0;
  virtual std::string GetModelPath(const ModelInfo& info, Query query) const = 0;
  virtual bool RequiresCompletedCache() const { return false; }

  std::vector<Model> CreateModels(const std::vector<ModelInfo>& infos, Query query = Query::Exact) const;
  bool IsModelCompatible(const ModelInfo& info) const;
  const IEpDetector& CatalogDevices() const;

 private:
  class Devices;
  std::unique_ptr<Devices> devices_;
  ModelFactory factory_;
  ILogger& logger_;
};

}  // namespace fl
