// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for cache-only / external-service-url mode using only the public C++ API.
// Each test creates its own Manager instance because Manager is a singleton —
// the previous one must be destroyed before creating a new one.
//

#include <foundry_local/foundry_local_cpp.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
// Windows headers define StartService as a macro (StartServiceA/StartServiceW).
// Undefine it so we can call foundry_local::Manager::StartService() directly.
#undef StartService
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ========================================================================
// Test fixture — creates a unique temp directory with an optional cache file
// ========================================================================

class CacheOnlyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = (fs::temp_directory_path() / ("fl_cache_only_test_" + std::to_string(GetPid()))).string();
    fs::create_directories(test_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  /// Write a JSON cache file containing two known models.
  void WriteTwoModelCacheFile() {
    auto j = BuildCacheJson({
        MakeModelJson("phi-4-mini-instruct-generic-cpu:2",
                      "phi-4-mini-instruct-generic-cpu",
                      /*version=*/2,
                      "phi-4-mini-instruct",
                      "chat-completion",
                      "Microsoft",
                      "Phi-4 Mini Instruct"),
        MakeModelJson("qwen2.5-0.5b-instruct-generic-cpu:1",
                      "qwen2.5-0.5b-instruct-generic-cpu",
                      /*version=*/1,
                      "qwen2.5-0.5b-instruct",
                      "chat-completion",
                      "Alibaba",
                      "Qwen 2.5 0.5B Instruct"),
    });

    WriteFile(CacheFilePath(), j.dump(2));
  }

  /// Write an empty-but-valid cache file (zero models).
  void WriteEmptyCacheFile() {
    auto j = BuildCacheJson({});
    WriteFile(CacheFilePath(), j.dump(2));
  }

  std::string CacheFilePath() const {
    return (fs::path(test_dir_) / "foundry.modelinfo.json").string();
  }

  /// Build a Configuration pointing at the temp dir in cache-only mode.
  foundry_local::Configuration MakeCacheOnlyConfig() {
    foundry_local::Configuration config("cache_only_test");
    config.SetModelCacheDir(test_dir_)
        .SetExternalServiceUrl("http://127.0.0.1:12345");
    return config;
  }

  std::string CreateLocalModel(const std::string& model_id, const std::string& directory_name) {
    auto model_directory = fs::path(test_dir_) / "Microsoft" / directory_name;
    fs::create_directories(model_directory);
    WriteFile((model_directory / "genai_config.json").string(), "{}");
    WriteFile((model_directory / "inference_model.json").string(),
              nlohmann::json({{"Name", model_id}}).dump());
    return model_directory.string();
  }

  std::string test_dir_;

 private:
  static nlohmann::json MakeModelJson(const std::string& id,
                                      const std::string& name,
                                      int version,
                                      const std::string& alias,
                                      const std::string& task,
                                      const std::string& publisher,
                                      const std::string& display_name) {
    return {
        {"id", id},
        {"name", name},
        {"version", version},
        {"alias", alias},
        {"uri", "azureml://registries/azureml/models/" + name + "/versions/" + std::to_string(version)},
        {"providerType", "FoundryLocal"},
        {"modelType", "ONNX"},
        {"task", task},
        {"publisher", publisher},
        {"displayName", display_name},
    };
  }

  static nlohmann::json BuildCacheJson(std::initializer_list<nlohmann::json> models) {
    nlohmann::json j;
    j["version"] = 1;
    j["savedAtUnix"] = 1713800000;
    j["models"] = nlohmann::json::array();

    for (const auto& m : models) {
      j["models"].push_back(m);
    }

    return j;
  }

  static void WriteFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    ASSERT_TRUE(f.is_open()) << "Failed to open cache file for writing: " << path;
    f << content;
  }

#ifdef _WIN32
  static DWORD GetPid() { return ::GetCurrentProcessId(); }
#else
  static pid_t GetPid() { return ::getpid(); }
#endif
};

// ========================================================================
// Tests
// ========================================================================

TEST_F(CacheOnlyTest, CatalogReturnsModelsFromCacheFile) {
  WriteTwoModelCacheFile();

  {
    foundry_local::Manager manager(MakeCacheOnlyConfig());
    auto& catalog = manager.GetCatalog();
    auto model_list = catalog.GetModels();
    const auto& models = model_list;

    ASSERT_EQ(models.size(), 2u);

    // Verify first model fields.
    auto info0 = models[0]->GetInfo();
    EXPECT_EQ(info0.Alias(), "phi-4-mini-instruct");
    EXPECT_EQ(info0.Id(), "phi-4-mini-instruct-generic-cpu:2");
    EXPECT_EQ(info0.Name(), "phi-4-mini-instruct-generic-cpu");
    EXPECT_EQ(info0.Version(), 2);
    EXPECT_EQ(info0.Task().value_or(""), "chat-completion");

    // Verify second model fields.
    auto info1 = models[1]->GetInfo();
    EXPECT_EQ(info1.Alias(), "qwen2.5-0.5b-instruct");
    EXPECT_EQ(info1.Id(), "qwen2.5-0.5b-instruct-generic-cpu:1");
    EXPECT_EQ(info1.Name(), "qwen2.5-0.5b-instruct-generic-cpu");
    EXPECT_EQ(info1.Version(), 1);
    EXPECT_EQ(info1.Task().value_or(""), "chat-completion");
  }
}

TEST_F(CacheOnlyTest, CatalogGetModelByAliasWorksInCacheOnlyMode) {
  WriteTwoModelCacheFile();

  {
    foundry_local::Manager manager(MakeCacheOnlyConfig());
    auto& catalog = manager.GetCatalog();
    auto model = catalog.GetModel("phi-4-mini-instruct");

    ASSERT_NE(model, nullptr) << "Expected GetModel to find 'phi-4-mini-instruct' in cache";

    auto info = model->GetInfo();
    EXPECT_EQ(info.Alias(), "phi-4-mini-instruct");
    EXPECT_EQ(info.Id(), "phi-4-mini-instruct-generic-cpu:2");
    EXPECT_EQ(info.Name(), "phi-4-mini-instruct-generic-cpu");
    EXPECT_EQ(info.Version(), 2);
    EXPECT_EQ(info.Task().value_or(""), "chat-completion");
  }
}

TEST_F(CacheOnlyTest, CatalogGetModelVariantByIdWorksInCacheOnlyMode) {
  WriteTwoModelCacheFile();

  {
    foundry_local::Manager manager(MakeCacheOnlyConfig());
    auto& catalog = manager.GetCatalog();
    auto variant = catalog.GetModelVariant("qwen2.5-0.5b-instruct-generic-cpu:1");

    ASSERT_NE(variant, nullptr)
        << "Expected GetModelVariant to find 'qwen2.5-0.5b-instruct-generic-cpu:1' in cache";

    auto info = variant->GetInfo();
    EXPECT_EQ(info.Id(), "qwen2.5-0.5b-instruct-generic-cpu:1");
    EXPECT_EQ(info.Alias(), "qwen2.5-0.5b-instruct");
    EXPECT_EQ(info.Version(), 1);
  }
}

TEST_F(CacheOnlyTest, CatalogGetModelReturnsNulloptForUnknownAlias) {
  WriteTwoModelCacheFile();

  {
    foundry_local::Manager manager(MakeCacheOnlyConfig());
    auto& catalog = manager.GetCatalog();
    auto model = catalog.GetModel("nonexistent");

    EXPECT_EQ(model, nullptr);
  }
}

TEST_F(CacheOnlyTest, StartServiceThrowsInCacheOnlyMode) {
  WriteTwoModelCacheFile();

  {
    foundry_local::Manager manager(MakeCacheOnlyConfig());

    EXPECT_THROW(manager.StartWebService(), std::exception);
  }
}

TEST_F(CacheOnlyTest, EmptyCacheFileReturnsEmptyModelList) {
  WriteEmptyCacheFile();

  {
    foundry_local::Manager manager(MakeCacheOnlyConfig());
    auto& catalog = manager.GetCatalog();
    auto model_list = catalog.GetModels();

    EXPECT_TRUE(model_list.empty());
  }
}

TEST_F(CacheOnlyTest, MissingCacheFileReturnsEmptyModelList) {
  // Don't create any cache file — the temp dir exists but is empty.

  {
    foundry_local::Manager manager(MakeCacheOnlyConfig());
    auto& catalog = manager.GetCatalog();
    auto model_list = catalog.GetModels();

    EXPECT_TRUE(model_list.empty());
  }
}

TEST_F(CacheOnlyTest, SnapshotAndByomModelsUseScannedLocalPaths) {
  WriteTwoModelCacheFile();
  const auto snapshot_model_path =
      CreateLocalModel("phi-4-mini-instruct-generic-cpu:2", "phi-snapshot-model");
  const auto byom_model_path = CreateLocalModel("contoso-custom-model:7", "contoso-byom-model");

  {
    foundry_local::Manager manager(MakeCacheOnlyConfig());
    auto& catalog = manager.GetCatalog();
    auto models = catalog.GetModels();
    auto cached_models = catalog.GetCachedModels();

    ASSERT_EQ(models.size(), 3u);
    ASSERT_EQ(cached_models.size(), 2u);

    auto snapshot_model = catalog.GetModelVariant("phi-4-mini-instruct-generic-cpu:2");
    ASSERT_NE(snapshot_model, nullptr);
    EXPECT_TRUE(snapshot_model->IsCached());
    EXPECT_EQ(std::string(snapshot_model->GetPath()), snapshot_model_path);
    EXPECT_EQ(snapshot_model->GetInfo().Publisher().value_or(""), "Microsoft");

    auto byom_model = catalog.GetModelVariant("contoso-custom-model:7");
    ASSERT_NE(byom_model, nullptr);
    EXPECT_TRUE(byom_model->IsCached());
    EXPECT_EQ(std::string(byom_model->GetPath()), byom_model_path);
    EXPECT_EQ(byom_model->GetInfo().Name(), "contoso-custom-model");
    EXPECT_EQ(byom_model->GetInfo().Alias(), "contoso-custom-model");
    EXPECT_EQ(byom_model->GetInfo().Version(), 7);
    EXPECT_EQ(byom_model->GetInfo().Uri(), "local://contoso-custom-model");
    EXPECT_EQ(byom_model->GetInfo().ModelProvider().value_or(""), "Local");
    EXPECT_EQ(byom_model->GetInfo().ModelType().value_or(""), "ONNX");
  }
}
