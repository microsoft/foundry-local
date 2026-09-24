// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_catalog.h"

#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "utils/temp_path.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
        root_.path() / "cache" / "models",
        [this](ModelInfo info, std::string path) {
          return Model::FromLocalRegistration(std::move(info), std::move(path), bindings_.download_manager,
                                              bindings_.model_load_manager);
        },
        NullLog());
  }

  static void WriteConfig(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    std::ofstream(path / "genai_config.json") << R"({"model":{"type":"phi3","context_length":4096}})";
  }

  ModelInfo MakeMetadata(std::string task = "chat-completion") const {
    ModelInfo info;
    info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, std::move(task));
    return info;
  }

  Model* Register(std::string model_id = "my-model:1") {
    return catalog_.RegisterModel(model_dir_.string(), model_id, MakeMetadata());
  }

  TempPath root_;
  std::filesystem::path model_dir_;
  FakeServiceBindings bindings_;
  LocalModelCatalog catalog_;
};

TEST_F(LocalModelCatalogTest, RegisterResolvesMetadataAndWritesLocalModelInfoCache) {
  auto info = MakeMetadata();
  info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, "text,image");
  info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, "text");
  info.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT, 321);
  info.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, 123);
  info.SetPropertyStr("custom_metadata", "preserved");
  info.SetPropertyInt("custom_count", 42);
  info.prompt_templates.Add("user", "<|user|>{Content}<|end|>");
  info.model_settings.Add("temperature", "0.5");
  info.detected_region = "caller-region";
  info.SetPropertyStr("model_path", "ignored");
  info.SetPropertyStr("alias", "ignored");
  info.SetPropertyStr("version", "ignored");
  info.SetPropertyInt("model_path", 99);
  info.SetPropertyInt("alias", 99);
  info.SetPropertyInt("_local_registration_id", 99);
  info.SetPropertyInt("version", 99);

  auto* model = catalog_.RegisterModel(model_dir_.string(), "my-model-generic-cpu:7", info);

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), "my-model-generic-cpu:7");
  EXPECT_EQ(model->Info().name, "my-model-generic-cpu");
  EXPECT_EQ(model->Info().alias, "my-model");
  EXPECT_EQ(model->Info().version, 7);
  EXPECT_EQ(model->Info().task, "chat-completion");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR, std::string{}),
            "my-model-generic-cpu");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, std::string{}),
            "text,image");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, std::string{}),
            "text");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT, int64_t{-1}), 321);
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}), 4096);
  EXPECT_EQ(model->Info().GetPropertyStr("model_path"), nullptr);
  EXPECT_EQ(model->Info().GetPropertyStr("alias"), nullptr);
  EXPECT_EQ(model->Info().GetPropertyStr("version"), nullptr);
  EXPECT_EQ(model->Info().GetPropertyInt("model_path"), nullptr);
  EXPECT_EQ(model->Info().GetPropertyInt("alias"), nullptr);
  EXPECT_EQ(model->Info().GetPropertyInt("_local_registration_id"), nullptr);
  EXPECT_EQ(model->Info().GetPropertyInt("version"), nullptr);
  EXPECT_TRUE(model->Info().prompt_templates.empty());
  EXPECT_TRUE(model->Info().model_settings.empty());
  EXPECT_TRUE(model->Info().detected_region.empty());
  EXPECT_TRUE(model->IsCached());
  EXPECT_FALSE(std::filesystem::exists(model_dir_ / "model_metadata.yml"));

  const auto index_path = root_.path() / "cache" / "models" / "foundry.local.modelinfo.json";
  ASSERT_TRUE(std::filesystem::exists(index_path));
  nlohmann::json index;
  std::ifstream(index_path) >> index;
  EXPECT_EQ(index["version"], 3);
  ASSERT_EQ(index["models"].size(), 1u);
  EXPECT_EQ(index["models"][0].size(), 2u);
  ASSERT_TRUE(index["models"][0].contains("model_info"));
  EXPECT_EQ(index["models"][0]["model_info"]["id"], "my-model-generic-cpu:7");
  EXPECT_EQ(index["models"][0]["model_info"]["name"], "my-model-generic-cpu");
  EXPECT_EQ(index["models"][0]["model_info"]["version"], 7);
  EXPECT_EQ(index["models"][0]["model_info"]["alias"], "my-model");
  EXPECT_EQ(index["models"][0]["model_info"]["stringProperties"]["custom_metadata"], "preserved");
  EXPECT_EQ(index["models"][0]["model_info"]["intProperties"]["custom_count"], 42);
  EXPECT_FALSE(index["models"][0].contains("registration_id"));
  EXPECT_FALSE(index["models"][0].contains("model_id"));
  EXPECT_FALSE(index["models"][0].contains("alias"));
  EXPECT_FALSE(index["models"][0].contains("properties"));
  EXPECT_FALSE(index["models"][0].contains("registered_at"));
  EXPECT_FALSE(index["models"][0].contains("metadata_prepared"));

  auto restored = MakeCatalog();
  auto* restored_model = restored.GetModelVariant("my-model-generic-cpu:7");
  ASSERT_NE(restored_model, nullptr);
  EXPECT_EQ(restored_model->Info().GetPropertyWithDefault("custom_metadata", std::string{}), "preserved");
  EXPECT_EQ(restored_model->Info().GetPropertyWithDefault("custom_count", int64_t{-1}), 42);
  EXPECT_EQ(restored_model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}),
            4096);
  EXPECT_TRUE(restored_model->Info().prompt_templates.empty());
  EXPECT_TRUE(restored_model->Info().model_settings.empty());
  EXPECT_TRUE(restored_model->Info().detected_region.empty());
}

TEST_F(LocalModelCatalogTest, RegistrationDerivesDefaultsAndPreservesApplicationOverrides) {
  std::ofstream(model_dir_ / "genai_config.json")
      << R"({"model":{"type":"phi3","context_length":8192,)"
         R"("prompt_templates":{"user":"<|user|>{Content}<|end|>"},)"
         R"("decoder":{"session_options":{"provider_options":[{"cuda":{}}]}}}})";

  auto* derived = catalog_.RegisterModel(model_dir_.string(), "derived-model:1", MakeMetadata());

  ASSERT_NE(derived, nullptr);
  EXPECT_EQ(derived->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR, std::string{}),
            "derived-model");
  EXPECT_EQ(derived->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR, std::string{}), "local");
  EXPECT_EQ(derived->Info().execution_provider, "CUDAExecutionProvider");
  EXPECT_FALSE(derived->Info().execution_provider_override);
  EXPECT_EQ(derived->Info().device_type, DeviceType::kGPU);
  EXPECT_EQ(derived->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}), 8192);
  EXPECT_EQ(derived->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, std::string{}),
            "text");
  EXPECT_EQ(derived->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, std::string{}),
            "text");
  EXPECT_STREQ(derived->Info().prompt_templates.Find("user"), "<|user|>{Content}<|end|>");

  auto restored = MakeCatalog();
  auto* restored_derived = restored.GetModelVariant("derived-model:1");
  ASSERT_NE(restored_derived, nullptr);
  EXPECT_STREQ(restored_derived->Info().prompt_templates.Find("user"), "<|user|>{Content}<|end|>");

  auto overrides = MakeMetadata();
  overrides.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR, "Custom display name");
  overrides.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR, "Contoso");
  overrides.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_EP_STR, "cpu");
  overrides.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_DEVICE_TYPE_STR, "CPU");
  overrides.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, "custom-input");
  overrides.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, "custom-output");
  auto* overridden = catalog_.RegisterModel(model_dir_.string(), "overridden-model:1", overrides);

  ASSERT_NE(overridden, nullptr);
  EXPECT_EQ(overridden->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR, std::string{}),
            "Custom display name");
  EXPECT_EQ(overridden->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR, std::string{}),
            "Contoso");
  EXPECT_EQ(overridden->Info().execution_provider, "CPUExecutionProvider");
  EXPECT_TRUE(overridden->Info().execution_provider_override);
  EXPECT_EQ(overridden->Info().device_type, DeviceType::kCPU);
  EXPECT_EQ(overridden->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, std::string{}),
            "custom-input");
  EXPECT_EQ(overridden->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, std::string{}),
            "custom-output");
}

TEST_F(LocalModelCatalogTest, RegistrationRejectsInvalidRuntimeMetadata) {
  const auto expect_invalid_argument = [&](const std::string& model_id, const ModelInfo& metadata,
                                           std::string_view message_fragment) {
    try {
      catalog_.RegisterModel(model_dir_.string(), model_id, metadata);
      FAIL() << "Expected exception";
    } catch (const Exception& ex) {
      EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
      EXPECT_NE(std::string(ex.what()).find(message_fragment), std::string::npos) << ex.what();
    }
  };

  auto unknown_provider = MakeMetadata();
  unknown_provider.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_EP_STR, "UnknownExecutionProvider");
  expect_invalid_argument("unknown-provider:1", unknown_provider, "unsupported execution provider");

  auto dml_provider = MakeMetadata();
  dml_provider.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_EP_STR, "DmlExecutionProvider");
  expect_invalid_argument("dml-provider:1", dml_provider, "DirectML execution provider is not supported");

  auto mismatched_device = MakeMetadata();
  mismatched_device.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_EP_STR, "CUDA");
  mismatched_device.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_DEVICE_TYPE_STR, "NPU");
  expect_invalid_argument("mismatched-device:1", mismatched_device,
                          "device_type does not match execution_provider CUDAExecutionProvider");

  auto invalid_device = MakeMetadata();
  invalid_device.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_DEVICE_TYPE_STR, "accelerator");
  expect_invalid_argument("invalid-device:1", invalid_device, "device_type must be CPU, GPU, or NPU");
}

TEST_F(LocalModelCatalogTest, RegistrationCanonicalizesProviderAndDerivesDevice) {
  auto metadata = MakeMetadata();
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_EP_STR, "CUDA");

  auto* model = catalog_.RegisterModel(model_dir_.string(), "canonical-provider:1", metadata);

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().execution_provider, "CUDAExecutionProvider");
  EXPECT_EQ(model->Info().device_type, DeviceType::kGPU);
  EXPECT_TRUE(model->Info().execution_provider_override);
}

TEST_F(LocalModelCatalogTest, RegistrationRejectsEveryDmlArtifactProviderSpelling) {
  const std::vector<std::string> dml_spellings = {
      "dml",
      "DML",
      "DmlExecutionProvider",
      "DMLExecutionProvider",
  };

  for (size_t index = 0; index < dml_spellings.size(); ++index) {
    std::ofstream(model_dir_ / "genai_config.json")
        << R"({"model":{"decoder":{"session_options":{"provider_options":[{"cpu":{}},{")"
        << dml_spellings[index] << R"(":{}}]}}}})";

    try {
      catalog_.RegisterModel(model_dir_.string(), "dml-config-" + std::to_string(index) + ":1", MakeMetadata());
      FAIL() << "Expected exception for " << dml_spellings[index];
    } catch (const Exception& ex) {
      EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
      EXPECT_NE(std::string(ex.what()).find("DirectML"), std::string::npos) << dml_spellings[index];
    }
  }
}

TEST_F(LocalModelCatalogTest, ArtifactRuntimeMetadataDoesNotOverrideArtifactProviderOptionsOnLoad) {
  std::ofstream(model_dir_ / "genai_config.json")
      << R"({"model":{"decoder":{"session_options":{"provider_options":[{"cuda":{"device_id":"1"}}]}}}})";
  auto* model = catalog_.RegisterModel(model_dir_.string(), "provider-options:1", MakeMetadata());

  ASSERT_NE(model, nullptr);
  EXPECT_FALSE(model->Info().execution_provider_override);
  try {
    model->Load();
    FAIL() << "Expected CUDA availability validation";
  } catch (const Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_NE(std::string(ex.what()).find("CUDAExecutionProvider"), std::string::npos);
  }
}

TEST_F(LocalModelCatalogTest, CallerRuntimeProviderOverridesArtifactProviderOnLoad) {
  std::ofstream(model_dir_ / "genai_config.json")
      << R"({"model":{"decoder":{"session_options":{"provider_options":[{"cuda":{"device_id":"1"}}]}}}})";
  auto metadata = MakeMetadata();
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_EP_STR, "cpu");
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_DEVICE_TYPE_STR, "CPU");
  auto* model = catalog_.RegisterModel(model_dir_.string(), "caller-provider:1", metadata);

  ASSERT_NE(model, nullptr);
  EXPECT_TRUE(model->Info().execution_provider_override);
  try {
    model->Load();
  } catch (const Exception& ex) {
    EXPECT_NE(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE) << ex.what();
  }

  auto restored = MakeCatalog();
  auto* restored_model = restored.GetModelVariant("caller-provider:1");
  ASSERT_NE(restored_model, nullptr);
  EXPECT_TRUE(restored_model->Info().execution_provider_override);
}

TEST_F(LocalModelCatalogTest, RegistrationDerivesQnnDeviceType) {
  std::ofstream(model_dir_ / "genai_config.json")
      << R"({"model":{"type":"phi3","decoder":{"session_options":{"provider_options":[{"qnn":{}}]}}}})";

  auto* model = catalog_.RegisterModel(model_dir_.string(), "qnn-model:1", MakeMetadata());

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().execution_provider, "QNNExecutionProvider");
  EXPECT_EQ(model->Info().device_type, DeviceType::kNPU);
}

TEST_F(LocalModelCatalogTest, RegistrationPersistsOpenVinoWithoutUnknownDevice) {
  std::ofstream(model_dir_ / "genai_config.json")
      << R"({"model":{"decoder":{"session_options":{"provider_options":[{"OpenVINO":{}}]}}}})";

  auto* model = catalog_.RegisterModel(model_dir_.string(), "openvino-model:1", MakeMetadata());

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().execution_provider, "OpenVINOExecutionProvider");
  EXPECT_EQ(model->Info().device_type, DeviceType::kNotSet);

  const auto index_path = root_.path() / "cache" / "models" / "foundry.local.modelinfo.json";
  nlohmann::json index;
  std::ifstream(index_path) >> index;
  const auto& runtime = index["models"][0]["model_info"]["runtime"];
  EXPECT_EQ(runtime["executionProvider"], "OpenVINOExecutionProvider");
  EXPECT_FALSE(runtime.contains("deviceType"));

  auto restored = MakeCatalog();
  auto* restored_model = restored.GetModelVariant("openvino-model:1");
  ASSERT_NE(restored_model, nullptr);
  EXPECT_EQ(restored_model->Info().execution_provider, "OpenVINOExecutionProvider");
  EXPECT_EQ(restored_model->Info().device_type, DeviceType::kNotSet);
}

TEST_F(LocalModelCatalogTest, RegistrationDerivesEmbeddingModalities) {
  auto* model = catalog_.RegisterModel(model_dir_.string(), "embedding-model:1", MakeMetadata("embeddings"));

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, std::string{}),
            "text");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, std::string{}),
            "embeddings");
}

TEST_F(LocalModelCatalogTest, RegistrationOverwritesSdkOwnedMetadata) {
  auto metadata = MakeMetadata();
  metadata.model_id = "caller-id:99";
  metadata.name = "caller-name";
  metadata.version = 99;
  metadata.alias = "caller-alias";
  metadata.uri = "caller://uri";
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR, "CallerProvider");
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_ENTITY_TYPE_STR, "CallerEntity");
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR, "CallerType");
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, "2000-01-01T00:00:00Z");
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, "caller-context");
  metadata.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, 1);
  metadata.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, 123);

  auto* model = catalog_.RegisterModel(model_dir_.string(), "sdk-owned-generic-cpu:4", metadata);

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().model_id, "sdk-owned-generic-cpu:4");
  EXPECT_EQ(model->Info().name, "sdk-owned-generic-cpu");
  EXPECT_EQ(model->Info().version, 4);
  EXPECT_EQ(model->Info().alias, "sdk-owned");
  EXPECT_TRUE(model->Info().uri.empty());
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR, std::string{}),
            "LocalRegistration");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_ENTITY_TYPE_STR, std::string{}), "Model");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR, std::string{}), "ONNX");
  EXPECT_NE(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, std::string{}),
            "2000-01-01T00:00:00Z");
  EXPECT_NE(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, int64_t{0}), 1);
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}), 4096);
  EXPECT_EQ(model->Info().GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT), nullptr);
}

TEST_F(LocalModelCatalogTest, RegistrationDoesNotUseCallerContextLengthWhenConfigOmitsIt) {
  std::ofstream(model_dir_ / "genai_config.json") << R"({"model":{"type":"phi3"}})";
  auto metadata = MakeMetadata();
  metadata.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, 123);

  auto* model = catalog_.RegisterModel(model_dir_.string(), "no-context:1", metadata);

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT), nullptr);
}

TEST_F(LocalModelCatalogTest, RegistrationRequiresExistingDirectoryAndParseableConfig) {
  EXPECT_THROW(catalog_.RegisterModel((root_.path() / "missing").string(), "missing:1", MakeMetadata()), Exception);

  const auto no_config_path = root_.path() / "no-config";
  std::filesystem::create_directories(no_config_path);
  EXPECT_THROW(catalog_.RegisterModel(no_config_path.string(), "no-config:1", MakeMetadata()), Exception);

  const auto malformed_path = root_.path() / "malformed";
  std::filesystem::create_directories(malformed_path);
  std::ofstream(malformed_path / "genai_config.json") << R"({"model":)";
  EXPECT_THROW(catalog_.RegisterModel(malformed_path.string(), "malformed:1", MakeMetadata()), Exception);
}

TEST_F(LocalModelCatalogTest, RegistrationRequiresSupportedTask) {
  ModelInfo missing_task;
  EXPECT_THROW(catalog_.RegisterModel(model_dir_.string(), "missing-task:1", missing_task), Exception);
  EXPECT_THROW(catalog_.RegisterModel(model_dir_.string(), "invalid-task:1", MakeMetadata("text-generation")),
               Exception);
}

TEST_F(LocalModelCatalogTest, RegistrationRequiresCanonicalModelId) {
  const std::vector<std::string> invalid_ids = {
      "",
      "missing-version",
      ":1",
      "model:",
      "model:1:2",
      "model:-1",
      "model:+1",
      "model:01",
      "model:2147483648",
      "model/path:1",
      "model name:1",
  };

  for (const auto& model_id : invalid_ids) {
    EXPECT_THROW(catalog_.RegisterModel(model_dir_.string(), model_id, MakeMetadata()), Exception) << model_id;
  }
}

TEST_F(LocalModelCatalogTest, RegistrationUsesUniqueIdsAndGroupsDistinctNamesByDerivedAlias) {
  auto* first = Register("my-model-generic-cpu:1");
  ASSERT_NE(first, nullptr);
  EXPECT_THROW(Register("my-model-generic-cpu:1"), Exception);

  auto* second = Register("my-model-generic-cpu:2");
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->Info().version, 2);
  EXPECT_NO_THROW(first->Download());

  auto* grouped = catalog_.GetModel("my-model");
  ASSERT_NE(grouped, nullptr);
  EXPECT_EQ(grouped->Variants().size(), 2u);
  EXPECT_EQ(grouped->Id(), "my-model-generic-cpu:2");
  EXPECT_NE(catalog_.GetModelVariant("my-model-generic-cpu:1"), nullptr);
  EXPECT_EQ(catalog_.GetModelVariant("my-model-generic-cpu:2"), second);

  grouped->SelectVariant(*first);
  EXPECT_EQ(grouped->Id(), "my-model-generic-cpu:1");
  auto other = MakeCatalog();
  ASSERT_NE(other.RegisterModel(model_dir_.string(), "my-model-generic-gpu:3", MakeMetadata()), nullptr);
  ASSERT_EQ(catalog_.ListModels().size(), 1u);
  EXPECT_EQ(grouped->Variants().size(), 3u);
  EXPECT_EQ(grouped->Id(), "my-model-generic-cpu:1");
  EXPECT_EQ(catalog_.GetModelVariant("my-model-generic-cpu:1"), first);
  EXPECT_EQ(catalog_.GetModelVariant("my-model-generic-gpu:3")->Info().name, "my-model-generic-gpu");

  catalog_.UnregisterModel("my-model-generic-cpu:1");
  EXPECT_EQ(catalog_.GetModelVariant("my-model-generic-cpu:1"), nullptr);
  EXPECT_THROW(first->Download(), Exception);
  EXPECT_EQ(catalog_.GetModelVariant("my-model-generic-cpu:2"), second);
  EXPECT_NO_THROW(second->Download());
}

TEST_F(LocalModelCatalogTest, AliasUnregisterRemovesAllVersionsAndAllowsReregistration) {
  auto* first = Register("my-model-generic-cpu:1");
  auto* second = Register("my-model-generic-cpu:2");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(catalog_.GetModel("my-model")->Variants().size(), 2u);

  catalog_.UnregisterModel("my-model");

  EXPECT_EQ(catalog_.GetModel("my-model"), nullptr);
  EXPECT_EQ(catalog_.GetModelVariant("my-model-generic-cpu:1"), nullptr);
  EXPECT_EQ(catalog_.GetModelVariant("my-model-generic-cpu:2"), nullptr);
  EXPECT_THROW(first->Download(), Exception);
  EXPECT_THROW(second->Download(), Exception);

  EXPECT_NE(Register("my-model-generic-cpu:1"), nullptr);
}

TEST_F(LocalModelCatalogTest, ModelIdUnregisterIsIndependentOfOtherVersions) {
  const auto loadable_model_path = GetTestDataPath("tiny-random-gpt2-fp32-1");
  auto* loaded = catalog_.RegisterModel(loadable_model_path.string(), "my-model-generic-cpu:1", MakeMetadata());
  auto* removed = catalog_.RegisterModel(loadable_model_path.string(), "my-model-generic-cpu:2", MakeMetadata());
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(removed, nullptr);
  ASSERT_NO_THROW(loaded->Load(ExecutionProvider::kCPU));

  EXPECT_NO_THROW(catalog_.UnregisterModel("my-model-generic-cpu:2"));
  EXPECT_EQ(catalog_.GetModelVariant("my-model-generic-cpu:2"), nullptr);
  EXPECT_EQ(catalog_.GetModelVariant("my-model-generic-cpu:1"), loaded);
  EXPECT_TRUE(loaded->IsLoaded());
  EXPECT_THROW(catalog_.UnregisterModel("my-model"), Exception);

  EXPECT_NO_THROW(loaded->Unload());
}

TEST_F(LocalModelCatalogTest, RegistrationDerivesAliasFromCanonicalGenericSuffixesOnly) {
  auto* npu = Register("npu-model-generic-npu:1");
  auto* unsuffixed = Register("my-model-custom:1");
  auto* wrong_case = Register("case-model-GENERIC-CPU:1");

  ASSERT_NE(npu, nullptr);
  EXPECT_EQ(npu->Info().alias, "npu-model");
  ASSERT_NE(unsuffixed, nullptr);
  EXPECT_EQ(unsuffixed->Info().alias, "my-model-custom");
  ASSERT_NE(wrong_case, nullptr);
  EXPECT_EQ(wrong_case->Info().alias, "case-model-GENERIC-CPU");
}

TEST_F(LocalModelCatalogTest, GetCachedModelsReturnsActiveRegisteredLeafVariants) {
  Register("my-model:1");
  Register("my-model:2");

  auto cached = catalog_.GetCachedModels();
  ASSERT_EQ(cached.size(), 2u);
  EXPECT_EQ(cached[0]->Id(), "my-model:2");
  EXPECT_EQ(cached[1]->Id(), "my-model:1");
  EXPECT_FALSE(cached[0]->IsContainer());
  EXPECT_FALSE(cached[1]->IsContainer());

  catalog_.UnregisterModel("my-model:2");

  cached = catalog_.GetCachedModels();
  ASSERT_EQ(cached.size(), 1u);
  EXPECT_EQ(cached.front()->Id(), "my-model:1");
}

TEST_F(LocalModelCatalogTest, RefreshDefersVariantReconciliationDuringUnregister) {
  Register("my-model:1");
  auto* grouped = catalog_.GetModel("my-model");
  ASSERT_NE(grouped, nullptr);

  auto other = MakeCatalog();
  grouped->BeginUnregister();
  size_t variants_during_unregister = 0;
  try {
    EXPECT_NE(other.RegisterModel(model_dir_.string(), "my-model:2", MakeMetadata()), nullptr);
    variants_during_unregister = catalog_.ListModels().front()->Variants().size();
  } catch (...) {
    grouped->EndUnregister();
    throw;
  }
  grouped->EndUnregister();

  EXPECT_EQ(variants_during_unregister, 1u);
  ASSERT_EQ(catalog_.ListModels().size(), 1u);
  EXPECT_EQ(grouped->Variants().size(), 2u);
}

TEST_F(LocalModelCatalogTest, UnregisterPersistsAndPreservesAssets) {
  auto* stale = Register();
  catalog_.UnregisterModel("my-model:1");

  EXPECT_TRUE(catalog_.ListModels().empty());
  EXPECT_TRUE(std::filesystem::exists(model_dir_ / "genai_config.json"));
  EXPECT_THROW(stale->Load(), Exception);
  EXPECT_NO_THROW(stale->Unload());
  auto restored = MakeCatalog();
  EXPECT_TRUE(restored.ListModels().empty());
}

TEST_F(LocalModelCatalogTest, AliasHandleRemainsSafeAfterItsFinalVariantIsUnregisteredById) {
  Register();
  auto* alias_handle = catalog_.GetModel("my-model");
  ASSERT_NE(alias_handle, nullptr);

  catalog_.UnregisterModel("my-model:1");

  EXPECT_FALSE(alias_handle->IsLoaded());
  EXPECT_EQ(alias_handle->Info().model_id, "my-model:1");
  EXPECT_TRUE(alias_handle->Variants().empty());
  EXPECT_THROW(alias_handle->Load(), Exception);
  EXPECT_NO_THROW(alias_handle->Unload());
}

TEST_F(LocalModelCatalogTest, UnregisterWriteFailureLeavesModelActiveAndUsable) {
  auto* model = Register();
  ASSERT_NE(model, nullptr);

  const auto temp_index_path = root_.path() / "cache" / "models" / "foundry.local.modelinfo.json.tmp";
  std::filesystem::create_directory(temp_index_path);

  EXPECT_THROW(catalog_.UnregisterModel("my-model"), Exception);
  EXPECT_EQ(catalog_.GetModelVariant("my-model:1"), model);

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

  auto* stale = Register();
  ASSERT_NE(stale, nullptr);
  ASSERT_EQ(second.ListModels().size(), 1u);

  second.UnregisterModel("my-model");
  EXPECT_TRUE(catalog_.ListModels().empty());
  EXPECT_FALSE(stale->IsLoaded());
  EXPECT_THROW(stale->Download(), Exception);
  EXPECT_THROW(stale->Load(), Exception);
  EXPECT_THROW(stale->RemoveFromCache(), Exception);

  const auto loadable_model_path = GetTestDataPath("tiny-random-gpt2-fp32-1");
  auto* replacement = second.RegisterModel(loadable_model_path.string(), "my-model:1", MakeMetadata());
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(replacement->Id(), stale->Id());
  EXPECT_NO_THROW(replacement->Load(ExecutionProvider::kCPU));
  EXPECT_TRUE(replacement->IsLoaded());
  EXPECT_FALSE(stale->IsLoaded());
  EXPECT_NO_THROW(stale->Unload());
  EXPECT_TRUE(replacement->IsLoaded());
  EXPECT_NO_THROW(replacement->Unload());
  ASSERT_EQ(catalog_.ListModels().size(), 1u);
  EXPECT_NE(catalog_.GetModelVariant("my-model:1"), stale);
}

TEST_F(LocalModelCatalogTest, RefreshReplacesSameIdAtDifferentPathWhenUnregisterWasNotObserved) {
  const auto original_path = GetTestDataPath("tiny-random-gpt2-fp32-1");
  auto* stale = catalog_.RegisterModel(original_path.string(), "my-model:1", MakeMetadata());
  ASSERT_NE(stale, nullptr);
  ASSERT_NO_THROW(stale->Load(ExecutionProvider::kCPU));
  ASSERT_TRUE(stale->IsLoaded());

  auto second = MakeCatalog();
  ASSERT_NE(second.GetModelVariant("my-model:1"), nullptr);

  const auto replacement_path = root_.path() / "replacement-model";
  std::filesystem::copy(original_path, replacement_path, std::filesystem::copy_options::recursive);

  const auto index_path = root_.path() / "cache" / "models" / "foundry.local.modelinfo.json";
  nlohmann::json index;
  std::ifstream(index_path) >> index;
  index["models"][0]["model_path"] = std::filesystem::absolute(replacement_path).lexically_normal().string();
  std::ofstream(index_path, std::ios::binary | std::ios::trunc) << index.dump(2);

  auto* replacement = catalog_.GetModelVariant("my-model:1");
  ASSERT_NE(replacement, nullptr);
  EXPECT_NE(replacement, stale);
  EXPECT_EQ(replacement->LocalPath(), std::filesystem::absolute(replacement_path).lexically_normal().string());
  EXPECT_THROW(stale->Load(), Exception);
  EXPECT_FALSE(replacement->IsLoaded());

  EXPECT_NO_THROW(stale->Unload());
  EXPECT_NO_THROW(replacement->Load(ExecutionProvider::kCPU));
  EXPECT_TRUE(replacement->IsLoaded());
  EXPECT_NO_THROW(replacement->Unload());
}

TEST_F(LocalModelCatalogTest, LoadsLegacyRegistrationPropertiesUsingPersistedModelId) {
  const auto cache_dir = root_.path() / "cache" / "models";
  std::filesystem::create_directories(cache_dir);
  const nlohmann::json legacy_index = {
      {"version", 1},
      {"catalog_name", "local"},
      {"models",
       {{{"alias", "legacy-wrong-alias"},
         {"model_id", "legacy-model:4"},
         {"model_path", model_dir_.string()},
         {"registration_id", "ignored-legacy-registration-id"},
         {"properties",
          {{"alias", "legacy-wrong-alias"},
           {"model_path", model_dir_.string()},
           {"version", 99},
           {FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion"}}}}}},
  };
  std::ofstream(cache_dir / "foundry.local.modelinfo.json") << legacy_index.dump(2);

  auto restored = MakeCatalog();
  auto* model = restored.GetModelVariant("legacy-model:4");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().alias, "legacy-model");
  EXPECT_EQ(model->Info().version, 4);
  EXPECT_EQ(model->Info().GetPropertyStr("model_path"), nullptr);
  EXPECT_EQ(model->Info().GetPropertyStr("alias"), nullptr);
  EXPECT_EQ(model->Info().GetPropertyInt("version"), nullptr);

  nlohmann::json migrated_index;
  std::ifstream(cache_dir / "foundry.local.modelinfo.json") >> migrated_index;
  EXPECT_EQ(migrated_index["version"], 3);
  ASSERT_EQ(migrated_index["models"].size(), 1u);
  EXPECT_TRUE(migrated_index["models"][0].contains("model_info"));
  EXPECT_FALSE(migrated_index["models"][0].contains("properties"));

  ASSERT_NE(restored.RegisterModel(model_dir_.string(), "new-model:1", MakeMetadata()), nullptr);
  std::ifstream(cache_dir / "foundry.local.modelinfo.json") >> migrated_index;
  ASSERT_EQ(migrated_index["models"].size(), 2u);
  EXPECT_TRUE(migrated_index["models"][0].contains("model_info"));
  EXPECT_FALSE(migrated_index["models"][0].contains("properties"));
  EXPECT_NE(restored.GetModelVariant("legacy-model:4"), nullptr);
}

TEST_F(LocalModelCatalogTest, MigratesSchemaV2MetadataUsingAuthoritativeSources) {
  std::ofstream(model_dir_ / "genai_config.json")
      << R"({"model":{"type":"phi3","context_length":8192,)"
       R"("prompt_templates":{"user":"artifact-template"},)"
       R"("decoder":{"session_options":{"provider_options":[{"cuda":{"device_id":"1"}}]}}}})";

  auto legacy_info = MakeMetadata();
  legacy_info.model_id = "migration-model:2";
  legacy_info.name = "wrong-name";
  legacy_info.version = 99;
  legacy_info.alias = "wrong-alias";
  legacy_info.uri = "caller://uri";
  legacy_info.detected_region = "caller-region";
  legacy_info.prompt_templates.Add("user", "caller-template");
  legacy_info.model_settings.Add("temperature", "0.5");
  legacy_info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR, "CallerProvider");
  legacy_info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_ENTITY_TYPE_STR, "CallerEntity");
  legacy_info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR, "CallerType");
  legacy_info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, "2000-01-01T00:00:00Z");
  legacy_info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_EP_STR, "CUDAExecutionProvider");
  legacy_info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_DEVICE_TYPE_STR, "GPU");
  legacy_info.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, 123);
  legacy_info.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, 1);
  legacy_info.SetPropertyStr("model_path", "caller-path");
  legacy_info.SetPropertyStr("custom_metadata", "preserved");

  const auto cache_dir = root_.path() / "cache" / "models";
  std::filesystem::create_directories(cache_dir);
  const nlohmann::json legacy_index = {
      {"version", 2},
      {"catalog_name", "local"},
      {"models", {{{"model_info", ModelInfoToJson(legacy_info)}, {"model_path", model_dir_.string()}}}},
  };
  const auto index_path = cache_dir / "foundry.local.modelinfo.json";
  std::ofstream(index_path) << legacy_index.dump(2);

  auto restored = MakeCatalog();
  auto* model = restored.GetModelVariant("migration-model:2");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().name, "migration-model");
  EXPECT_EQ(model->Info().alias, "migration-model");
  EXPECT_EQ(model->Info().version, 2);
  EXPECT_TRUE(model->Info().uri.empty());
  EXPECT_TRUE(model->Info().detected_region.empty());
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR, std::string{}),
            "LocalRegistration");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_ENTITY_TYPE_STR, std::string{}), "Model");
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR, std::string{}), "ONNX");
  EXPECT_NE(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, std::string{}),
            "2000-01-01T00:00:00Z");
  EXPECT_NE(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, int64_t{0}), 1);
  EXPECT_EQ(model->Info().GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{-1}), 8192);
  EXPECT_TRUE(model->Info().execution_provider_override);
  EXPECT_EQ(ResolveLoadExecutionProvider(model->Info(), true, ExecutionProvider::kDefault),
            ExecutionProvider::kCUDA);
  EXPECT_STREQ(model->Info().prompt_templates.Find("user"), "artifact-template");
  EXPECT_TRUE(model->Info().model_settings.empty());
  EXPECT_EQ(model->Info().GetPropertyStr("model_path"), nullptr);
  EXPECT_EQ(model->Info().GetPropertyWithDefault("custom_metadata", std::string{}), "preserved");

  nlohmann::json migrated_index;
  std::ifstream(index_path) >> migrated_index;
  EXPECT_EQ(migrated_index["version"], 3);
  EXPECT_EQ(migrated_index["models"][0]["model_info"]["intProperties"]
                          [FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT],
            8192);
  EXPECT_EQ(migrated_index["models"][0]["model_info"]["promptTemplate"]["user"], "artifact-template");
  EXPECT_FALSE(migrated_index["models"][0]["model_info"].contains("modelSettings"));
  EXPECT_EQ(migrated_index["models"][0]["execution_provider_override"], true);
}

TEST_F(LocalModelCatalogTest, MigratesSchemaV2WithoutProviderToArtifactDefault) {
  std::ofstream(model_dir_ / "genai_config.json")
      << R"({"model":{"decoder":{"session_options":{"provider_options":[{"cuda":{"device_id":"1"}}]}}}})";

  auto legacy_info = MakeMetadata();
  legacy_info.model_id = "migration-artifact-default:1";

  const auto cache_dir = root_.path() / "cache" / "models";
  std::filesystem::create_directories(cache_dir);
  const nlohmann::json legacy_index = {
      {"version", 2},
      {"catalog_name", "local"},
      {"models", {{{"model_info", ModelInfoToJson(legacy_info)}, {"model_path", model_dir_.string()}}}},
  };
  const auto index_path = cache_dir / "foundry.local.modelinfo.json";
  std::ofstream(index_path) << legacy_index.dump(2);

  auto restored = MakeCatalog();
  auto* model = restored.GetModelVariant("migration-artifact-default:1");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().execution_provider, "CUDAExecutionProvider");
  EXPECT_FALSE(model->Info().execution_provider_override);
  EXPECT_EQ(ResolveLoadExecutionProvider(model->Info(), true, ExecutionProvider::kDefault),
            ExecutionProvider::kDefault);

  nlohmann::json migrated_index;
  std::ifstream(index_path) >> migrated_index;
  EXPECT_EQ(migrated_index["version"], 3);
  EXPECT_FALSE(migrated_index["models"][0].contains("execution_provider_override"));
}

TEST_F(LocalModelCatalogTest, MigratesSchemaV2CallerProviderOverrideAndPersistsProvenance) {
  std::ofstream(model_dir_ / "genai_config.json")
      << R"({"model":{"decoder":{"session_options":{"provider_options":[{"cuda":{"device_id":"1"}}]}}}})";

  auto legacy_info = MakeMetadata();
  legacy_info.model_id = "migration-override:1";
  legacy_info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_EP_STR, "cpu");
  legacy_info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_DEVICE_TYPE_STR, "CPU");

  const auto cache_dir = root_.path() / "cache" / "models";
  std::filesystem::create_directories(cache_dir);
  const nlohmann::json legacy_index = {
      {"version", 2},
      {"catalog_name", "local"},
      {"models", {{{"model_info", ModelInfoToJson(legacy_info)}, {"model_path", model_dir_.string()}}}},
  };
  const auto index_path = cache_dir / "foundry.local.modelinfo.json";
  std::ofstream(index_path) << legacy_index.dump(2);

  auto restored = MakeCatalog();
  auto* model = restored.GetModelVariant("migration-override:1");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().execution_provider, "CPUExecutionProvider");
  EXPECT_TRUE(model->Info().execution_provider_override);
  EXPECT_EQ(ResolveLoadExecutionProvider(model->Info(), true, ExecutionProvider::kDefault),
            ExecutionProvider::kCPU);

  nlohmann::json migrated_index;
  std::ifstream(index_path) >> migrated_index;
  EXPECT_EQ(migrated_index["version"], 3);
  EXPECT_EQ(migrated_index["models"][0]["execution_provider_override"], true);

  auto restored_again = MakeCatalog();
  auto* model_again = restored_again.GetModelVariant("migration-override:1");
  ASSERT_NE(model_again, nullptr);
  EXPECT_TRUE(model_again->Info().execution_provider_override);
  EXPECT_EQ(ResolveLoadExecutionProvider(model_again->Info(), true, ExecutionProvider::kDefault),
            ExecutionProvider::kCPU);
}

TEST_F(LocalModelCatalogTest, SchemaV2MigrationFailurePreservesOriginalIndex) {
  auto legacy_info = MakeMetadata();
  legacy_info.model_id = "missing-artifact:1";
  const auto missing_model_path = root_.path() / "missing-model";

  const auto cache_dir = root_.path() / "cache" / "models";
  std::filesystem::create_directories(cache_dir);
  const nlohmann::json legacy_index = {
      {"version", 2},
      {"catalog_name", "local"},
      {"models", {{{"model_info", ModelInfoToJson(legacy_info)}, {"model_path", missing_model_path.string()}}}},
  };
  const auto index_path = cache_dir / "foundry.local.modelinfo.json";
  std::ofstream(index_path) << legacy_index.dump(2);
  std::ifstream original_stream(index_path, std::ios::binary);
  const std::string original_index{std::istreambuf_iterator<char>(original_stream),
                                   std::istreambuf_iterator<char>()};

  auto restored = MakeCatalog();
  EXPECT_THROW(restored.ListModels(), Exception);

  std::ifstream stream(index_path, std::ios::binary);
  const std::string preserved_index{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  EXPECT_EQ(preserved_index, original_index);
  EXPECT_FALSE(std::filesystem::exists(index_path.string() + ".tmp"));
}

TEST_F(LocalModelCatalogTest, RegistrationDoesNotOverwriteUnreadableOrUnsupportedIndex) {
  Register();
  const auto index_path = root_.path() / "cache" / "models" / "foundry.local.modelinfo.json";
  nlohmann::json index;
  std::ifstream(index_path) >> index;
  index["version"] = 999;
  const std::vector<std::string> invalid_indices = {"{ not-json", index.dump(2)};

  for (const auto& invalid_index : invalid_indices) {
    std::ofstream(index_path, std::ios::binary | std::ios::trunc) << invalid_index;

    EXPECT_THROW(catalog_.RegisterModel(model_dir_.string(), "another-model:1", MakeMetadata()), Exception);

    std::ifstream stream(index_path, std::ios::binary);
    const std::string preserved_index{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    EXPECT_EQ(preserved_index, invalid_index);
  }
}

TEST_F(LocalModelCatalogTest, RestoredRegistrationUsesFreshMetadataValidation) {
  Register();
  const auto index_path = root_.path() / "cache" / "models" / "foundry.local.modelinfo.json";
  nlohmann::json index;
  std::ifstream(index_path) >> index;
  index["models"][0]["model_info"]["task"] = "unsupported-task";
  const auto invalid_index = index.dump(2);
  std::ofstream(index_path, std::ios::binary | std::ios::trunc) << invalid_index;

  auto restored = MakeCatalog();
  EXPECT_THROW(restored.ListModels(), Exception);
  EXPECT_THROW(restored.RegisterModel(model_dir_.string(), "another-model:1", MakeMetadata()), Exception);

  std::ifstream stream(index_path, std::ios::binary);
  const std::string preserved_index{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  EXPECT_EQ(preserved_index, invalid_index);
}

TEST_F(LocalModelCatalogTest, RestoredSchemaV3RegistrationValidatesRuntimeMetadata) {
  Register();
  const auto index_path = root_.path() / "cache" / "models" / "foundry.local.modelinfo.json";
  nlohmann::json index;
  std::ifstream(index_path) >> index;
  const std::vector<nlohmann::json> invalid_runtimes = {
      {{"deviceType", "NPU"}, {"executionProvider", "CPUExecutionProvider"}},
      {{"deviceType", "accelerator"}},
      {{"deviceType", 1}},
      {{"executionProvider", 1}},
      {{"executionProvider", "UnknownExecutionProvider"}},
      {{"executionProvider", "DMLExecutionProvider"}},
      "not-an-object",
  };

  const auto expect_preserved_failure = [&](const nlohmann::json& invalid_index) {
    const auto serialized_index = invalid_index.dump(2);
    std::ofstream(index_path, std::ios::binary | std::ios::trunc) << serialized_index;

    auto restored = MakeCatalog();
    EXPECT_THROW(restored.ListModels(), Exception);

    std::ifstream stream(index_path, std::ios::binary);
    const std::string preserved_index{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    EXPECT_EQ(preserved_index, serialized_index);
    EXPECT_FALSE(std::filesystem::exists(index_path.string() + ".tmp"));
  };

  for (const auto& invalid_runtime : invalid_runtimes) {
    index["models"][0]["model_info"]["runtime"] = invalid_runtime;
    index["models"][0].erase("execution_provider_override");
    expect_preserved_failure(index);
  }

  index["models"][0]["model_info"].erase("runtime");
  index["models"][0]["execution_provider_override"] = "true";
  expect_preserved_failure(index);
}

TEST_F(LocalModelCatalogTest, PublicCatalogContractRejectsMutation) {
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

  EXPECT_THROW(catalog.RegisterModel(model_dir_.string(), "my-model:1", MakeMetadata()), Exception);
  EXPECT_THROW(catalog.UnregisterModel("my-model:1"), Exception);
}

}  // namespace
}  // namespace fl::test
