// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "catalog/base_model_catalog.h"

#include <filesystem>
#include <functional>
#include <mutex>

namespace fl {

/// Mutable, persistent catalog for models registered from arbitrary local directories.
class LocalModelCatalog final : public BaseModelCatalog {
 public:
  using ModelFactory = std::function<Model(ModelInfo, std::string, std::function<void(const std::string&)>,
                                           std::function<void()>)>;

  LocalModelCatalog(std::filesystem::path app_data_dir, ModelFactory model_factory, ILogger& logger);

  Model* RegisterModel(const ModelInfo& model_info) override;
  void UnregisterModel(const std::string& alias_or_model_id) override;
  std::vector<Model*> GetLocalModels() const override;

  struct Registration {
    ModelInfo info;
    std::string model_path;
  };

 protected:
  std::vector<Model> FetchModels() const override;

 private:
  ModelInfo ResolveMetadata(const ModelInfo& supplied, const std::string& model_path, const std::string& alias) const;
  std::vector<Registration> LoadRegistrations() const;
  void SaveRegistrations(const std::vector<Registration>& registrations) const;
  void WriteMetadata(const Registration& registration) const;
  Model CreateModel(const Registration& registration) const;

  std::filesystem::path catalog_dir_;
  std::filesystem::path index_path_;
  std::filesystem::path lock_path_;
  ModelFactory model_factory_;
  ILogger& logger_;
  mutable std::mutex registration_mutex_;
};

}  // namespace fl
