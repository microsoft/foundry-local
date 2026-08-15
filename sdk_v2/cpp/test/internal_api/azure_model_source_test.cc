// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for AzureModelSource — the Public (Azure) catalog source (fetch guts ported from the
// former AzureModelCatalog). A source returns ModelInfo only: it stamps catalog_source, attaches
// each cached entry's transient local_path, synthesizes kLocal stubs for disk-only models, falls
// back to the on-disk snapshot when every live URL fails, and saves the live snapshot.

#include "catalog/azure_model_source.h"
#include "catalog/catalog_cache.h"
#include "catalog/catalog_client.h"
#include "internal_api/test_helpers.h"
#include "model_info.h"
#include "utils/temp_path.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
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

class TestAzureModelSource final : public AzureModelSource {
 public:
  using ClientFactory =
      std::function<std::unique_ptr<ICatalogClient>(const std::string& url, const std::string& filter)>;

  TestAzureModelSource(std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
                       std::string cache_dir,
                       const IEpDetector& ep_detector,
                       ILogger& logger,
                       bool cache_only,
                       ClientFactory client_factory)
      : AzureModelSource(std::move(catalog_urls), std::move(cache_dir), ep_detector, logger, cache_only),
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

const ModelInfo* FindInfo(const std::vector<ModelInfo>& infos, const std::string& model_id) {
  for (const auto& info : infos) {
    if (info.model_id == model_id) {
      return &info;
    }
  }

  return nullptr;
}

}  // namespace

class AzureModelSourceTest : public ::testing::Test {
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

  std::unique_ptr<AzureModelSource> CreateSource(
      std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls,
      bool cache_only = false) {
    auto client_factory = [this](const std::string& url, const std::string&) {
      ++client_creations_;
      auto behavior = behaviors_.find(url);
      if (behavior == behaviors_.end()) {
        throw std::runtime_error("missing fake behavior for " + url);
      }

      return std::make_unique<FakeCatalogClient>(behavior->second);
    };

    return std::make_unique<TestAzureModelSource>(std::move(catalog_urls), cache_directory_.string(),
                                                  services_.ep_detector, services_.logger, cache_only,
                                                  std::move(client_factory));
  }

  fl::test::TempPath cache_directory_ = fl::test::TempPath::CreateTempDir("fl_azure_model_source_");
  fl::test::FakeServiceBindings services_;
  std::unordered_map<std::string, std::shared_ptr<CatalogBehavior>> behaviors_;
  int client_creations_ = 0;
};

TEST_F(AzureModelSourceTest, AllUrlsFailUsesSnapshotMetadataAndScannedPathsWithoutRewritingSnapshot) {
  const auto gpu_info = MakeModelInfo("snapshot-gpu:2", "snapshot-gpu", 2, "snapshot-alias", "SnapshotProvider");
  const auto cpu_info = MakeModelInfo("snapshot-cpu:2", "snapshot-cpu", 2, "snapshot-alias", "SnapshotProvider");
  WriteSnapshot({gpu_info, cpu_info});
  const auto gpu_path = AddLocalModel("snapshot-gpu:2", "snapshot-gpu");
  const auto cpu_path = AddLocalModel("snapshot-cpu:2", "snapshot-cpu");
  const auto byom_path = AddLocalModel("offline-byom:4", "offline-byom");

  AddBehavior("https://catalog-one.test", true);
  AddBehavior("https://catalog-two.test", true);
  auto source = CreateSource({
      {"https://catalog-one.test", std::nullopt},
      {"https://catalog-two.test", std::nullopt},
  });

  const auto infos = source->FetchModels();

  ASSERT_EQ(infos.size(), 3u);
  const auto* gpu = FindInfo(infos, "snapshot-gpu:2");
  ASSERT_NE(gpu, nullptr);
  EXPECT_EQ(gpu->alias, "snapshot-alias");
  EXPECT_EQ(gpu->catalog_source, CatalogSource::kPublic);
  const auto* provider = gpu->GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR);
  ASSERT_NE(provider, nullptr);
  EXPECT_EQ(*provider, "SnapshotProvider");
  EXPECT_EQ(gpu->local_path, gpu_path.string());

  const auto* cpu = FindInfo(infos, "snapshot-cpu:2");
  ASSERT_NE(cpu, nullptr);
  EXPECT_EQ(cpu->local_path, cpu_path.string());

  const auto* byom = FindInfo(infos, "offline-byom:4");
  ASSERT_NE(byom, nullptr);
  EXPECT_EQ(byom->catalog_source, CatalogSource::kLocal);
  EXPECT_EQ(byom->local_path, byom_path.string());

  CatalogCache persisted_cache(cache_directory_.string(), services_.logger);
  persisted_cache.Load();
  const auto persisted_models = persisted_cache.GetCachedModels();
  ASSERT_TRUE(persisted_models.has_value());
  ASSERT_EQ(persisted_models->size(), 2u);
  EXPECT_EQ((*persisted_models)[0].model_id, "snapshot-gpu:2");
  EXPECT_EQ((*persisted_models)[1].model_id, "snapshot-cpu:2");
  EXPECT_EQ(client_creations_, 2);
}

TEST_F(AzureModelSourceTest, AllUrlsFailWithoutSnapshotSurfacesScannedModelAsByom) {
  const auto local_path = AddLocalModel("custom-model:0", "custom-model");
  AddBehavior("https://catalog-one.test", true);
  AddBehavior("https://catalog-two.test", true);
  auto source = CreateSource({
      {"https://catalog-one.test", std::nullopt},
      {"https://catalog-two.test", std::nullopt},
  });

  const auto infos = source->FetchModels();

  ASSERT_EQ(infos.size(), 1u);
  const auto* byom = FindInfo(infos, "custom-model:0");
  ASSERT_NE(byom, nullptr);
  EXPECT_EQ(byom->name, "custom-model");
  EXPECT_EQ(byom->alias, "custom-model");
  EXPECT_EQ(byom->version, 0);
  EXPECT_EQ(byom->uri, "local://custom-model");
  EXPECT_EQ(byom->catalog_source, CatalogSource::kLocal);
  const auto* provider = byom->GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR);
  const auto* model_type = byom->GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR);
  ASSERT_NE(provider, nullptr);
  ASSERT_NE(model_type, nullptr);
  EXPECT_EQ(*provider, "Local");
  EXPECT_EQ(*model_type, "ONNX");
  EXPECT_EQ(byom->local_path, local_path.string());
  EXPECT_FALSE(fs::exists(cache_directory_.path() / "foundry.modelinfo.json"));
}

TEST_F(AzureModelSourceTest, EmptySuccessfulUrlPreventsSnapshotFallbackWhenAnotherUrlFails) {
  WriteSnapshot({MakeModelInfo("snapshot-only:1", "snapshot-only", 1, "snapshot-only", "SnapshotProvider")});
  AddBehavior("https://failed-catalog.test", true);
  const auto successful_behavior = AddBehavior("https://empty-catalog.test");
  auto source = CreateSource({
      {"https://failed-catalog.test", std::nullopt},
      {"https://empty-catalog.test", std::nullopt},
  });

  const auto infos = source->FetchModels();

  EXPECT_TRUE(infos.empty());
  EXPECT_EQ(successful_behavior->fetch_all_calls, 1);
  EXPECT_EQ(client_creations_, 2);
}

TEST_F(AzureModelSourceTest, CacheOnlyUsesSnapshotAndScannedByomWithoutCreatingLiveClient) {
  WriteSnapshot({MakeModelInfo("snapshot-model:3", "snapshot-model", 3, "snapshot-alias", "SnapshotProvider")});
  AddLocalModel("snapshot-model:3", "snapshot-model");
  AddLocalModel("cache-only-byom:5", "cache-only-byom");
  auto source = CreateSource({{"https://must-not-be-called.test", std::nullopt}}, true);

  const auto infos = source->FetchModels();

  ASSERT_EQ(infos.size(), 2u);
  EXPECT_NE(FindInfo(infos, "snapshot-model:3"), nullptr);
  EXPECT_NE(FindInfo(infos, "cache-only-byom:5"), nullptr);
  EXPECT_EQ(client_creations_, 0);
}

TEST_F(AzureModelSourceTest, LiveAggregationDeduplicatesAndSavesResolvedAndByomMetadata) {
  AddLocalModel("old-model:1", "old-model");
  AddLocalModel("custom-model:0", "custom-model");

  const auto latest_info = MakeModelInfo("latest-model:2", "latest-model", 2, "latest-model", "LiveProvider");
  const auto old_info = MakeModelInfo("old-model:1", "old-model", 1, "old-model", "ResolvedProvider");

  const auto first_behavior = AddBehavior("https://catalog-one.test");
  first_behavior->all_models = {latest_info};
  first_behavior->models_by_id = {old_info};
  const auto second_behavior = AddBehavior("https://catalog-two.test");
  second_behavior->all_models = {latest_info};

  auto source = CreateSource({
      {"https://catalog-one.test", std::nullopt},
      {"https://catalog-two.test", std::nullopt},
  });

  const auto infos = source->FetchModels();

  ASSERT_EQ(infos.size(), 3u);
  EXPECT_EQ(first_behavior->fetch_by_id_calls, 1);
  EXPECT_EQ(second_behavior->fetch_by_id_calls, 1);
  EXPECT_NE(FindInfo(infos, "latest-model:2"), nullptr);
  EXPECT_NE(FindInfo(infos, "old-model:1"), nullptr);

  const auto* byom = FindInfo(infos, "custom-model:0");
  ASSERT_NE(byom, nullptr);
  EXPECT_EQ(byom->catalog_source, CatalogSource::kLocal);

  CatalogCache persisted_cache(cache_directory_.string(), services_.logger);
  persisted_cache.Load();
  const auto persisted_models = persisted_cache.GetCachedModels();
  ASSERT_TRUE(persisted_models.has_value());
  ASSERT_EQ(persisted_models->size(), 3u);

  std::unordered_set<std::string> persisted_ids;
  for (const auto& info : *persisted_models) {
    persisted_ids.insert(info.model_id);
  }

  EXPECT_EQ(persisted_ids,
            (std::unordered_set<std::string>{"latest-model:2", "old-model:1", "custom-model:0"}));
}
