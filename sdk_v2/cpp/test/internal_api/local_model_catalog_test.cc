// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_catalog.h"

#include "internal_api/test_helpers.h"
#include "utils/temp_path.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fl::test {
namespace {

class LocalModelCatalogTest : public ::testing::Test {
 protected:
  LocalModelCatalogTest()
      : root_(TempPath::CreateTempDir("local_model_catalog")),
        model_dir_(root_.path() / "model"),
        catalog_(MakeCatalog()) {
    WriteConfig(model_dir_);
  }

  LocalModelCatalog MakeCatalog() {
    return LocalModelCatalog(
        root_.path() / "appdata",
        [this](ModelInfo info, std::string path, std::string runtime_model_id) {
          return Model::FromLocalRegistration(std::move(info), std::move(path), bindings_.download_manager,
                                              bindings_.model_load_manager, std::move(runtime_model_id));
        },
        NullLog());
  }

  static void WriteConfig(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    std::ofstream(path / "genai_config.json") << R"({"model":{"type":"phi3","context_length":4096}})";
  }

  ModelInfo MakeInfo(std::string alias = "my-model", std::string task = "chat-completion") const {
    ModelInfo info;
    SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, model_dir_.string());
    SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_ALIAS, std::move(alias));
    SetModelInfoStringProperty(info, FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, std::move(task));
    return info;
  }

  TempPath root_;
  std::filesystem::path model_dir_;
  FakeServiceBindings bindings_;
  LocalModelCatalog catalog_;
};

TEST_F(LocalModelCatalogTest, RegisterPreservesCallerMetadataAndWritesOnlyAppDataIndex) {
  auto info = MakeInfo();
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, "text,image");
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, "text");
  SetModelInfoIntProperty(info, FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT, 321);

  auto* model = catalog_.RegisterModel(info);

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), "my-model:0");
  EXPECT_EQ(model->Info().task, "chat-completion");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, std::string{}),
            "text,image");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, std::string{}),
            "text");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT, int64_t{-1}), 321);
  EXPECT_TRUE(model->IsCached());
  EXPECT_FALSE(std::filesystem::exists(model_dir_ / "model_metadata.yml"));

  const auto index_path = root_.path() / "appdata" / "catalogs" / "local" / "local_models.json";
  ASSERT_TRUE(std::filesystem::exists(index_path));
  nlohmann::json index;
  std::ifstream(index_path) >> index;
  ASSERT_EQ(index["models"].size(), 1u);
  EXPECT_EQ(index["models"][0]["model_id"], "my-model:0");
  EXPECT_FALSE(index["models"][0].contains("metadata_prepared"));
}

TEST_F(LocalModelCatalogTest, RegistrationRequiresExistingDirectoryAndParseableConfig) {
  auto missing = MakeInfo();
  SetModelInfoStringProperty(missing, FOUNDRY_LOCAL_REG_MODEL_PATH, (root_.path() / "missing").string());
  EXPECT_THROW(catalog_.RegisterModel(missing), Exception);

  const auto no_config_path = root_.path() / "no-config";
  std::filesystem::create_directories(no_config_path);
  auto no_config = MakeInfo("no-config");
  SetModelInfoStringProperty(no_config, FOUNDRY_LOCAL_REG_MODEL_PATH, no_config_path.string());
  EXPECT_THROW(catalog_.RegisterModel(no_config), Exception);

  const auto malformed_path = root_.path() / "malformed";
  std::filesystem::create_directories(malformed_path);
  std::ofstream(malformed_path / "genai_config.json") << R"({"model":)";
  auto malformed = MakeInfo("malformed");
  SetModelInfoStringProperty(malformed, FOUNDRY_LOCAL_REG_MODEL_PATH, malformed_path.string());
  EXPECT_THROW(catalog_.RegisterModel(malformed), Exception);
}

TEST_F(LocalModelCatalogTest, RegistrationRequiresSupportedTask) {
  ModelInfo missing_task;
  SetModelInfoStringProperty(missing_task, FOUNDRY_LOCAL_REG_MODEL_PATH, model_dir_.string());
  SetModelInfoStringProperty(missing_task, FOUNDRY_LOCAL_REG_ALIAS, "missing-task");
  EXPECT_THROW(catalog_.RegisterModel(missing_task), Exception);
  EXPECT_THROW(catalog_.RegisterModel(MakeInfo("invalid-task", "text-generation")), Exception);
}

TEST_F(LocalModelCatalogTest, UnregisterPersistsAndPreservesAssets) {
  auto* stale = catalog_.RegisterModel(MakeInfo());
  catalog_.UnregisterModel("my-model:0");

  EXPECT_TRUE(catalog_.ListModels().empty());
  EXPECT_TRUE(std::filesystem::exists(model_dir_ / "genai_config.json"));
  EXPECT_THROW(stale->Load(), Exception);
  EXPECT_NO_THROW(stale->Unload());
  auto restored = MakeCatalog();
  EXPECT_TRUE(restored.ListModels().empty());
}

TEST_F(LocalModelCatalogTest, UnregisterWriteFailureLeavesModelActiveAndUsable) {
  auto* model = catalog_.RegisterModel(MakeInfo());
  ASSERT_NE(model, nullptr);

  const auto temp_index_path = root_.path() / "appdata" / "catalogs" / "local" / "local_models.json.tmp";
  std::filesystem::create_directory(temp_index_path);

  EXPECT_THROW(catalog_.UnregisterModel("my-model"), Exception);
  EXPECT_TRUE(model->IsActive());
  EXPECT_EQ(catalog_.GetModelVariant("my-model:0"), model);

  float progress = 0.0f;
  EXPECT_NO_THROW(model->Download([&progress](float value) {
    progress = value;
    return 0;
  }));
  EXPECT_EQ(progress, 100.0f);
}

TEST_F(LocalModelCatalogTest, TwoCatalogsReconcileRegisterUnregisterAndReregister) {
  auto second = MakeCatalog();
  EXPECT_TRUE(second.ListModels().empty());

  auto* stale = catalog_.RegisterModel(MakeInfo());
  ASSERT_NE(stale, nullptr);
  ASSERT_EQ(second.ListModels().size(), 1u);

  second.UnregisterModel("my-model");
  EXPECT_TRUE(catalog_.ListModels().empty());
  EXPECT_FALSE(stale->IsActive());
  EXPECT_THROW(stale->Download(), Exception);
  EXPECT_THROW(stale->Load(), Exception);
  EXPECT_THROW(stale->RemoveFromCache(), Exception);

  auto* replacement = second.RegisterModel(MakeInfo());
  ASSERT_NE(replacement, nullptr);
  EXPECT_NE(replacement->RuntimeId(), stale->RuntimeId());
  ASSERT_EQ(catalog_.ListModels().size(), 1u);
  EXPECT_NE(catalog_.GetModelVariant("my-model:0"), stale);
}

TEST_F(LocalModelCatalogTest, PublicCatalogContractRejectsRegistration) {
  class ReadOnlyCatalog final : public ICatalog {
   public:
    const std::string& GetName() const override { return name_; }
    std::vector<Model*> ListModels() const override { return {}; }
    Model* GetModel(const std::string&) const override { return nullptr; }
    Model* GetModelVariant(const std::string&) const override { return nullptr; }
    Model* GetLatestVersion(const Model*) const override { return nullptr; }
    std::vector<Model*> GetModelVersions(const std::string&, const std::string&, int) override { return {}; }
    std::vector<Model*> GetCachedModels() const override { return {}; }
    std::vector<Model*> GetLoadedModels() const override { return {}; }

   private:
    std::string name_ = "public";
  } catalog;

  EXPECT_THROW(catalog.RegisterModel(MakeInfo()), Exception);
}

}  // namespace
}  // namespace fl::test
