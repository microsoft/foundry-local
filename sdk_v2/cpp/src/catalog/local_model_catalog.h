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
  using ModelFactory = std::function<Model(ModelInfo info, std::string local_path)>;

  LocalModelCatalog(std::filesystem::path model_cache_dir, ModelFactory model_factory, ILogger& logger);

  Model* RegisterModel(const std::string& model_path, const std::string& model_id,
                       const ModelInfo& metadata) override;
  void UnregisterModel(const std::string& alias_or_model_id) override;

  struct Registration {
    ModelInfo info;
    std::string model_path;
  };

 protected:
  std::vector<Model> FetchModels() const override;
  bool IsAuthoritativeSnapshot() const override { return true; }

 private:
  ModelInfo ResolveMetadata(const ModelInfo& metadata, const std::string& model_id,
                            const std::string& name, int version) const;
  std::vector<Registration> LoadRegistrations() const;
  void SaveRegistrations(const std::vector<Registration>& registrations) const;
  Model CreateModel(const Registration& registration) const;

  std::filesystem::path model_cache_dir_;
  std::filesystem::path index_path_;
  std::filesystem::path lock_path_;
  ModelFactory model_factory_;
  mutable std::mutex registration_mutex_;
};

}  // namespace fl
