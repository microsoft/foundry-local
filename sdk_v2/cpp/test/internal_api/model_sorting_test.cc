// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for model variant sorting within ModelCatalog.
// Verifies the C++ port of C# AzureFoundryService.SortModels /
// CompareModelsForSort / GetModelPriority:
//   - Device-type priority: NPU > vendor-GPU > CUDA-GPU > generic-GPU
//                         > vendor-CPU > generic-CPU > unknown
//   - Version descending (higher first)
//   - CreatedAtUnix descending (newer first)
//
#include "catalog/model_catalog.h"
#include "catalog/model_source.h"
#include "internal_api/test_helpers.h"
#include "logger.h"
#include "model.h"
#include "model_info.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace fl;

namespace {

// Minimal fetch-only source returning canned ModelInfo for the sort harness.
class FakeSortSource : public IModelSource {
 public:
  CatalogSource Source() const override { return CatalogSource::kPublic; }
  std::string Name() const override { return "sort-fake-source"; }
  std::vector<ModelInfo> FetchModels() const override { return models_; }
  void AddModel(ModelInfo info) { models_.push_back(std::move(info)); }

 private:
  std::vector<ModelInfo> models_;
};

}  // namespace

// ========================================================================
// Thin wrapper around ModelCatalog preserving the old test ergonomics:
// AddModel(ModelInfo) before querying, then GetModel(alias).
// ========================================================================

class SortTestCatalog {
 public:
  explicit SortTestCatalog(ILogger& logger) : logger_(logger) {
    auto source = std::make_unique<FakeSortSource>();
    source_ = source.get();
    sources_.push_back(std::move(source));
  }

  void AddModel(ModelInfo info) { source_->AddModel(std::move(info)); }

  Model* GetModel(const std::string& alias) {
    if (!catalog_) {
      catalog_ = std::make_unique<ModelCatalog>(
          "sort-test-catalog", std::move(sources_),
          [this](ModelInfo info) {
            return Model::FromModelInfo(std::move(info), svc_.download_manager, svc_.model_load_manager);
          },
          logger_);
    }
    return catalog_->GetModel(alias);
  }

 private:
  ILogger& logger_;
  fl::test::FakeServiceBindings svc_;
  std::vector<std::unique_ptr<IModelSource>> sources_;
  FakeSortSource* source_ = nullptr;
  std::unique_ptr<ModelCatalog> catalog_;
};

// Helper: create a ModelInfo with device suffix baked into model_id.
static ModelInfo MakeModel(const std::string& base_name,
                           const std::string& device_suffix,
                           int version,
                           const std::string& alias,
                           int64_t created_at_unix = 0) {
  ModelInfo info;
  info.model_id = base_name + device_suffix + ":" + std::to_string(version);
  info.name = base_name;
  info.version = version;
  info.alias = alias;

  if (created_at_unix != 0) {
    info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT] = created_at_unix;
  }

  return info;
}

// ========================================================================
// Test fixture
// ========================================================================

class ModelSortingTest : public ::testing::Test {
 protected:
  StderrLogger logger_;
};

// ========================================================================
// Device-type priority — ordering within a single alias group
// ========================================================================

TEST_F(ModelSortingTest, DevicePriority_NPU_BeforeGPU) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi"));
  catalog.AddModel(MakeModel("phi", "-npu", 1, "phi"));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_NE(variants[0]->Info().model_id.find("-npu:"), std::string::npos);
  EXPECT_NE(variants[1]->Info().model_id.find("-gpu:"), std::string::npos);
}

TEST_F(ModelSortingTest, DevicePriority_VendorGPU_BeforeCudaGPU) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "-cuda-gpu", 1, "phi"));
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi"));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_NE(variants[0]->Info().model_id.find("-gpu:"), std::string::npos);
  EXPECT_FALSE(variants[0]->Info().model_id.find("-cuda-gpu:") != std::string::npos);
  EXPECT_NE(variants[1]->Info().model_id.find("-cuda-gpu:"), std::string::npos);
}

TEST_F(ModelSortingTest, DevicePriority_CudaGPU_BeforeGenericGPU) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "-generic-gpu", 1, "phi"));
  catalog.AddModel(MakeModel("phi", "-cuda-gpu", 1, "phi"));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_NE(variants[0]->Info().model_id.find("-cuda-gpu:"), std::string::npos);
  EXPECT_NE(variants[1]->Info().model_id.find("-generic-gpu:"), std::string::npos);
}

TEST_F(ModelSortingTest, DevicePriority_GenericGPU_BeforeVendorCPU) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "-cpu", 1, "phi"));
  catalog.AddModel(MakeModel("phi", "-generic-gpu", 1, "phi"));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_NE(variants[0]->Info().model_id.find("-generic-gpu:"), std::string::npos);
  EXPECT_NE(variants[1]->Info().model_id.find("-cpu:"), std::string::npos);
}

TEST_F(ModelSortingTest, DevicePriority_VendorCPU_BeforeGenericCPU) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "-generic-cpu", 1, "phi"));
  catalog.AddModel(MakeModel("phi", "-cpu", 1, "phi"));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_NE(variants[0]->Info().model_id.find("-cpu:"), std::string::npos);
  EXPECT_FALSE(variants[0]->Info().model_id.find("-generic-cpu:") != std::string::npos);
  EXPECT_NE(variants[1]->Info().model_id.find("-generic-cpu:"), std::string::npos);
}

TEST_F(ModelSortingTest, DevicePriority_GenericCPU_BeforeUnknown) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "", 1, "phi"));  // unknown device
  catalog.AddModel(MakeModel("phi", "-generic-cpu", 1, "phi"));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_NE(variants[0]->Info().model_id.find("-generic-cpu:"), std::string::npos);
}

TEST_F(ModelSortingTest, DevicePriority_CaseInsensitive) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "-GPU", 1, "phi"));
  catalog.AddModel(MakeModel("phi", "-NPU", 1, "phi"));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_NE(variants[0]->Info().model_id.find("-NPU:"), std::string::npos);
  EXPECT_NE(variants[1]->Info().model_id.find("-GPU:"), std::string::npos);
}

// ========================================================================
// Full priority chain — all seven device types in reverse order
// ========================================================================

TEST_F(ModelSortingTest, DevicePriority_FullChain) {
  SortTestCatalog catalog(logger_);
  // Add in worst-to-best order to verify sort fixes the ordering.
  catalog.AddModel(MakeModel("phi", "", 1, "phi"));              // 6: unknown
  catalog.AddModel(MakeModel("phi", "-generic-cpu", 1, "phi"));  // 5
  catalog.AddModel(MakeModel("phi", "-cpu", 1, "phi"));          // 4
  catalog.AddModel(MakeModel("phi", "-generic-gpu", 1, "phi"));  // 3
  catalog.AddModel(MakeModel("phi", "-cuda-gpu", 1, "phi"));     // 2
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi"));          // 1
  catalog.AddModel(MakeModel("phi", "-npu", 1, "phi"));          // 0

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 7u);
  EXPECT_NE(variants[0]->Info().model_id.find("-npu:"), std::string::npos);
  EXPECT_NE(variants[6]->Info().model_id.find("phi:"), std::string::npos);  // unknown (no device suffix)
}

// ========================================================================
// Version descending — same device, different versions
// ========================================================================

TEST_F(ModelSortingTest, Version_HigherFirst) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi"));
  catalog.AddModel(MakeModel("phi", "-gpu", 3, "phi"));
  catalog.AddModel(MakeModel("phi", "-gpu", 2, "phi"));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 3u);
  EXPECT_EQ(variants[0]->Info().version, 3);
  EXPECT_EQ(variants[1]->Info().version, 2);
  EXPECT_EQ(variants[2]->Info().version, 1);
}

// ========================================================================
// CreatedAtUnix descending — same device, same version
// ========================================================================

TEST_F(ModelSortingTest, CreatedAt_NewerFirst) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi", 1000));
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi", 3000));
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi", 2000));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 3u);

  auto get_created = [](const Model* m) {
    auto it = m->Info().int_properties.find(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT);
    return (it != m->Info().int_properties.end()) ? it->second : 0;
  };

  EXPECT_EQ(get_created(variants[0]), 3000);
  EXPECT_EQ(get_created(variants[1]), 2000);
  EXPECT_EQ(get_created(variants[2]), 1000);
}

TEST_F(ModelSortingTest, CreatedAt_MissingTreatedAsZero) {
  SortTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi"));  // no created_at → 0
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi", 1000));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);

  auto get_created = [](const Model* m) {
    auto it = m->Info().int_properties.find(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT);
    return (it != m->Info().int_properties.end()) ? it->second : 0;
  };

  EXPECT_EQ(get_created(variants[0]), 1000);  // newer first
  EXPECT_EQ(get_created(variants[1]), 0);     // missing → last
}

// ========================================================================
// Three-level combined sort — device → version → created_at
// ========================================================================

TEST_F(ModelSortingTest, CombinedSort_DeviceTrumpsVersion) {
  SortTestCatalog catalog(logger_);
  // CPU version 10 should still come after GPU version 1
  catalog.AddModel(MakeModel("phi", "-cpu", 10, "phi"));
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi"));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_NE(variants[0]->Info().model_id.find("-gpu:"), std::string::npos);
  EXPECT_NE(variants[1]->Info().model_id.find("-cpu:"), std::string::npos);
}

TEST_F(ModelSortingTest, CombinedSort_VersionTrumpsCreatedAt) {
  SortTestCatalog catalog(logger_);
  // Same device: version 1 with newer timestamp should come after version 2
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi", 9999));
  catalog.AddModel(MakeModel("phi", "-gpu", 2, "phi", 1000));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_EQ(variants[0]->Info().version, 2);
  EXPECT_EQ(variants[1]->Info().version, 1);
}

TEST_F(ModelSortingTest, CombinedSort_FullTiebreaker) {
  SortTestCatalog catalog(logger_);
  // All three criteria matter:
  //   Best: NPU v2 (created 3000) — best device
  //   Next: GPU v2 (created 3000) — same version/time, worse device
  //   Next: GPU v2 (created 1000) — same device/version, older
  //   Next: GPU v1 (created 9000) — same device, lower version
  //   Last: CPU v3 (created 9999) — worst device despite highest version
  catalog.AddModel(MakeModel("phi", "-cpu", 3, "phi", 9999));
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi", 9000));
  catalog.AddModel(MakeModel("phi", "-gpu", 2, "phi", 1000));
  catalog.AddModel(MakeModel("phi", "-npu", 2, "phi", 3000));
  catalog.AddModel(MakeModel("phi", "-gpu", 2, "phi", 3000));

  auto variants = catalog.GetModel("phi")->Variants();
  ASSERT_EQ(variants.size(), 5u);

  EXPECT_NE(variants[0]->Info().model_id.find("-npu:"), std::string::npos);

  EXPECT_NE(variants[1]->Info().model_id.find("-gpu:"), std::string::npos);
  EXPECT_EQ(variants[1]->Info().version, 2);

  EXPECT_NE(variants[2]->Info().model_id.find("-gpu:"), std::string::npos);
  EXPECT_EQ(variants[2]->Info().version, 2);

  EXPECT_NE(variants[3]->Info().model_id.find("-gpu:"), std::string::npos);
  EXPECT_EQ(variants[3]->Info().version, 1);

  EXPECT_NE(variants[4]->Info().model_id.find("-cpu:"), std::string::npos);
}

// ========================================================================
// Integration — sorted variants affect which is the "selected" variant
// ========================================================================

TEST_F(ModelSortingTest, SelectedVariant_IsBestAfterSort) {
  SortTestCatalog catalog(logger_);
  // Add GPU v1 first, then NPU v1. After sort, NPU should be first in the
  // container, so the selected variant (container's Info()) should be NPU.
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi"));
  catalog.AddModel(MakeModel("phi", "-npu", 1, "phi"));

  auto* m = catalog.GetModel("phi");
  ASSERT_NE(m, nullptr);
  EXPECT_NE(m->Info().model_id.find("-npu:"), std::string::npos);
}

TEST_F(ModelSortingTest, MultipleAliasGroups_SortedIndependently) {
  SortTestCatalog catalog(logger_);
  // phi: GPU v1, NPU v1 → NPU should be selected
  catalog.AddModel(MakeModel("phi", "-gpu", 1, "phi"));
  catalog.AddModel(MakeModel("phi", "-npu", 1, "phi"));
  // llama: CPU v2, GPU v1 → GPU should be selected
  catalog.AddModel(MakeModel("llama", "-cpu", 2, "llama"));
  catalog.AddModel(MakeModel("llama", "-gpu", 1, "llama"));

  auto* phi = catalog.GetModel("phi");
  ASSERT_NE(phi, nullptr);
  EXPECT_NE(phi->Info().model_id.find("-npu:"), std::string::npos);

  auto* llama = catalog.GetModel("llama");
  ASSERT_NE(llama, nullptr);
  EXPECT_NE(llama->Info().model_id.find("-gpu:"), std::string::npos);
}
