// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Shared helpers for web service tests — MockCatalog, MakeTestModelInfo, etc.
// Used by both web_service_test.cc and audio_web_service_test.cc.
#pragma once

#include "catalog.h"
#include "model.h"
#include "model_info.h"

#include <foundry_local/foundry_local_c.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fl::test {

// ========================================================================
// Mock catalog — hand-rolled (project convention: no GMock)
// ========================================================================

class MockCatalog : public ICatalog {
 public:
  const std::string& GetName() const override {
    static const std::string name = "mock-catalog";
    return name;
  }

  std::vector<Model*> ListModels() const override {
    std::vector<Model*> result;
    result.reserve(models_.size());
    for (auto& m : models_) {
      result.push_back(&m);
    }
    return result;
  }

  Model* GetModel(const std::string& alias) const override {
    for (auto& m : models_) {
      if (m.Alias() == alias) {
        return &m;
      }
    }
    return nullptr;
  }

  Model* GetModelVariant(const std::string& model_id) const override {
    for (auto& m : models_) {
      for (auto* v : m.Variants()) {
        if (v->Id() == model_id) {
          return v;
        }
      }
    }
    return nullptr;
  }

  Model* GetLatestVersion(const Model* /*model*/) const override {
    return nullptr;
  }

  std::vector<Model*> GetCachedModels() const override {
    std::vector<Model*> result;
    for (auto& m : models_) {
      if (m.IsCached()) {
        result.push_back(&m);
      }
    }
    return result;
  }

  std::vector<Model*> GetLoadedModels() const override {
    std::vector<Model*> result;
    for (auto& m : models_) {
      if (m.IsLoaded()) {
        result.push_back(&m);
      }
    }
    return result;
  }

  std::vector<Model*> GetModelVersions(const std::string& model_alias,
                                       const std::string& variant_name,
                                       int max_versions = 0) override {
    std::vector<Model*> result;
    for (auto& m : models_) {
      if (!model_alias.empty() && m.Alias() != model_alias) {
        continue;
      }
      for (auto* v : m.Variants()) {
        if (max_versions > 0 && result.size() >= static_cast<size_t>(max_versions)) {
          return result;
        }
        if (variant_name.empty() || v->Info().name == variant_name) {
          result.push_back(v);
        }
      }
    }
    return result;
  }

  /// Add a model variant. Groups variants by alias into container models,
  /// matching BaseModelCatalog behavior.
  void AddModel(Model model) {
    const std::string& alias = model.Alias();

    for (auto& m : models_) {
      if (m.Alias() == alias) {
        m.AddVariant(std::move(model));
        return;
      }
    }

    // First variant with this alias — create a new container.
    models_.push_back(Model::MakeContainer(std::move(model)));
  }

 private:
  mutable std::vector<Model> models_;
};

// ========================================================================
// Helper: create a test ModelInfo
// ========================================================================

/// The tiny GPT-2 model shipped in testdata/ — small enough to load in all tests.
static constexpr const char* kLoadableTestModelAlias = "tiny-random-gpt2-fp32-1";

static inline ModelInfo MakeTestModelInfo(const std::string& alias,
                                          const std::string& publisher = "test-publisher",
                                          const std::string& task = "chat-completion") {
  ModelInfo info;
  info.model_id = alias + ":1";
  info.name = alias;
  info.version = 1;
  info.alias = alias;
  info.uri = "test://" + alias;
  info.device_type = DeviceType::kCPU;
  info.execution_provider = "CPUExecutionProvider";
  info.task = task;
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = publisher;
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TASK_STR] = task;
  info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT] = 1700000000;
  return info;
}

static inline std::string GetTestDataModelPath(const std::string& model_dir_name) {
  auto runtime_path = std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / model_dir_name;
  return runtime_path.lexically_normal().string();
}

}  // namespace fl::test
