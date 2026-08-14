// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_catalog.h"

#include "internal_api/test_helpers.h"
#include "utils/temp_path.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>

namespace fl::test {
namespace {

class LocalModelCatalogTest : public ::testing::Test {
 protected:
  LocalModelCatalogTest()
      : root_(TempPath::CreateTempDir("local_model_catalog")),
        model_dir_(root_.path() / "model"),
        catalog_(root_.path() / "appdata",
                 [this](ModelInfo info, std::string path, std::function<void(const std::string&)> unregister_callback,
               std::function<std::optional<ModelInfo>()> prepare_callback) {
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
  EXPECT_TRUE(std::filesystem::exists(model_dir_ / "model_metadata.yml"));
  const auto index_path = root_.path() / "appdata" / "catalogs" / "local" / "local_models.json";
  ASSERT_TRUE(std::filesystem::exists(index_path));
  nlohmann::json index;
  std::ifstream(index_path) >> index;
  EXPECT_EQ(index["version"], 1);
  ASSERT_EQ(index["models"].size(), 1u);
  EXPECT_TRUE(index["models"][0].contains("properties"));
  EXPECT_FALSE(index["models"][0].contains("supplied_properties"));
  EXPECT_TRUE(index["models"][0].contains("metadata_prepared"));
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
            std::function<std::optional<ModelInfo>()> prepare_callback) {
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
              std::function<std::optional<ModelInfo>()> prepare_callback) {
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

TEST_F(LocalModelCatalogTest, DeferredWhisperAssetsRefreshLiveAndPersistedMetadata) {
  const auto deferred_path = root_.path() / "deferred-whisper";
  ModelInfo info;
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, deferred_path.string());
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_ALIAS, "deferred-whisper");
  auto* model = catalog_.RegisterModel(info);
  const auto* original_info = &model->Info();
  EXPECT_EQ(original_info->task, "chat-completion");

  std::filesystem::create_directories(deferred_path);
  std::ofstream(deferred_path / "genai_config.json")
      << R"({"model":{"type":"whisper","context_length":448}})";

  EXPECT_TRUE(model->IsCached());
  EXPECT_EQ(model->Info().task, "automatic-speech-recognition");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, std::string{}),
            "audio");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}), 448);
  EXPECT_EQ(original_info->task, "chat-completion");

  LocalModelCatalog restored(
      root_.path() / "appdata",
      [this](ModelInfo restored_info, std::string path,
             std::function<void(const std::string&)> unregister_callback,
              std::function<std::optional<ModelInfo>()> prepare_callback) {
        return Model::FromLocalRegistration(std::move(restored_info), std::move(path), bindings_.download_manager,
                                            bindings_.model_load_manager, std::move(unregister_callback),
                                            std::move(prepare_callback));
      },
      NullLog());
  auto restored_models = restored.ListModels();
  ASSERT_EQ(restored_models.size(), 1u);
  EXPECT_EQ(restored_models.front()->Info().task, "automatic-speech-recognition");
}

TEST_F(LocalModelCatalogTest, ExistingEmptyDirectoryStillRefreshesWhenAssetsAppear) {
  const auto deferred_path = root_.path() / "existing-deferred-whisper";
  std::filesystem::create_directories(deferred_path);
  ModelInfo info;
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, deferred_path.string());
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_ALIAS, "existing-deferred-whisper");
  auto* model = catalog_.RegisterModel(info);
  EXPECT_TRUE(std::filesystem::exists(deferred_path / "model_metadata.yml"));
  EXPECT_EQ(model->Info().task, "chat-completion");

  std::ofstream(deferred_path / "genai_config.json")
    << R"({"model":{"type":"whisper","context_length":448}})";

  EXPECT_TRUE(model->IsCached());
  EXPECT_EQ(model->Info().task, "automatic-speech-recognition");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, std::string{}),
      "audio");
}

TEST_F(LocalModelCatalogTest, DeferredAssetsAddedWhileStoppedRefreshAfterRestore) {
  const auto deferred_path = root_.path() / "stopped-deferred-whisper";
  ModelInfo info;
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, deferred_path.string());
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_ALIAS, "stopped-deferred-whisper");
  catalog_.RegisterModel(info);

  std::filesystem::create_directories(deferred_path);
  std::ofstream(deferred_path / "genai_config.json")
      << R"({"model":{"type":"whisper","context_length":448}})";

  LocalModelCatalog restored(
      root_.path() / "appdata",
      [this](ModelInfo restored_info, std::string path,
             std::function<void(const std::string&)> unregister_callback,
             std::function<std::optional<ModelInfo>()> prepare_callback) {
        return Model::FromLocalRegistration(std::move(restored_info), std::move(path), bindings_.download_manager,
                                            bindings_.model_load_manager, std::move(unregister_callback),
                                            std::move(prepare_callback));
      },
      NullLog());
  auto models = restored.ListModels();
  ASSERT_EQ(models.size(), 1u);

  EXPECT_TRUE(models.front()->IsCached());
  EXPECT_EQ(models.front()->Info().task, "automatic-speech-recognition");
  EXPECT_EQ(models.front()->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}),
            448);
}

TEST_F(LocalModelCatalogTest, RestoreRepairsMissingMetadataSidecar) {
  catalog_.RegisterModel(MakeInfo());
  ASSERT_TRUE(std::filesystem::remove(model_dir_ / "model_metadata.yml"));

  LocalModelCatalog restored(
      root_.path() / "appdata",
      [this](ModelInfo restored_info, std::string path,
             std::function<void(const std::string&)> unregister_callback,
             std::function<std::optional<ModelInfo>()> prepare_callback) {
        return Model::FromLocalRegistration(std::move(restored_info), std::move(path), bindings_.download_manager,
                                            bindings_.model_load_manager, std::move(unregister_callback),
                                            std::move(prepare_callback));
      },
      NullLog());
  auto models = restored.ListModels();
  ASSERT_EQ(models.size(), 1u);

  EXPECT_TRUE(models.front()->IsCached());
  EXPECT_TRUE(std::filesystem::exists(model_dir_ / "model_metadata.yml"));
  EXPECT_TRUE(models.front()->IsCached());
}

TEST_F(LocalModelCatalogTest, MalformedDeferredConfigRetriesAfterCorrection) {
  const auto deferred_path = root_.path() / "malformed-deferred-whisper";
  ModelInfo info;
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, deferred_path.string());
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_ALIAS, "malformed-deferred-whisper");
  SetModelInfoIntProperty(info, FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_BYTES_INT, 1234);
  auto* model = catalog_.RegisterModel(info);

  std::filesystem::create_directories(deferred_path);
  std::ofstream(deferred_path / "genai_config.json") << R"({"model":)";
  EXPECT_TRUE(model->IsCached());
  EXPECT_EQ(model->Info().task, "chat-completion");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_BYTES_INT, int64_t{-1}), 1234);

  std::ofstream(deferred_path / "genai_config.json", std::ios::trunc)
      << R"({"model":{"type":"whisper","context_length":448}})";
  EXPECT_TRUE(model->IsCached());
  EXPECT_EQ(model->Info().task, "automatic-speech-recognition");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}), 448);
  EXPECT_NE(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_BYTES_INT, int64_t{-1}), 1234);
}

TEST_F(LocalModelCatalogTest, RegistrationWithoutPreparationStateRefreshesFromAssets) {
  const auto model_path = root_.path() / "old-model";
  const auto catalog_dir = root_.path() / "old-appdata" / "catalogs" / "local";
  std::filesystem::create_directories(catalog_dir);
  nlohmann::json properties = {
    {FOUNDRY_LOCAL_REG_MODEL_PATH, model_path.string()},
    {FOUNDRY_LOCAL_REG_ALIAS, "old-model"},
    {FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion"},
    {"_local_registration_id", "old-model-registration"},
  };
  nlohmann::json index = {
    {"version", 1},
    {"catalog_name", "local"},
    {"models", {{{"alias", "old-model"}, {"model_path", model_path.string()}, {"properties", properties}}}},
  };
  std::ofstream(catalog_dir / "local_models.json") << index.dump(2);

  LocalModelCatalog restored(
      root_.path() / "old-appdata",
      [this](ModelInfo restored_info, std::string path,
             std::function<void(const std::string&)> unregister_callback,
             std::function<std::optional<ModelInfo>()> prepare_callback) {
        return Model::FromLocalRegistration(std::move(restored_info), std::move(path), bindings_.download_manager,
                                            bindings_.model_load_manager, std::move(unregister_callback),
                                            std::move(prepare_callback));
      },
        NullLog());
      auto models = restored.ListModels();
      ASSERT_EQ(models.size(), 1u);

      std::filesystem::create_directories(model_path);
      std::ofstream(model_path / "genai_config.json")
        << R"({"model":{"type":"whisper","context_length":448}})";
      EXPECT_TRUE(models.front()->IsCached());
      EXPECT_EQ(models.front()->Info().task, "automatic-speech-recognition");
      EXPECT_EQ(models.front()->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}),
          448);
}

TEST_F(LocalModelCatalogTest, IgnoresRestoredRegistrationWithDuplicateStableId) {
  const auto catalog_dir = root_.path() / "duplicate-id-appdata" / "catalogs" / "local";
  std::filesystem::create_directories(catalog_dir);
  const auto make_properties = [&](const std::string& alias) {
    return nlohmann::json{
        {FOUNDRY_LOCAL_REG_MODEL_PATH, (root_.path() / alias).string()},
        {FOUNDRY_LOCAL_REG_ALIAS, alias},
        {FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion"},
        {"_local_registration_id", "duplicate-registration-id"},
    };
  };
  nlohmann::json index = {
      {"version", 1},
      {"catalog_name", "local"},
      {"models",
       {{{"alias", "first"}, {"model_path", (root_.path() / "first").string()}, {"properties", make_properties("first")}},
        {{"alias", "second"},
         {"model_path", (root_.path() / "second").string()},
         {"properties", make_properties("second")}}}},
  };
  std::ofstream(catalog_dir / "local_models.json") << index.dump(2);

  LocalModelCatalog restored(
      root_.path() / "duplicate-id-appdata",
      [this](ModelInfo restored_info, std::string path,
             std::function<void(const std::string&)> unregister_callback,
             std::function<std::optional<ModelInfo>()> prepare_callback) {
        return Model::FromLocalRegistration(std::move(restored_info), std::move(path), bindings_.download_manager,
                                            bindings_.model_load_manager, std::move(unregister_callback),
                                            std::move(prepare_callback));
      },
      NullLog());

  auto models = restored.ListModels();
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front()->Alias(), "first");
}

    TEST_F(LocalModelCatalogTest, DeferredEmbeddingsAssetsOverrideRuntimeMetadataAndPreserveDescription) {
  const auto deferred_path = root_.path() / "deferred-embeddings";
  ModelInfo info;
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, deferred_path.string());
  SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_ALIAS, "deferred-embeddings");
  SetModelInfoIntProperty(info, FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, 1234);
      SetModelInfoStringProperty(info, FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "automatic-speech-recognition");
      SetModelInfoStringProperty(info, FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, "audio");
      SetModelInfoStringProperty(info, FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR, "My Embeddings Model");
      SetModelInfoStringProperty(info, FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR, "MIT");
  auto* model = catalog_.RegisterModel(info);

  std::filesystem::create_directories(deferred_path);
  std::ofstream(deferred_path / "genai_config.json")
      << R"({"model":{"type":"bert","hidden_size":384,"context_length":512}})";

  EXPECT_TRUE(model->IsCached());
  EXPECT_EQ(model->Info().task, "embeddings");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}), 512);
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, std::string{}),
            "language");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR, std::string{}),
            "My Embeddings Model");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR, std::string{}), "MIT");
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
