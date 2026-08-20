// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for ModelCatalog — the aggregating store behind ICatalog. Fetch lives in
// IModelSources (here a FakeModelSource); the store owns the ModelFactory, groups variants
// by alias, keeps same-model_id variants from different sources visible in deterministic order,
// and resolves by-ID lookups to the preferred source. Also covers SelectDefaultVariant
// precedence, the Unregister / RemoveVariant fallback path, and catalog_source JSON round-trip.
//
#include "catalog/model_catalog.h"
#include "catalog/model_source.h"
#include "internal_api/test_helpers.h"
#include "logger.h"
#include "model.h"
#include "model_info.h"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace fl;

namespace {

ModelInfo MakeInfo(const std::string& model_id, const std::string& name, int version,
                   const std::string& alias, const std::string& local_path = {},
                   CatalogSource source = CatalogSource::kPublic) {
  ModelInfo info;
  info.model_id = model_id;
  info.name = name;
  info.version = version;
  info.alias = alias;
  info.local_path = local_path;
  info.catalog_source = source;
  return info;
}

// A fetch-only IModelSource returning canned ModelInfo. Configure it before the first query.
class FakeModelSource : public IModelSource {
 public:
  explicit FakeModelSource(CatalogSource source = CatalogSource::kPublic, std::string name = "fake-source")
      : source_(source), name_(std::move(name)) {}

  CatalogSource Source() const override { return source_; }
  std::string Name() const override { return name_; }

  std::vector<ModelInfo> FetchModels() const override { return models_; }

  std::vector<ModelInfo> FetchModelVersions(const std::string& model_alias,
                                            const std::string& model_name = "") const override {
    std::vector<ModelInfo> result;
    for (const auto& info : version_results_) {
      if (info.alias != model_alias) {
        continue;
      }
      if (!model_name.empty() && info.name != model_name) {
        continue;
      }
      result.push_back(info);
    }
    return result;
  }

  std::vector<ModelInfo> FetchModelsByIds(const std::vector<std::string>& model_ids) const override {
    const std::unordered_set<std::string> requested(model_ids.begin(), model_ids.end());
    std::vector<ModelInfo> result;
    for (const auto& info : id_results_) {
      if (requested.contains(info.model_id)) {
        result.push_back(info);
      }
    }
    return result;
  }

  void AddModel(ModelInfo info) { models_.push_back(std::move(info)); }
  void SetVersionResults(std::vector<ModelInfo> results) { version_results_ = std::move(results); }
  void SetIdResults(std::vector<ModelInfo> results) { id_results_ = std::move(results); }

 private:
  CatalogSource source_;
  std::string name_;
  std::vector<ModelInfo> models_;
  std::vector<ModelInfo> version_results_;
  std::vector<ModelInfo> id_results_;
};

}  // namespace

// ========================================================================
// Fixture — a default public FakeModelSource plus optional extra sources.
// ========================================================================

class ModelCatalogTest : public ::testing::Test {
 protected:
  ModelCatalogTest() { default_source_ = AddSource(CatalogSource::kPublic); }

  FakeModelSource* AddSource(CatalogSource source, std::string name = "fake-source") {
    auto s = std::make_unique<FakeModelSource>(source, std::move(name));
    auto* ptr = s.get();
    pending_sources_.push_back(std::move(s));
    return ptr;
  }

  void AddModel(ModelInfo info) { default_source_->AddModel(std::move(info)); }

  ModelCatalog& Catalog(const std::string& name = "test-catalog") {
    if (!catalog_) {
      catalog_ = std::make_unique<ModelCatalog>(
          name, std::move(pending_sources_),
          [this](ModelInfo info) {
            return Model::FromModelInfo(std::move(info), svc_.download_manager, svc_.model_load_manager);
          },
          logger_);
    }
    return *catalog_;
  }

  StderrLogger logger_;
  fl::test::FakeServiceBindings svc_;
  std::vector<std::unique_ptr<IModelSource>> pending_sources_;
  FakeModelSource* default_source_ = nullptr;
  std::unique_ptr<ModelCatalog> catalog_;
};

// ========================================================================
// GetName
// ========================================================================

TEST_F(ModelCatalogTest, GetName_ReturnsNameFromConstruction) {
  EXPECT_EQ(Catalog("test-catalog").GetName(), "test-catalog");
}

// ========================================================================
// GetModel
// ========================================================================

TEST_F(ModelCatalogTest, GetModel_ByName_ReturnsNullptr) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  EXPECT_EQ(Catalog().GetModel("phi-3-mini"), nullptr);
}

TEST_F(ModelCatalogTest, GetModel_ByAlias) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  Model* m = Catalog().GetModel("phi-3");
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->Info().model_id, "phi-3-mini:1");
}

TEST_F(ModelCatalogTest, GetModel_Nonexistent_ReturnsNullptr) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  EXPECT_EQ(Catalog().GetModel("nonexistent"), nullptr);
}

TEST_F(ModelCatalogTest, GetModel_EmptyCatalog_ReturnsNullptr) {
  EXPECT_EQ(Catalog().GetModel("anything"), nullptr);
}

// ========================================================================
// GetModelVariant
// ========================================================================

TEST_F(ModelCatalogTest, GetModelVariant_ById) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  Model* m = Catalog().GetModelVariant("phi-3-mini:1");
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->Info().model_id, "phi-3-mini:1");
}

TEST_F(ModelCatalogTest, GetModelVariant_Nonexistent_ReturnsNullptr) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  EXPECT_EQ(Catalog().GetModelVariant("nonexistent"), nullptr);
}

TEST_F(ModelCatalogTest, GetModel_VariantsAccessible) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  AddModel(MakeInfo("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));
  Model* container = Catalog().GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  EXPECT_EQ(container->Variants().size(), 2u);
}

TEST_F(ModelCatalogTest, GetModel_EmptyString_ReturnsNullptr) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  EXPECT_EQ(Catalog().GetModel(""), nullptr);
}

// ========================================================================
// ListModels
// ========================================================================

TEST_F(ModelCatalogTest, ListModels_ReturnsGroupedByAlias) {
  AddModel(MakeInfo("a:1", "a", 1, "a"));
  AddModel(MakeInfo("b:1", "b", 1, "b"));
  EXPECT_EQ(Catalog().ListModels().size(), 2u);
}

TEST_F(ModelCatalogTest, ListModels_VariantsGroupedIntoSingleModel) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  AddModel(MakeInfo("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));
  auto list = Catalog().ListModels();
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0]->Alias(), "phi-3");
  EXPECT_EQ(list[0]->Variants().size(), 2u);
}

TEST_F(ModelCatalogTest, ListModels_MultipleAliasGroups) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  AddModel(MakeInfo("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));
  AddModel(MakeInfo("llama:1", "llama", 1, "llama"));
  EXPECT_EQ(Catalog().ListModels().size(), 2u);
}

// ========================================================================
// Invalid entries are skipped
// ========================================================================

TEST_F(ModelCatalogTest, GetModel_SkipsInvalidEntries) {
  AddModel(MakeInfo("", "bad", 0, "bad-alias"));  // missing model_id
  AddModel(MakeInfo("good:1", "good", 1, "good-alias"));

  auto& catalog = Catalog();
  EXPECT_EQ(catalog.ListModels().size(), 1u);
  EXPECT_NE(catalog.GetModelVariant("good:1"), nullptr);
  EXPECT_EQ(catalog.GetModel("bad"), nullptr);
}

// ========================================================================
// Cached-variant preference
// ========================================================================

TEST_F(ModelCatalogTest, GroupedModel_PrefersCachedVariant) {
  AddModel(MakeInfo("phi-3:1", "phi-3", 1, "phi-3"));
  AddModel(MakeInfo("phi-3:2", "phi-3", 2, "phi-3", "/path/to/cached/model"));

  auto* m = Catalog().GetModel("phi-3");
  ASSERT_NE(m, nullptr);
  EXPECT_TRUE(m->IsCached());
  EXPECT_EQ(m->Info().model_id, "phi-3:2");
}

TEST_F(ModelCatalogTest, GetCachedModelsReturnsEveryCachedLeafInCatalogVariantOrder) {
  AddModel(MakeInfo("alpha:1", "alpha", 1, "alpha", "/cache/alpha-1"));
  AddModel(MakeInfo("alpha:3", "alpha", 3, "alpha"));
  AddModel(MakeInfo("alpha:2", "alpha", 2, "alpha", "/cache/alpha-2"));
  AddModel(MakeInfo("beta:1", "beta", 1, "beta", "/cache/beta-1"));

  auto& catalog = Catalog();
  auto cached = catalog.GetCachedModels();

  ASSERT_EQ(cached.size(), 3u);
  EXPECT_EQ(cached[0]->Info().model_id, "alpha:2");
  EXPECT_EQ(cached[1]->Info().model_id, "alpha:1");
  EXPECT_EQ(cached[2]->Info().model_id, "beta:1");

  for (auto* variant : cached) {
    EXPECT_FALSE(variant->IsContainer());
    EXPECT_TRUE(variant->IsCached());
    EXPECT_EQ(variant, catalog.GetModelVariant(variant->Info().model_id));
  }
}

TEST_F(ModelCatalogTest, GetModelVariant_ById_ReturnsVariantNotContainer) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  AddModel(MakeInfo("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));

  auto& catalog = Catalog();
  Model* v1 = catalog.GetModelVariant("phi-3-mini:1");
  ASSERT_NE(v1, nullptr);
  EXPECT_EQ(v1->Info().model_id, "phi-3-mini:1");

  Model* v2 = catalog.GetModelVariant("phi-3-mini:2");
  ASSERT_NE(v2, nullptr);
  EXPECT_EQ(v2->Info().model_id, "phi-3-mini:2");

  Model* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  EXPECT_EQ(container->Variants().size(), 2u);
}

// ========================================================================
// GetModelVersions / by-id fetch (delegates to sources)
// ========================================================================

TEST_F(ModelCatalogTest, GetModelVersionsDoesNotIntegrateFetchedVariants) {
  AddModel(MakeInfo("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));
  default_source_->SetVersionResults({MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3")});

  auto& catalog = Catalog();
  auto versions = catalog.GetModelVersions("phi-3", "", 0);
  ASSERT_EQ(versions.size(), 1u);
  EXPECT_EQ(versions[0]->Info().model_id, "phi-3-mini:1");

  auto* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  EXPECT_EQ(container->Variants().size(), 1u)
      << "GetModelVersions should not add fetched versions to the catalog's main indices.";
}

TEST_F(ModelCatalogTest, GetModelVersionsCrossAliasPointersRemainValid) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  AddModel(MakeInfo("llama:1", "llama", 1, "llama"));

  default_source_->SetVersionResults({
      MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"),
      MakeInfo("phi-3-mini:2", "phi-3-mini", 2, "phi-3"),
      MakeInfo("llama:1", "llama", 1, "llama"),
      MakeInfo("llama:2", "llama", 2, "llama"),
  });

  auto& catalog = Catalog();
  auto phi3_result = catalog.GetModelVersions("phi-3", "", 0);
  ASSERT_EQ(phi3_result.size(), 2u);
  Model* phi3_ptr = phi3_result[0];

  auto llama_result = catalog.GetModelVersions("llama", "", 0);
  ASSERT_EQ(llama_result.size(), 2u);

  EXPECT_EQ(phi3_ptr->Info().alias, "phi-3")
      << "Querying a different alias should not invalidate pointers from a prior GetModelVersions call.";
}

TEST_F(ModelCatalogTest, GetModelVersionsMaxVersionsSelectsLatestRegardlessOfFetchOrder) {
  default_source_->SetVersionResults({
      MakeInfo("phi-3-mini-generic-cpu:2", "phi-3-mini", 2, "phi-3"),
      MakeInfo("phi-3-mini-generic-cpu:1", "phi-3-mini", 1, "phi-3"),
      MakeInfo("phi-3-mini-generic-cpu:3", "phi-3-mini", 3, "phi-3"),
  });

  auto versions = Catalog().GetModelVersions("phi-3", "", /*max_versions=*/1);
  ASSERT_EQ(versions.size(), 1u);
  EXPECT_EQ(versions.front()->Info().version, 3)
      << "max_versions=1 should pick the latest version even when fetch order is arbitrary.";
}

TEST_F(ModelCatalogTest, GetModelVariantIdIntegratesFetchedVariant) {
  AddModel(MakeInfo("phi-3-mini:2", "phi-3-mini", 2, "phi-3"));
  default_source_->SetIdResults({MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3")});

  auto& catalog = Catalog();
  auto* fetched = catalog.GetModelVariant("phi-3-mini:1");
  ASSERT_NE(fetched, nullptr);
  EXPECT_EQ(fetched->Info().model_id, "phi-3-mini:1");

  auto* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  EXPECT_EQ(container->Variants().size(), 2u)
      << "ID-based fetches should still integrate so download-specific lookups persist in the catalog.";
}

TEST_F(ModelCatalogTest, GetModelVariantIdIntegrationPreservesPriorityOrdering) {
  AddModel(MakeInfo("phi-3-mini-generic-cpu:1", "phi-3-mini", 1, "phi-3"));
  default_source_->SetIdResults({MakeInfo("phi-3-mini-npu:1", "phi-3-mini", 1, "phi-3")});

  auto& catalog = Catalog();
  auto* fetched = catalog.GetModelVariant("phi-3-mini-npu:1");
  ASSERT_NE(fetched, nullptr);

  auto* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  auto variants = container->Variants();
  ASSERT_EQ(variants.size(), 2u);
  EXPECT_EQ(variants.front()->Info().model_id, "phi-3-mini-npu:1")
      << "Integrated variants should be re-sorted so higher-priority devices stay first.";
}

// ========================================================================
// Multi-source: union of aliases, duplicate variants, preference, visibility
// ========================================================================

TEST_F(ModelCatalogTest, MultipleSourcesUnionAliases) {
  default_source_->AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  auto* local = AddSource(CatalogSource::kLocal, "local-source");
  local->AddModel(MakeInfo("llama:1", "llama", 1, "llama", "/cache/llama", CatalogSource::kLocal));

  auto list = Catalog().ListModels();
  EXPECT_EQ(list.size(), 2u);
}

TEST_F(ModelCatalogTest, DuplicateModelIdsRemainVisibleInSortedSourceOrderAndByIdPrefersSource) {
  default_source_->AddModel(
      MakeInfo("phi-3-mini-gpu:2", "phi-3-mini", 2, "phi-3", "", CatalogSource::kPublic));
  default_source_->AddModel(
      MakeInfo("phi-3-mini-cpu:1", "phi-3-mini", 1, "phi-3", "", CatalogSource::kPublic));
  default_source_->AddModel(
      MakeInfo("phi-3-mini-gpu:1", "phi-3-mini", 1, "phi-3", "", CatalogSource::kPublic));

  auto* local = AddSource(CatalogSource::kLocal, "local-source");
  local->AddModel(
      MakeInfo("phi-3-mini-gpu:1", "phi-3-mini", 1, "phi-3", "", CatalogSource::kLocal));

  auto* priv = AddSource(CatalogSource::kPrivate, "private-source");
  priv->AddModel(
      MakeInfo("phi-3-mini-gpu:1", "phi-3-mini", 1, "phi-3", "", CatalogSource::kPrivate));

  auto& catalog = Catalog();
  auto* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);

  const std::vector<std::pair<std::string, CatalogSource>> expected = {
      {"phi-3-mini-gpu:2", CatalogSource::kPublic},
      {"phi-3-mini-gpu:1", CatalogSource::kLocal},
      {"phi-3-mini-gpu:1", CatalogSource::kPrivate},
      {"phi-3-mini-gpu:1", CatalogSource::kPublic},
      {"phi-3-mini-cpu:1", CatalogSource::kPublic},
  };

  const auto variants = container->Variants();
  ASSERT_EQ(variants.size(), expected.size());

  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(variants[i]->Info().model_id, expected[i].first);
    EXPECT_EQ(variants[i]->Info().catalog_source, expected[i].second);
  }

  auto* by_id = catalog.GetModelVariant("phi-3-mini-gpu:1");
  ASSERT_NE(by_id, nullptr);
  EXPECT_EQ(by_id->Info().catalog_source, CatalogSource::kLocal)
      << "By-ID lookup should still resolve duplicate model IDs to the preferred source.";
}

TEST_F(ModelCatalogTest, IdIndexResolvesToPreferredSource) {
  default_source_->AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  auto* priv = AddSource(CatalogSource::kPrivate, "private-source");
  priv->AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3", "", CatalogSource::kPrivate));

  auto* variant = Catalog().GetModelVariant("phi-3-mini:1");
  ASSERT_NE(variant, nullptr);
  EXPECT_EQ(variant->Info().catalog_source, CatalogSource::kPrivate)
      << "private ranks above public, so by-id lookup resolves to the private shadow.";
}

TEST_F(ModelCatalogTest, SelectDefaultVariantCachedBeatsSourcePreference) {
  // Preferred-source (private) copy is uncached; lower-priority (public) copy is cached.
  default_source_->AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3", "/cache/public"));
  auto* priv = AddSource(CatalogSource::kPrivate, "private-source");
  priv->AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3", "", CatalogSource::kPrivate));

  auto* container = Catalog().GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  EXPECT_TRUE(container->IsCached());
  EXPECT_EQ(container->Info().catalog_source, CatalogSource::kPublic)
      << "cached beats source preference for the container's default selection.";
}

// ========================================================================
// Unregister / RemoveVariant fallback
// ========================================================================

TEST_F(ModelCatalogTest, UnregisterRemovesShadowAndReSelectsSurvivor) {
  default_source_->AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  auto* local = AddSource(CatalogSource::kLocal, "local-source");
  local->AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3", "/cache/phi", CatalogSource::kLocal));

  auto& catalog = catalog_ ? *catalog_ : Catalog();
  auto* container = catalog.GetModel("phi-3");
  ASSERT_NE(container, nullptr);
  ASSERT_EQ(container->Variants().size(), 2u);

  // The by-id lookup and the local shadow both resolve to the local (preferred) copy.
  EXPECT_EQ(catalog.GetModelVariant("phi-3-mini:1")->Info().catalog_source, CatalogSource::kLocal);

  catalog.Unregister("phi-3-mini:1");

  // The local shadow is gone; the surviving public copy is now the by-id result.
  auto* survivor = catalog.GetModelVariant("phi-3-mini:1");
  ASSERT_NE(survivor, nullptr);
  EXPECT_EQ(survivor->Info().catalog_source, CatalogSource::kPublic);
  EXPECT_EQ(container->Variants().size(), 1u);
}

TEST_F(ModelCatalogTest, UnregisterLastVariantRemovesContainer) {
  AddModel(MakeInfo("solo:1", "solo", 1, "solo", "/cache/solo", CatalogSource::kLocal));

  auto& catalog = Catalog();
  ASSERT_NE(catalog.GetModel("solo"), nullptr);

  catalog.Unregister("solo:1");

  EXPECT_EQ(catalog.GetModel("solo"), nullptr);
  EXPECT_EQ(catalog.GetModelVariant("solo:1"), nullptr);
  EXPECT_TRUE(catalog.ListModels().empty());
}

TEST_F(ModelCatalogTest, UnregisterUnknownIdThrows) {
  AddModel(MakeInfo("phi-3-mini:1", "phi-3-mini", 1, "phi-3"));
  auto& catalog = Catalog();
  catalog.ListModels();  // force populate
  EXPECT_THROW(catalog.Unregister("does-not-exist:1"), std::exception);
}

// ========================================================================
// catalog_source round-trips through the cache JSON
// ========================================================================

TEST_F(ModelCatalogTest, CatalogSourceRoundTripsThroughJson) {
  for (auto source : {CatalogSource::kPublic, CatalogSource::kPrivate, CatalogSource::kLocal}) {
    ModelInfo info = MakeInfo("m:1", "m", 1, "m");
    info.catalog_source = source;
    const auto restored = ModelInfoFromJson(ModelInfoToJson(info));
    EXPECT_EQ(restored.catalog_source, source);
  }
}

TEST_F(ModelCatalogTest, CatalogSourceAbsentFromJsonDecodesAsPublic) {
  nlohmann::json j = ModelInfoToJson(MakeInfo("m:1", "m", 1, "m"));
  EXPECT_FALSE(j.contains("catalogSource")) << "public is the default and is not serialized.";
  EXPECT_EQ(ModelInfoFromJson(j).catalog_source, CatalogSource::kPublic);
}
