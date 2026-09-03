// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "catalog/azure_model_catalog.h"
#include "catalog/catalog_cache.h"
#include "catalog/catalog_client.h"
#include "internal_api/test_helpers.h"
#include "model.h"
#include "model_info.h"
#include "utils/temp_path.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace fl;

namespace {

namespace fs = std::filesystem;

struct CatalogBehavior {
  bool fail_fetch_all = false;
  std::vector<ModelInfo> all_models;
  std::vector<ModelInfo> models_by_id;
  int fetch_all_calls = 0;
  int fetch_by_id_calls = 0;
  std::vector<std::string> last_requested_ids;
};

class FakeCatalogClient final : public ICatalogClient {
 public:
  explicit FakeCatalogClient(std::shared_ptr<CatalogBehavior> behavior) : behavior_(std::move(behavior)) {}

  std::vector<ModelInfo> FetchAllModelInfos() override {
    ++behavior_->fetch_all_calls;
    if (behavior_->fail_fetch_all) {
      throw std::runtime_error("configured catalog failure");
    }

    return behavior_->all_models;
  }

  std::vector<ModelInfo> FetchModelsByIds(const std::vector<std::string>& model_ids) override {
    ++behavior_->fetch_by_id_calls;
    behavior_->last_requested_ids = model_ids;

    const std::unordered_set<std::string> requested_ids(model_ids.begin(), model_ids.end());
    std::vector<ModelInfo> result;
    for (const auto& info : behavior_->models_by_id) {
      if (requested_ids.contains(info.model_id)) {
        result.push_back(info);
      }
    }

    return result;
  }

 private:
  std::shared_ptr<CatalogBehavior> behavior_;
};

class TestAzureModelCatalog final : public AzureModelCatalog {
 public:
  using ClientFactory =
      std::function<std::unique_ptr<ICatalogClient>(const std::string& url, const std::string& filter)>;

  TestAzureModelCatalog(std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
                        std::string cache_dir,
                        ModelFactory model_factory,
                        const IEpDetector& ep_detector,
                        ILogger& logger,
                        bool cache_only,
                        ClientFactory client_factory)
      : AzureModelCatalog(std::move(catalog_urls), std::move(cache_dir), std::move(model_factory), ep_detector, logger,
                          cache_only),
        client_factory_(std::move(client_factory)) {}

 protected:
  std::unique_ptr<ICatalogClient> CreateCatalogClient(const std::string& url,
                                                      const std::string& filter) const override {
    return client_factory_(url, filter);
  }

 private:
  ClientFactory client_factory_;
};

ModelInfo MakeModelInfo(const std::string& model_id,
                        const std::string& name,
                        int version,
                        const std::string& alias,
                        const std::string& provider) {
  ModelInfo info;
  info.model_id = model_id;
  info.name = name;
  info.version = version;
  info.alias = alias;
  info.uri = "https://example.test/" + model_id;
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR] = provider;
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR] = "ONNX";
  return info;
}

Model* FindVariant(const std::vector<Model*>& models, const std::string& model_id) {
  for (auto* model : models) {
    for (auto* variant : model->Variants()) {
      if (variant->Info().model_id == model_id) {
        return variant;
      }
    }
  }

  return nullptr;
}

}  // namespace

class AzureModelCatalogTest : public ::testing::Test {
 protected:
  std::shared_ptr<CatalogBehavior> AddBehavior(const std::string& url, bool fail_fetch_all = false) {
    auto behavior = std::make_shared<CatalogBehavior>();
    behavior->fail_fetch_all = fail_fetch_all;
    behaviors_[url] = behavior;
    return behavior;
  }

  fs::path AddLocalModel(const std::string& model_id, const std::string& directory_name) {
    auto model_directory = cache_directory_.path() / "Microsoft" / directory_name;
    fs::create_directories(model_directory);

    {
      std::ofstream genai_config(model_directory / "genai_config.json");
      genai_config << "{}";
    }

    {
      std::ofstream inference_model(model_directory / "inference_model.json");
      inference_model << nlohmann::json({{"Name", model_id}}).dump();
    }

    return model_directory;
  }

  void WriteSnapshot(const std::vector<ModelInfo>& model_infos) {
    nlohmann::json models = nlohmann::json::array();
    for (const auto& info : model_infos) {
      models.push_back(ModelInfoToJson(info));
    }

    const nlohmann::json snapshot = {
        {"version", 1},
        {"savedAtUnix", 0},
        {"models", std::move(models)},
    };

    std::ofstream file(cache_directory_.path() / "foundry.modelinfo.json");
    ASSERT_TRUE(file.is_open());
    file << snapshot.dump(2);
  }

  std::unique_ptr<AzureModelCatalog> CreateCatalog(
      std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
      bool cache_only = false) {
    auto model_factory = [this](ModelInfo info, std::string local_path) {
      return Model::FromModelInfo(std::move(info), std::move(local_path), services_.download_manager,
                                  services_.model_load_manager);
    };
    auto client_factory = [this](const std::string& url, const std::string&) {
      ++factory_calls_;
      auto behavior = behaviors_.find(url);
      if (behavior == behaviors_.end()) {
        throw std::runtime_error("missing fake behavior for " + url);
      }

      return std::make_unique<FakeCatalogClient>(behavior->second);
    };

    return std::make_unique<TestAzureModelCatalog>(std::move(catalog_urls), cache_directory_.string(),
                                                   std::move(model_factory), services_.ep_detector, services_.logger,
                                                   cache_only, std::move(client_factory));
  }

  fl::test::TempPath cache_directory_ = fl::test::TempPath::CreateTempDir("fl_azure_model_catalog_");
  fl::test::FakeServiceBindings services_;
  std::unordered_map<std::string, std::shared_ptr<CatalogBehavior>> behaviors_;
  int factory_calls_ = 0;
};

TEST_F(AzureModelCatalogTest, AllUrlsFailUsesSnapshotMetadataAndScannedPathsWithoutRewritingSnapshot) {
  const auto gpu_info =
      MakeModelInfo("snapshot-gpu:2", "snapshot-gpu", 2, "snapshot-alias", "SnapshotProvider");
  const auto cpu_info =
      MakeModelInfo("snapshot-cpu:2", "snapshot-cpu", 2, "snapshot-alias", "SnapshotProvider");
  WriteSnapshot({gpu_info, cpu_info});
  const auto gpu_path = AddLocalModel("snapshot-gpu:2", "snapshot-gpu");
  const auto cpu_path = AddLocalModel("snapshot-cpu:2", "snapshot-cpu");
  AddLocalModel("offline-byom:4", "offline-byom");

  AddBehavior("https://catalog-one.test", true);
  AddBehavior("https://catalog-two.test", true);
  auto catalog = CreateCatalog({
      {"https://catalog-one.test", std::nullopt},
      {"https://catalog-two.test", std::nullopt},
  });

  const auto cached_models = catalog->GetCachedModels();

  ASSERT_EQ(cached_models.size(), 2u);
  auto* gpu_model = FindVariant(cached_models, "snapshot-gpu:2");
  ASSERT_NE(gpu_model, nullptr);
  EXPECT_EQ(gpu_model->Info().alias, "snapshot-alias");
  const auto* provider = gpu_model->Info().GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR);
  ASSERT_NE(provider, nullptr);
  EXPECT_EQ(*provider, "SnapshotProvider");
  EXPECT_EQ(gpu_model->LocalPath(), gpu_path.string());

  auto* cpu_model = FindVariant(cached_models, "snapshot-cpu:2");
  ASSERT_NE(cpu_model, nullptr);
  EXPECT_EQ(cpu_model->Info().alias, "snapshot-alias");
  EXPECT_EQ(cpu_model->LocalPath(), cpu_path.string());

  EXPECT_EQ(FindVariant(cached_models, "offline-byom:4"), nullptr);

  CatalogCache persisted_cache(cache_directory_.string(), services_.logger);
  persisted_cache.Load();
  const auto persisted_models = persisted_cache.GetCachedModels();
  ASSERT_TRUE(persisted_models.has_value());
  ASSERT_EQ(persisted_models->size(), 2u);
  EXPECT_EQ((*persisted_models)[0].model_id, "snapshot-gpu:2");
  EXPECT_EQ((*persisted_models)[1].model_id, "snapshot-cpu:2");
  EXPECT_EQ(factory_calls_, 2);
}

TEST_F(AzureModelCatalogTest, AllUrlsFailWithoutSnapshotIgnoresUnknownScannedModel) {
  AddLocalModel("custom-model:0", "custom-model");
  AddBehavior("https://catalog-one.test", true);
  AddBehavior("https://catalog-two.test", true);
  auto catalog = CreateCatalog({
      {"https://catalog-one.test", std::nullopt},
      {"https://catalog-two.test", std::nullopt},
  });

  const auto cached_models = catalog->GetCachedModels();

  EXPECT_TRUE(cached_models.empty());
  EXPECT_EQ(catalog->GetModelVariant("custom-model:0"), nullptr);
  EXPECT_FALSE(fs::exists(cache_directory_.path() / "foundry.modelinfo.json"));
}

TEST_F(AzureModelCatalogTest, EmptySuccessfulUrlPreventsSnapshotFallbackWhenAnotherUrlFails) {
  WriteSnapshot({MakeModelInfo("snapshot-only:1", "snapshot-only", 1, "snapshot-only", "SnapshotProvider")});
  AddBehavior("https://failed-catalog.test", true);
  const auto successful_behavior = AddBehavior("https://empty-catalog.test");
  auto catalog = CreateCatalog({
      {"https://failed-catalog.test", std::nullopt},
      {"https://empty-catalog.test", std::nullopt},
  });

  const auto models = catalog->ListModels();

  EXPECT_TRUE(models.empty());
  EXPECT_EQ(successful_behavior->fetch_all_calls, 1);
  EXPECT_EQ(factory_calls_, 2);
}

TEST_F(AzureModelCatalogTest, CacheOnlyUsesSnapshotPathAndIgnoresUnknownScannedModelWithoutCreatingLiveClient) {
  WriteSnapshot({MakeModelInfo("snapshot-model:3", "snapshot-model", 3, "snapshot-alias", "SnapshotProvider")});
  AddLocalModel("snapshot-model:3", "snapshot-model");
  AddLocalModel("cache-only-byom:5", "cache-only-byom");
  auto catalog = CreateCatalog({{"https://must-not-be-called.test", std::nullopt}}, true);

  const auto cached_models = catalog->GetCachedModels();

  ASSERT_EQ(cached_models.size(), 1u);
  EXPECT_NE(FindVariant(cached_models, "snapshot-model:3"), nullptr);
  EXPECT_EQ(catalog->GetModelVariant("cache-only-byom:5"), nullptr);
  EXPECT_EQ(factory_calls_, 0);
}

TEST_F(AzureModelCatalogTest, LiveAggregationDeduplicatesAndSavesOnlyResolvedPublicMetadata) {
  AddLocalModel("old-model:1", "old-model");
  AddLocalModel("custom-model:0", "custom-model");

  const auto latest_info = MakeModelInfo("latest-model:2", "latest-model", 2, "latest-model", "LiveProvider");
  const auto old_info = MakeModelInfo("old-model:1", "old-model", 1, "old-model", "ResolvedProvider");

  const auto first_behavior = AddBehavior("https://catalog-one.test");
  first_behavior->all_models = {latest_info};
  first_behavior->models_by_id = {old_info};
  const auto second_behavior = AddBehavior("https://catalog-two.test");
  second_behavior->all_models = {latest_info};

  auto catalog = CreateCatalog({
      {"https://catalog-one.test", std::nullopt},
      {"https://catalog-two.test", std::nullopt},
  });

  const auto models = catalog->ListModels();

  ASSERT_EQ(models.size(), 2u);
  EXPECT_EQ(FindVariant(models, "custom-model:0"), nullptr);
  EXPECT_EQ(first_behavior->fetch_by_id_calls, 1);
  EXPECT_EQ(second_behavior->fetch_by_id_calls, 1);

  CatalogCache persisted_cache(cache_directory_.string(), services_.logger);
  persisted_cache.Load();
  const auto persisted_models = persisted_cache.GetCachedModels();
  ASSERT_TRUE(persisted_models.has_value());
  ASSERT_EQ(persisted_models->size(), 2u);

  std::unordered_set<std::string> persisted_ids;
  for (const auto& info : *persisted_models) {
    persisted_ids.insert(info.model_id);
  }

  EXPECT_EQ(persisted_ids, (std::unordered_set<std::string>{"latest-model:2", "old-model:1"}));
}

TEST_F(AzureModelCatalogTest, CacheOnlyIgnoresLegacySynthesizedByomSnapshotEntry) {
  const auto public_info = MakeModelInfo("snapshot-model:3", "snapshot-model", 3, "snapshot-alias",
                                         "SnapshotProvider");
  const auto legacy_byom_info = MakeModelInfo("legacy-byom:1", "legacy-byom", 1, "legacy-byom", "Local");
  WriteSnapshot({public_info, legacy_byom_info});
  AddLocalModel("snapshot-model:3", "snapshot-model");
  AddLocalModel("legacy-byom:1", "legacy-byom");
  auto catalog = CreateCatalog({{"https://must-not-be-called.test", std::nullopt}}, true);

  const auto models = catalog->ListModels();
  const auto cached_models = catalog->GetCachedModels();

  ASSERT_EQ(models.size(), 1u);
  ASSERT_EQ(cached_models.size(), 1u);
  EXPECT_NE(catalog->GetModelVariant("snapshot-model:3"), nullptr);
  EXPECT_EQ(catalog->GetModelVariant("legacy-byom:1"), nullptr);
  EXPECT_EQ(factory_calls_, 0);
}
