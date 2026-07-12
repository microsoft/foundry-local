// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for BaseModelCatalog's lookup methods after variant grouping:
//   - ListModels: returns Model containers (one per alias group)
//   - GetModel: by name/alias (returns Model container)
//   - GetModelVariant: by model_id (returns specific variant)
//   - GetCachedModels / GetLoadedModels
//   - Variant grouping behavior
//
#include "catalog/base_model_catalog.h"
#include "internal_api/test_helpers.h"
#include "logger.h"
#include "model.h"
#include "model_info.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

using namespace fl;

static Model MakeModel(const std::string& model_id, const std::string& name,
                       int version, const std::string& alias,
                       const std::string& local_path);

// ========================================================================
// Concrete test catalog — returns canned models from FetchModels()
// ========================================================================

class TestCatalog : public BaseModelCatalog {
 public:
  explicit TestCatalog(ILogger& logger) : BaseModelCatalog("test-catalog", logger) {}

  void AddModel(Model model) {
    models_.push_back(std::move(model));
  }

 protected:
  std::vector<Model> FetchModels() const override {
    return std::move(models_);
  }

 private:
  mutable std::vector<Model> models_;
};

class QueryingTestCatalog : public BaseModelCatalog {
 public:
  explicit QueryingTestCatalog(ILogger& logger) : BaseModelCatalog("querying-test-catalog", logger) {}

  void AddModel(Model model) {
    models_.push_back(std::move(model));
  }

  void SetVersionFetchResults(std::vector<Model> models) {
    version_fetch_results_ = std::move(models);
  }

  void SetIdFetchResults(std::vector<Model> models) {
    id_fetch_results_ = std::move(models);
  }

 protected:
  std::vector<Model> FetchModels() const override {
    return std::move(models_);
  }

  std::vector<Model> FetchModelVersions(const std::string& model_alias,
                                        const std::string& model_name = "") const override {
    std::vector<Model> result;
    for (const auto& model : version_fetch_results_) {
      const auto& info = model.Info();
      if (info.alias != model_alias) {
        continue;
      }

      if (!model_name.empty() && info.name != model_name) {
        continue;
      }

      result.push_back(MakeModel(info.model_id, info.name, info.version, info.alias, model.LocalPath()));
    }

    return result;
  }

  std::vector<Model> FetchModelsByIds(const std::vector<std::string>& model_ids) const override {
    std::unordered_set<std::string> requested(model_ids.begin(), model_ids.end());
    std::vector<Model> result;
    for (const auto& model : id_fetch_results_) {
      const auto& info = model.Info();
      if (!requested.contains(info.model_id)) {
        continue;
      }

      result.push_back(MakeModel(info.model_id, info.name, info.version, info.alias, model.LocalPath()));
    }

    return result;
  }

 private:
  mutable std::vector<Model> models_;
  mutable std::vector<Model> version_fetch_results_;
  mutable std::vector<Model> id_fetch_results_;
};

// Helper: create a Model from basic fields.
static Model MakeModel(const std::string& model_id, const std::string& name,
                       int version, const std::string& alias,
                       const std::string& local_path = {}) {
  static fl::test::FakeServiceBindings svc;
  ModelInfo info;
  info.model_id = model_id;
  info.name = name;
  info.version = version;
  info.alias = alias;
  return Model::FromModelInfo(std::move(info), local_path,
                              svc.download_manager, svc.model_load_manager);
}

// ========================================================================
// Test fixture
// ========================================================================

class BaseModelCatalogTest : public ::testing::Test {
 protected:
  StderrLogger logger_;
};

// ========================================================================
// GetName
// ========================================================================

TEST_F(BaseModelCatalogTest, GetName_ReturnsNameFromConstruction) {
  TestCatalog catalog(logger_);
  EXPECT_EQ(catalog.GetName(), "test-catalog");
}

// ========================================================================
// GetModel
// ========================================================================

TEST_F(BaseModelCatalogTest, GetModel_ByName_ReturnsNullptr) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));

  // GetModel only matches by alias, not by name.
  EXPECT_EQ(catalog.GetModel("phi-3-mini"), nullptr);
}

TEST_F(BaseModelCatalogTest, GetModel_ByAlias) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));

  Model* m = catalog.GetModel("phi-3");
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->Info().model_id, "phi-3-mini:1");
}

TEST_F(BaseModelCatalogTest, GetModel_Nonexistent_ReturnsNullptr) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));

  EXPECT_EQ(catalog.GetModel("nonexistent"), nullptr);
}

TEST_F(BaseModelCatalogTest, GetModel_EmptyCatalog_ReturnsNullptr) {
  TestCatalog catalog(logger_);

  EXPECT_EQ(catalog.GetModel("anything"), nullptr);
}

// ========================================================================
// GetModelVariant
// ========================================================================

TEST_F(BaseModelCatalogTest, GetModelVariant_ById) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));

  Model* m = catalog.GetModelVariant("phi-3-mini:1");
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->Info().model_id, "phi-3-mini:1");
}

TEST_F(BaseModelCatalogTest, GetModelVariant_Nonexistent_ReturnsNullptr) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));

  EXPECT_EQ(catalog.GetModelVariant("nonexistent"), nullptr);
}

// ========================================================================
// GetModel — variants accessible via container
// ========================================================================

TEST_F(BaseModelCatalogTest, GetModel_VariantsAccessible) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  catalog.AddModel(MakeModel("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));

  Model* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  EXPECT_EQ(container->Variants().size(), 2u);
}

TEST_F(BaseModelCatalogTest, GetModel_NotFound_ReturnsNullptr) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));

  EXPECT_EQ(catalog.GetModel("nonexistent-alias"), nullptr);
}

TEST_F(BaseModelCatalogTest, GetModel_EmptyString_ReturnsNullptr) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));

  EXPECT_EQ(catalog.GetModel(""), nullptr);
}

// ========================================================================
// ListModels — returns grouped Model containers
// ========================================================================

TEST_F(BaseModelCatalogTest, ListModels_ReturnsGroupedByAlias) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("a:1", "a", 1, "a"));
  catalog.AddModel(MakeModel("b:1", "b", 1, "b"));

  auto list = catalog.ListModels();
  EXPECT_EQ(list.size(), 2u);
}

TEST_F(BaseModelCatalogTest, ListModels_VariantsGroupedIntoSingleModel) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  catalog.AddModel(MakeModel("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));

  // Two variants with same alias → one Model container
  auto list = catalog.ListModels();
  EXPECT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0]->Alias(), "phi-3");

  // The Model container has 2 variants
  const auto& variants = list[0]->Variants();
  EXPECT_EQ(variants.size(), 2u);
}

TEST_F(BaseModelCatalogTest, ListModels_MultipleAliasGroups) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  catalog.AddModel(MakeModel("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));
  catalog.AddModel(MakeModel("llama:1", "llama", 1, "llama"));

  // 2 alias groups: phi-3 (2 variants) and llama (1 variant)
  auto list = catalog.ListModels();
  EXPECT_EQ(list.size(), 2u);
}

// ========================================================================
// BuildFromVariants — models with missing fields are skipped
// ========================================================================

TEST_F(BaseModelCatalogTest, GetModel_SkipsInvalidEntries) {
  // Model with empty model_id should be skipped during grouping
  ModelInfo invalid_info;
  invalid_info.model_id = "";  // missing
  invalid_info.name = "bad";
  invalid_info.alias = "bad-alias";

  TestCatalog catalog(logger_);
  fl::test::FakeServiceBindings svc;
  catalog.AddModel(Model::FromModelInfo(invalid_info, "",
                                        svc.download_manager, svc.model_load_manager));
  catalog.AddModel(MakeModel("good:1", "good", 1, "good-alias"));

  // Invalid model is skipped during grouping — only the valid model is listed
  auto list = catalog.ListModels();
  EXPECT_EQ(list.size(), 1u);

  // Only the valid model is findable by id/alias
  EXPECT_NE(catalog.GetModelVariant("good:1"), nullptr);
  EXPECT_EQ(catalog.GetModel("bad"), nullptr);
}

// ========================================================================
// Variant grouping — cached variant preference
// ========================================================================

TEST_F(BaseModelCatalogTest, GroupedModel_PrefersCachedVariant) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3:1", "phi-3", 1, "phi-3"));                           // not cached
  catalog.AddModel(MakeModel("phi-3:2", "phi-3", 2, "phi-3", "/path/to/cached/model"));  // cached

  auto* m = catalog.GetModel("phi-3");
  ASSERT_NE(m, nullptr);

  // The selected variant should be the cached one (phi-3:2)
  EXPECT_TRUE(m->IsCached());
  EXPECT_EQ(m->Info().model_id, "phi-3:2");
}

TEST_F(BaseModelCatalogTest, GetModelVariant_ById_ReturnsVariantNotContainer) {
  TestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  catalog.AddModel(MakeModel("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));

  // Looking up by id returns the specific variant
  Model* v1 = catalog.GetModelVariant("phi-3-mini:1");
  ASSERT_NE(v1, nullptr);
  EXPECT_EQ(v1->Info().model_id, "phi-3-mini:1");

  Model* v2 = catalog.GetModelVariant("phi-3-mini:2");
  ASSERT_NE(v2, nullptr);
  EXPECT_EQ(v2->Info().model_id, "phi-3-mini:2");

  // Looking up by alias returns the Model container (delegates to selected variant)
  Model* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  EXPECT_EQ(container->Variants().size(), 2u);
}

TEST_F(BaseModelCatalogTest, GetModelVersionsDoesNotIntegrateFetchedVariants) {
  QueryingTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));

  std::vector<Model> version_results;
  version_results.push_back(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  catalog.SetVersionFetchResults(std::move(version_results));

  auto versions = catalog.GetModelVersions("phi-3", "", 0);
  ASSERT_EQ(versions.size(), 1u);
  EXPECT_EQ(versions[0]->Info().model_id, "phi-3-mini:1");

  auto* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  EXPECT_EQ(container->Variants().size(), 1u)
      << "GetModelVersions should not add fetched versions to the catalog's main indices.";
}

TEST_F(BaseModelCatalogTest, GetModelVersionsCrossAliasPointersRemainValid) {
  QueryingTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  catalog.AddModel(MakeModel("llama:1", "llama", 1, "llama"));

  // Seed version results for both aliases.
  std::vector<Model> phi3_versions;
  phi3_versions.push_back(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  phi3_versions.push_back(MakeModel("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));
  catalog.SetVersionFetchResults(std::move(phi3_versions));

  // First query: phi-3
  auto phi3_result = catalog.GetModelVersions("phi-3", "", 0);
  ASSERT_EQ(phi3_result.size(), 2u);
  Model* phi3_ptr = phi3_result[0];

  // Second query: llama — must not invalidate phi3_ptr.
  std::vector<Model> llama_versions;
  llama_versions.push_back(MakeModel("llama:1", "llama", 1, "llama"));
  llama_versions.push_back(MakeModel("llama:2", "llama", 2, "llama"));
  catalog.SetVersionFetchResults(std::move(llama_versions));

  auto llama_result = catalog.GetModelVersions("llama", "", 0);
  ASSERT_EQ(llama_result.size(), 2u);

  // phi3_ptr must still be alive and accessible.
  EXPECT_EQ(phi3_ptr->Info().alias, "phi-3")
      << "Querying a different alias should not invalidate pointers from a prior GetModelVersions call.";
}

    TEST_F(BaseModelCatalogTest, GetModelVersionsMaxVersionsSelectsLatestRegardlessOfFetchOrder) {
      QueryingTestCatalog catalog(logger_);

      std::vector<Model> version_results;
      // Intentionally unsorted fetch order: v2, v1, v3.
      version_results.push_back(MakeModel("phi-3-mini-generic-cpu:2", "phi-3-mini", 2, "phi-3"));
      version_results.push_back(MakeModel("phi-3-mini-generic-cpu:1", "phi-3-mini", 1, "phi-3"));
      version_results.push_back(MakeModel("phi-3-mini-generic-cpu:3", "phi-3-mini", 3, "phi-3"));
      catalog.SetVersionFetchResults(std::move(version_results));

      auto versions = catalog.GetModelVersions("phi-3", "", /*max_versions=*/1);
      ASSERT_EQ(versions.size(), 1u);
      EXPECT_EQ(versions.front()->Info().version, 3)
      << "max_versions=1 should pick the latest version even when fetch order is arbitrary.";
    }

TEST_F(BaseModelCatalogTest, GetModelVariantIdIntegratesFetchedVariant) {
  QueryingTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));

  std::vector<Model> id_results;
  id_results.push_back(MakeModel("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  catalog.SetIdFetchResults(std::move(id_results));

  auto* fetched = catalog.GetModelVariant("phi-3-mini:1");
  ASSERT_NE(fetched, nullptr);
  EXPECT_EQ(fetched->Info().model_id, "phi-3-mini:1");

  auto* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  EXPECT_EQ(container->Variants().size(), 2u)
      << "ID-based fetches should still integrate so download-specific lookups persist in the catalog.";
}

TEST_F(BaseModelCatalogTest, GetModelVariantIdIntegrationPreservesPriorityOrdering) {
  QueryingTestCatalog catalog(logger_);
  catalog.AddModel(MakeModel("phi-3-mini-generic-cpu:1", "phi-3-mini", 1, "phi-3"));

  std::vector<Model> id_results;
  id_results.push_back(MakeModel("phi-3-mini-npu:1", "phi-3-mini", 1, "phi-3"));
  catalog.SetIdFetchResults(std::move(id_results));

  auto* fetched = catalog.GetModelVariant("phi-3-mini-npu:1");
  ASSERT_NE(fetched, nullptr);

  auto* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  auto variants = container->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_EQ(variants.front()->Info().model_id, "phi-3-mini-npu:1")
      << "Integrated variants should be re-sorted so higher-priority devices stay first.";
}
