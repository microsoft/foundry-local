// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_catalog.h"

#include "internal_api/test_helpers.h"
#include "utils/temp_path.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fl::test {
namespace {

class LocalModelCatalogTest : public ::testing::Test {
 protected:
  LocalModelCatalogTest()
      : root_(TempPath::CreateTempDir("local_model_catalog")),
        model_dir_(root_.path() / "model"),
        catalog_(root_.path() / "appdata",
                 [this](ModelInfo info, std::string path, std::function<void(const std::string&)> unregister_callback,
                        std::function<void()> prepare_callback) {
                   return Model::FromLocalRegistration(std::move(info), std::move(path), bindings_.download_manager,
                                                       bindings_.model_load_manager, std::move(unregister_callback),
                                                       std::move(prepare_callback));
                 },
                 NullLog()) {
    std::filesystem::create_directories(model_dir_);
    std::ofstream(model_dir_ / "genai_config.json") << R"({"model":{"type":"phi3","context_length":4096}})";
  }

  ModelInfo MakeInfo(std::string alias = "my-model") const {
    ModelInfo info;
    SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, model_dir_.string());
    SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_ALIAS, std::move(alias));
    return info;
  }

  TempPath root_;
  std::filesystem::path model_dir_;
  FakeServiceBindings bindings_;
  LocalModelCatalog catalog_;
};

TEST_F(LocalModelCatalogTest, RegisterResolvesMetadataListsAndWritesFiles) {
  auto* model = catalog_.RegisterModel(MakeInfo());

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), "my-model:0");
  EXPECT_EQ(model->Alias(), "my-model");
  EXPECT_EQ(model->GetPath(), std::filesystem::absolute(model_dir_).lexically_normal().string());
  EXPECT_TRUE(model->IsCached());
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, -1), 4096);
  EXPECT_EQ(catalog_.ListModels().size(), 1u);
  EXPECT_EQ(catalog_.GetLocalModels().size(), 1u);
  EXPECT_TRUE(std::filesystem::exists(model_dir_ / "model_metadata.yml"));
  EXPECT_TRUE(std::filesystem::exists(root_.path() / "appdata" / "catalogs" / "local" / "local_models.json"));
}

TEST_F(LocalModelCatalogTest, RejectsMissingInvalidAndDuplicateAliases) {
  ModelInfo missing;
  EXPECT_THROW(catalog_.RegisterModel(missing), Exception);
  EXPECT_THROW(catalog_.RegisterModel(MakeInfo("bad alias")), Exception);

  catalog_.RegisterModel(MakeInfo());
  EXPECT_THROW(catalog_.RegisterModel(MakeInfo()), Exception);
}

TEST_F(LocalModelCatalogTest, PersistsAndUnregistersWithoutDeletingAssets) {
  catalog_.RegisterModel(MakeInfo());
  {
    LocalModelCatalog restored(
        root_.path() / "appdata",
        [this](ModelInfo info, std::string path, std::function<void(const std::string&)> unregister_callback,
               std::function<void()> prepare_callback) {
          return Model::FromLocalRegistration(std::move(info), std::move(path), bindings_.download_manager,
                                              bindings_.model_load_manager, std::move(unregister_callback),
                                              std::move(prepare_callback));
        },
        NullLog());
    ASSERT_EQ(restored.ListModels().size(), 1u);
    restored.UnregisterModel("my-model");
    EXPECT_TRUE(restored.ListModels().empty());
  }

  EXPECT_TRUE(std::filesystem::exists(model_dir_ / "genai_config.json"));
  LocalModelCatalog reloaded(
      root_.path() / "appdata",
      [this](ModelInfo info, std::string path, std::function<void(const std::string&)> unregister_callback,
             std::function<void()> prepare_callback) {
        return Model::FromLocalRegistration(std::move(info), std::move(path), bindings_.download_manager,
                                            bindings_.model_load_manager, std::move(unregister_callback),
                                            std::move(prepare_callback));
      },
      NullLog());
  EXPECT_TRUE(reloaded.ListModels().empty());
}

TEST_F(LocalModelCatalogTest, MissingDirectoryRemainsListedButIsNotCached) {
  catalog_.RegisterModel(MakeInfo());
  std::filesystem::remove_all(model_dir_);

  ASSERT_EQ(catalog_.ListModels().size(), 1u);
  EXPECT_TRUE(catalog_.GetCachedModels().empty());
}

TEST_F(LocalModelCatalogTest, RegistrationDoesNotValidateMissingModelDirectory) {
  const auto missing_path = root_.path() / "not-yet-provisioned";
  ModelInfo info;
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, missing_path.string());
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_ALIAS, "deferred-model");

  auto* model = catalog_.RegisterModel(info);

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), "deferred-model:0");
  EXPECT_FALSE(model->IsCached());
  EXPECT_EQ(catalog_.ListModels().size(), 1u);
  EXPECT_TRUE(catalog_.GetCachedModels().empty());
  EXPECT_FALSE(std::filesystem::exists(missing_path));

  std::filesystem::create_directories(missing_path);
  std::ofstream(missing_path / "genai_config.json") << R"({"model":{"type":"phi3"}})";
  EXPECT_TRUE(model->IsCached());
  EXPECT_TRUE(std::filesystem::exists(missing_path / "model_metadata.yml"));
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
