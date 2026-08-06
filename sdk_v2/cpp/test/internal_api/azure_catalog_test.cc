// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for the Azure catalog client infrastructure:
//   - Request JSON generation matches the known-good format
//   - Response JSON parsing works with realistic data
//   - Live integration test fetches models from ai.azure.com
//
#include "catalog/azure_catalog_client.h"
#include "catalog/azure_catalog_models.h"
#include "catalog/azure_model_catalog.h"
#include "catalog/catalog_cache.h"
#include "catalog/catalog_client.h"
#include "ep_detection/ep_detector.h"
#include "exception.h"
#include "internal_api/test_helpers.h"
#include "logger.h"
#include "model_info.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace fl;

namespace {

namespace fs = std::filesystem;

http::HttpResponse MakeOkResponse(std::string body) {
  http::HttpResponse response;
  response.status = 200;
  response.body = std::move(body);
  return response;
}

ModelPackageVariant MakePackageVariant(std::string name,
                                       std::string execution_provider,
                                       DeviceType device_type,
                                       std::string compatibility_string) {
  ModelPackageVariant variant;
  variant.name = std::move(name);
  variant.execution_provider = std::move(execution_provider);
  variant.device_type = device_type;
  variant.compatibility_string = std::move(compatibility_string);
  return variant;
}

ModelVariantMetadata MakeModelPackageMetadata(std::initializer_list<ModelPackageVariant> variants,
                                              std::string model_format = "ort-model-package",
                                              std::optional<int> schema_version = 1) {
  ModelVariantMetadata metadata;
  metadata.model_format = std::move(model_format);

  ModelPackageMetadata package;
  package.schema_version = schema_version;
  package.variants.assign(variants.begin(), variants.end());
  metadata.model_package = std::move(package);

  return metadata;
}

ModelInfo MakeCatalogModelInfo(std::string model_id,
                               std::string name,
                               int version,
                               std::string alias,
                               std::string execution_provider = "CPUExecutionProvider",
                               DeviceType device_type = DeviceType::kCPU,
                               std::optional<ModelVariantMetadata> variant_metadata = std::nullopt) {
  ModelInfo info;
  info.model_id = std::move(model_id);
  info.name = std::move(name);
  info.version = version;
  info.alias = std::move(alias);
  info.uri = "azureml://test/" + info.model_id;
  info.execution_provider = std::move(execution_provider);
  info.device_type = device_type;
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR] = "FoundryLocal";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR] = "ONNX";
  info.variant_metadata = std::move(variant_metadata);
  return info;
}

Model MakeTestModel(ModelInfo info, std::string local_path) {
  static fl::test::FakeServiceBindings services;
  return Model::FromModelInfo(std::move(info),
                              std::move(local_path),
                              services.download_manager,
                              services.model_load_manager);
}

struct FakeCatalogClientState {
  std::vector<ModelInfo> all_models;
  std::vector<ModelInfo> id_models;
  std::vector<ModelInfo> version_models;
  int fetch_all_calls = 0;
  int fetch_models_by_id_calls = 0;
  int fetch_versions_calls = 0;
  std::vector<std::vector<std::string>> requested_id_batches;
};

class FakeCatalogClient : public ICatalogClient {
 public:
  explicit FakeCatalogClient(std::shared_ptr<FakeCatalogClientState> state)
      : state_(std::move(state)) {}

  std::vector<ModelInfo> FetchAllModelInfos() override {
    ++state_->fetch_all_calls;
    return state_->all_models;
  }

  std::vector<ModelInfo> FetchModelsByIds(const std::vector<std::string>& model_ids) override {
    ++state_->fetch_models_by_id_calls;
    state_->requested_id_batches.push_back(model_ids);

    std::vector<ModelInfo> result;
    for (const auto& info : state_->id_models) {
      if (std::find(model_ids.begin(), model_ids.end(), info.model_id) != model_ids.end()) {
        result.push_back(info);
      }
    }

    return result;
  }

  std::vector<ModelInfo> FetchAllVersionsByAlias(const std::string& model_alias,
                                                 const std::string& model_name,
                                                 int /*max_versions*/) override {
    ++state_->fetch_versions_calls;

    std::vector<ModelInfo> result;
    for (const auto& info : state_->version_models) {
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

 private:
  std::shared_ptr<FakeCatalogClientState> state_;
};

AzureModelCatalog::CatalogClientFactory MakeFakeCatalogClientFactory(
    const std::shared_ptr<FakeCatalogClientState>& state) {
  return [state](const std::string& /*base_url*/,
                 const std::string& /*filter_override*/,
                 const IEpDetector& /*ep_detector*/,
                 ILogger& /*logger*/,
                 const std::string& /*cache_directory*/,
                 const std::string& /*catalog_region*/,
                 bool /*disable_region_fallback*/) {
    return std::make_unique<FakeCatalogClient>(state);
  };
}

class CompatibilityEpDetector : public IEpDetector {
 public:
  struct Call {
    std::string execution_provider;
    std::optional<DeviceType> device_type;
    std::string compatibility_string;
  };

  explicit CompatibilityEpDetector(
      std::map<std::string, CompiledModelCompatibility> compatibilities,
      std::map<std::string, std::vector<std::string>> devices = {{"CPU", {"CPUExecutionProvider"}}})
      : compatibilities_(std::move(compatibilities)),
        devices_(std::move(devices)) {}

  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return devices_;
  }

  CompiledModelCompatibility GetModelCompatibilityForEpDevices(
      std::string_view execution_provider,
      std::optional<DeviceType> device_type,
      std::string_view compatibility_string) const override {
    calls_.push_back(Call{
        std::string(execution_provider),
        device_type,
        std::string(compatibility_string),
    });

    const auto it = compatibilities_.find(std::string(compatibility_string));
    if (it != compatibilities_.end()) {
      return it->second;
    }

    return CompiledModelCompatibility::kUnknown;
  }

  mutable std::vector<Call> calls_;

 private:
  std::map<std::string, CompiledModelCompatibility> compatibilities_;
  std::map<std::string, std::vector<std::string>> devices_;
};

class AzureModelCatalogCompatibilityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    test_dir_ = (fs::temp_directory_path() /
                 ("fl_azure_model_catalog_" + std::to_string(unique_suffix))).string();
    fs::create_directories(test_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  std::unique_ptr<AzureModelCatalog> MakeCatalog(
      const IEpDetector& detector,
      bool cache_only,
      std::shared_ptr<FakeCatalogClientState> state = std::make_shared<FakeCatalogClientState>()) {
    return std::make_unique<AzureModelCatalog>(
        std::vector<std::pair<std::string, std::optional<std::string>>>{
            {"https://test.com", std::nullopt},
        },
        test_dir_,
        MakeTestModel,
        detector,
        logger_,
        cache_only,
        "eastus",
        false,
        MakeFakeCatalogClientFactory(std::move(state)));
  }

  void SaveCache(const std::vector<ModelInfo>& models) {
    CatalogCache cache(test_dir_, logger_);
    cache.Save(models);
  }

  void CreateScannedFlatModel(const std::string& model_id) {
    const fs::path model_root = fs::path(test_dir_) / "publisher" / "model";
    fs::create_directories(model_root);

    {
      std::ofstream genai_config(model_root / "genai_config.json", std::ios::trunc);
      ASSERT_TRUE(genai_config.is_open());
      genai_config << "{}";
    }

    {
      nlohmann::json inference_model;
      inference_model["Name"] = model_id;

      std::ofstream file(model_root / "inference_model.json", std::ios::trunc);
      ASSERT_TRUE(file.is_open());
      file << inference_model.dump(2);
    }
  }

  std::string test_dir_;
  StderrLogger logger_;
};

}  // namespace

// ========================================================================
// Test EP detector that returns all three devices (matching .http fixture)
// ========================================================================

class AllDevicesEpDetector : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    // Use PascalCase names matching ORT's GetEpDevices output.
    // BuildSearchFilters() lowers these for the catalog API.
    return {
        {"CPU", {"CPUExecutionProvider"}},
        {"GPU", {"CUDAExecutionProvider"}},
        {"NPU", {"QNNExecutionProvider"}},
    };
  }
};

// Single-device EP for tests where we only want one filter set
class CpuOnlyEpDetector : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {{"CPU", {"CPUExecutionProvider"}}};
  }
};

class CpuGpuEpDetector : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {
        {"CPU", {"CPUExecutionProvider"}},
        {"GPU", {"CUDAExecutionProvider"}},
    };
  }
};

// ========================================================================
// Request format tests
// ========================================================================

// Verify the generated request JSON has the right structure.
// We compare against the known-good azure_catalog_model_fetch.http request.
TEST(AzureCatalogClientTest, RequestFormatMatchesKnownGood) {
  AllDevicesEpDetector ep;
  StderrLogger logger;
  std::vector<std::string> captured_urls;
  std::vector<nlohmann::json> captured_bodies;

  // filter_override: "", "test" — matches the .http file's foundryLocal filter values
  AzureCatalogClient client(
      "https://ai.azure.com/api/eastus/ux/v1.0", "''", ep, logger,
      [&](const std::string& url, const std::string& body) {
        captured_urls.push_back(url);
        captured_bodies.push_back(nlohmann::json::parse(body));
        return MakeOkResponse(
            R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})");
      },
      "eastus");

  client.FetchAllModels();

  // Our code creates one filter set per device (3 devices → 3 requests).
  ASSERT_EQ(captured_bodies.size(), 3u);

  for (const auto& url : captured_urls) {
    EXPECT_EQ(url, "https://ai.azure.com/api/eastus/ux/v1.0/entities/crossRegion");
  }

  // Verify the first request (cpu) matches the expected structure.
  // The .http file combines all devices; the client splits requests per device.
  // We verify the structure, field names, and operators match.
  const auto& cpu_req = captured_bodies[0];

  // Top-level structure
  ASSERT_TRUE(cpu_req.contains("resourceIds"));
  ASSERT_TRUE(cpu_req.contains("indexEntitiesRequest"));

  // resourceIds
  const auto& resources = cpu_req["resourceIds"];
  ASSERT_EQ(resources.size(), 1u);
  EXPECT_EQ(resources[0]["resourceId"], "azureml");
  EXPECT_EQ(resources[0]["entityContainerType"], "Registry");

  // indexEntitiesRequest
  const auto& idx_req = cpu_req["indexEntitiesRequest"];
  ASSERT_TRUE(idx_req.contains("filters"));
  EXPECT_EQ(idx_req["pageSize"], 50);

  // Verify filters match the known-good pattern
  const auto& filters = idx_req["filters"];
  ASSERT_EQ(filters.size(), 6u);

  // Filter 0: type=models
  EXPECT_EQ(filters[0]["field"], "type");
  EXPECT_EQ(filters[0]["operator"], "eq");
  EXPECT_EQ(filters[0]["values"], nlohmann::json({"models"}));

  // Filter 1: kind=Versioned
  EXPECT_EQ(filters[1]["field"], "kind");
  EXPECT_EQ(filters[1]["operator"], "eq");
  EXPECT_EQ(filters[1]["values"], nlohmann::json({"Versioned"}));

  // Filter 2: labels=latest
  EXPECT_EQ(filters[2]["field"], "labels");
  EXPECT_EQ(filters[2]["operator"], "eq");
  EXPECT_EQ(filters[2]["values"], nlohmann::json({"latest"}));

  // Filter 3: foundryLocal filter — matches "", "test" from the .http file
  EXPECT_EQ(filters[3]["field"], "annotations/tags/foundryLocal");
  EXPECT_EQ(filters[3]["operator"], "eq");
  EXPECT_EQ(filters[3]["values"], nlohmann::json({""}));

  // Filter 4: device (per-device split — this is "cpu" for the first request)
  EXPECT_EQ(filters[4]["field"], "properties/variantInfo/variantMetadata/device");
  EXPECT_EQ(filters[4]["operator"], "eq");
  EXPECT_EQ(filters[4]["values"], nlohmann::json({"cpu"}));

  // Filter 5: executionProvider
  EXPECT_EQ(filters[5]["field"], "properties/variantInfo/variantMetadata/executionProvider");
  EXPECT_EQ(filters[5]["operator"], "eq");
}

// Verify page size default is 50.
TEST(AzureCatalogClientTest, DefaultPageSizeIs50) {
  AllDevicesEpDetector ep;
  StderrLogger logger;
  nlohmann::json captured;

  AzureCatalogClient client(
      "https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger,
      [&](const std::string&, const std::string& body) {
        captured = nlohmann::json::parse(body);
        return MakeOkResponse(
            R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})");
      },
      "eastus");

  client.FetchAllModels();
  EXPECT_EQ(captured["indexEntitiesRequest"]["pageSize"], 50);
}

// ========================================================================
// Response parsing tests
// ========================================================================

TEST(AzureCatalogClientTest, ParsesModelResponseCorrectly) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;

  // Realistic single-model response matching the Azure catalog schema
  const char* mock_response = R"({
    "indexEntitiesResponse": {
      "totalCount": 1,
      "value": [
        {
          "assetId": "azureml://registries/azureml/models/Phi-4-mini-instruct-generic-cpu/versions/2",
          "entityId": "Phi-4-mini-instruct-generic-cpu:2",
          "annotations": {
            "tags": {
              "alias": "Phi-4-mini-instruct",
              "foundryLocal": "",
              "task": "chat-completion",
              "license": "MIT",
              "licenseDescription": "MIT License",
              "supportsToolCalling": "true",
              "promptTemplate": "{\"system\":\"<|system|>\\n{Content}<|end|>\",\"user\":\"<|user|>\\n{Content}<|end|>\",\"assistant\":\"<|assistant|>\\n{Content}<|end|>\",\"prompt\":\"<|user|>\\n{Content}<|end|>\\n<|assistant|>\"}"
            },
            "systemCatalogData": {
              "publisher": "Microsoft",
              "displayName": "Phi-4 Mini Instruct",
              "maxOutputTokens": 4096
            }
          },
          "properties": {
            "name": "Phi-4-mini-instruct-generic-cpu",
            "version": 2,
            "variantInfo": {
              "parents": [{"assetId": "azureml://registries/azureml/models/Phi-4-mini-instruct/versions/3"}],
              "variantMetadata": {
                "modelType": "ONNX",
                "device": "cpu",
                "executionProvider": "CPUExecutionProvider",
                "fileSizeBytes": 4294967296
              }
            },
            "minFLVersion": "0.3.0"
          }
        }
      ],
      "nextSkip": 0,
      "continuationToken": ""
    }
  })";

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(mock_response);
                            });

  auto model_infos = client.FetchAllModelInfos();
  ASSERT_EQ(model_infos.size(), 1u);

  const auto& info = model_infos[0];
  EXPECT_EQ(info.model_id, "Phi-4-mini-instruct-generic-cpu:2");
  EXPECT_EQ(info.name, "Phi-4-mini-instruct-generic-cpu");
  EXPECT_EQ(info.alias, "Phi-4-mini-instruct");
  EXPECT_EQ(info.version, 2);
  EXPECT_EQ(info.uri, "azureml://registries/azureml/models/Phi-4-mini-instruct-generic-cpu/versions/2");
  EXPECT_EQ(info.device_type, DeviceType::kCPU);
  EXPECT_EQ(info.execution_provider, "CPUExecutionProvider");

  // Prompt templates should be parsed from the JSON string
  ASSERT_FALSE(info.prompt_templates.empty());
  EXPECT_TRUE(info.prompt_templates.count("prompt") > 0);
  EXPECT_TRUE(info.prompt_templates.count("system") > 0);
  EXPECT_TRUE(info.prompt_templates.count("user") > 0);
  EXPECT_TRUE(info.prompt_templates.count("assistant") > 0);

  // Metadata properties
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR), "chat-completion");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR), "MIT");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR), "Microsoft");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR), "Phi-4 Mini Instruct");
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR), "0.3.0");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR), "FoundryLocal");
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT), 4096);
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT), 4096);  // 4GB → 4096 MB
}

TEST(AzureCatalogClientTest, ParsesOrtModelPackageMetadataIntoTypedModelInfo) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;

  const char* mock_response = R"({
    "indexEntitiesResponse": {
      "totalCount": 1,
      "value": [{
        "assetId": "azureml://registries/azureml/models/phi-4-mini-ort-package/versions/1",
        "entityId": "phi-4-mini-ort-package:1",
        "annotations": {
          "tags": {
            "alias": "phi-4-mini-ort-package"
          }
        },
        "properties": {
          "name": "phi-4-mini-ort-package",
          "version": 1,
          "variantInfo": {
            "parents": [],
            "variantMetadata": {
              "modelType": "ONNX",
              "device": "cpu",
              "executionProvider": "CPUExecutionProvider",
              "modelFormat": "ort-model-package",
              "modelPackage": {
                "schemaVersion": 7,
                "variants": [
                  {
                    "name": "cpu-compiled",
                    "executionProvider": "CPUExecutionProvider",
                    "device": "cpu",
                    "compatibilityString": "compat-a"
                  },
                  {
                    "name": "gpu-compiled",
                    "executionProvider": "CUDAExecutionProvider",
                    "device": "gpu",
                    "compatibilityString": "compat-b"
                  }
                ]
              }
            }
          }
        }
      }],
      "nextSkip": 0,
      "continuationToken": ""
    }
  })";

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(mock_response);
                            });

  auto model_infos = client.FetchAllModelInfos();
  ASSERT_EQ(model_infos.size(), 1u);

  const auto& info = model_infos[0];
  ASSERT_TRUE(info.variant_metadata.has_value());
  ASSERT_TRUE(info.variant_metadata->model_format.has_value());
  EXPECT_EQ(*info.variant_metadata->model_format, "ort-model-package");

  ASSERT_TRUE(info.variant_metadata->model_package.has_value());
  ASSERT_TRUE(info.variant_metadata->model_package->schema_version.has_value());
  EXPECT_EQ(*info.variant_metadata->model_package->schema_version, 7);

  const auto& variants = info.variant_metadata->model_package->variants;
  ASSERT_EQ(variants.size(), 2u);

  EXPECT_EQ(variants[0].name, "cpu-compiled");
  EXPECT_EQ(variants[0].execution_provider, "CPUExecutionProvider");
  EXPECT_EQ(variants[0].device_type, DeviceType::kCPU);
  EXPECT_EQ(variants[0].compatibility_string, "compat-a");

  EXPECT_EQ(variants[1].name, "gpu-compiled");
  EXPECT_EQ(variants[1].execution_provider, "CUDAExecutionProvider");
  EXPECT_EQ(variants[1].device_type, DeviceType::kGPU);
  EXPECT_EQ(variants[1].compatibility_string, "compat-b");
}

// Verify that invalid models (missing required fields) are filtered out
TEST(AzureCatalogClientTest, SkipsInvalidModels) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;

  // Response with one valid model and one missing assetId
  const char* mock_response = R"({
    "indexEntitiesResponse": {
      "totalCount": 2,
      "value": [
        {
          "entityId": "no-asset-id",
          "properties": { "name": "bad-model", "version": 1, "variantInfo": { "parents": [] } }
        },
        {
          "assetId": "azureml://registries/azureml/models/good-model/versions/1",
          "entityId": "good-model:1",
          "annotations": { "tags": { "alias": "good" } },
          "properties": {
            "name": "good-model",
            "version": 1,
            "variantInfo": {
              "parents": [],
              "variantMetadata": { "device": "cpu", "executionProvider": "CPUExecutionProvider" }
            }
          }
        }
      ],
      "nextSkip": 0,
      "continuationToken": ""
    }
  })";

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(mock_response);
                            });

  auto infos = client.FetchAllModelInfos();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].alias, "good");
}

// Verify pagination follows nextSkip and continuationToken
TEST(AzureCatalogClientTest, FollowsPagination) {
  AllDevicesEpDetector ep;
  // Use a single device so we only test one filter set
  class SingleDeviceEp : public IEpDetector {
   public:
    std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
      return {{"CPU", {"CPUExecutionProvider"}}};
    }
  } single_ep;

  StderrLogger logger;
  int call_count = 0;
  AzureCatalogClient client("https://test.com", "", single_ep, logger,
                            [&](const std::string&, const std::string& body) {
                              call_count++;
                              auto req = nlohmann::json::parse(body);

                              if (call_count == 1) {
                                // First page — return nextSkip to trigger page 2
                                return MakeOkResponse(R"({
        "indexEntitiesResponse": {
          "totalCount": 2,
          "value": [{
            "assetId": "azureml://m/model-a/versions/1",
            "entityId": "model-a:1",
            "annotations": {"tags": {"alias": "a"}},
            "properties": {"name": "model-a", "version": 1, "variantInfo": {"parents": [], "variantMetadata": {"device": "cpu"}}}
          }],
          "nextSkip": 1,
          "continuationToken": "token123"
        }
      })");
                              }

                              // Second page — no more
                              // Verify skip/token were passed
                              EXPECT_EQ(req["indexEntitiesRequest"]["skip"], 1);
                              EXPECT_EQ(req["indexEntitiesRequest"]["continuationToken"], "token123");
                              return MakeOkResponse(R"({
        "indexEntitiesResponse": {
          "totalCount": 2,
          "value": [{
            "assetId": "azureml://m/model-b/versions/1",
            "entityId": "model-b:1",
            "annotations": {"tags": {"alias": "b"}},
            "properties": {"name": "model-b", "version": 1, "variantInfo": {"parents": [], "variantMetadata": {"device": "cpu"}}}
          }],
          "nextSkip": 0,
          "continuationToken": ""
        }
      })");
                            });

  auto models = client.FetchAllModels();
  EXPECT_EQ(call_count, 2);
  EXPECT_EQ(models.size(), 2u);
}

// ========================================================================
// Live integration test — fetches real models from ai.azure.com
// Disabled by default. Run with: --gtest_also_run_disabled_tests
// ========================================================================

TEST(AzureCatalogClientTest, DISABLED_LiveFetchModelsFromAzure) {
  AllDevicesEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "''", ep, logger);

  // Use the real HTTP client (WinHTTP on Windows)
  auto model_infos = client.FetchAllModelInfos();

  // We should get at least some models from the public catalog
  EXPECT_GT(model_infos.size(), 0u)
      << "Expected at least one model from Azure Foundry catalog";

  // Every model should have the basic required fields populated
  for (const auto& info : model_infos) {
    EXPECT_FALSE(info.model_id.empty()) << "model_id should not be empty";
    EXPECT_FALSE(info.name.empty()) << "name should not be empty";
    EXPECT_FALSE(info.alias.empty()) << "alias should not be empty for: " << info.model_id;
    EXPECT_FALSE(info.uri.empty()) << "uri should not be empty for: " << info.model_id;
  }

  // Print summary for manual verification
  std::cout << "\n=== Live Azure Catalog Results ===\n";
  std::cout << "Total models: " << model_infos.size() << "\n";
  for (const auto& info : model_infos) {
    std::cout << "  " << info.alias
              << " (v" << info.version << ")"
              << " [" << info.execution_provider << "]"
              << "\n";
  }
  std::cout << "=================================\n";
}

// ========================================================================
// FetchModelsByIds filter construction tests
// ========================================================================

TEST(AzureCatalogClientTest, BuildModelIdFiltersProducesCorrectStructure) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  nlohmann::json captured;

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              captured = nlohmann::json::parse(body);
                              return MakeOkResponse(
                                  R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})");
                            });

  client.FetchModelsByIds({"phi-4-mini:3", "llama-3:1"});

  // Should have made exactly one HTTP call (single filter set, no pagination).
  ASSERT_FALSE(captured.is_null());

  const auto& filters = captured["indexEntitiesRequest"]["filters"];
  ASSERT_EQ(filters.size(), 4u);

  // Filter 0: type=models
  EXPECT_EQ(filters[0]["field"], "type");
  EXPECT_EQ(filters[0]["values"], nlohmann::json({"models"}));

  // Filter 1: kind=Versioned
  EXPECT_EQ(filters[1]["field"], "kind");
  EXPECT_EQ(filters[1]["values"], nlohmann::json({"Versioned"}));

  // Filter 2: foundryLocal tag (no labels=latest!)
  EXPECT_EQ(filters[2]["field"], "annotations/tags/foundryLocal");

  // Filter 3: properties/id with the requested model IDs
  EXPECT_EQ(filters[3]["field"], "properties/id");
  EXPECT_EQ(filters[3]["values"], nlohmann::json({"phi-4-mini:3", "llama-3:1"}));

  // Verify NO labels filter and NO device/EP filters exist.
  for (const auto& f : filters) {
    EXPECT_NE(f["field"], "labels");
    EXPECT_NE(f["field"], "properties/variantInfo/variantMetadata/device");
    EXPECT_NE(f["field"], "properties/variantInfo/variantMetadata/executionProvider");
  }
}

TEST(AzureCatalogClientTest, FetchModelsByIdsEmptyReturnsEmptyNoHttp) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  bool http_called = false;

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              http_called = true;
                              return MakeOkResponse(
                                  R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})");
                            });

  auto result = client.FetchModelsByIds({});
  EXPECT_TRUE(result.empty());
  EXPECT_FALSE(http_called);
}

// ========================================================================
// FetchAllModelInfosWithCachedModels tests
// ========================================================================

// Helper: builds a minimal valid model response for a given model name.
static std::string MakeMockCatalogResponse(
    const std::vector<std::pair<std::string, int>>& models) {
  nlohmann::json value_array = nlohmann::json::array();

  for (const auto& [name, version] : models) {
    std::string entity_id = name + ":" + std::to_string(version);

    nlohmann::json entry;
    entry["assetId"] = "azureml://registries/azureml/models/" + name + "/versions/" +
                       std::to_string(version);
    entry["entityId"] = entity_id;
    entry["annotations"]["tags"]["alias"] = name;
    entry["properties"]["name"] = name;
    entry["properties"]["version"] = version;
    entry["properties"]["variantInfo"]["parents"] = nlohmann::json::array();
    entry["properties"]["variantInfo"]["variantMetadata"]["device"] = "cpu";
    entry["properties"]["variantInfo"]["variantMetadata"]["executionProvider"] =
        "CPUExecutionProvider";
    value_array.push_back(entry);
  }

  nlohmann::json response;
  response["indexEntitiesResponse"]["totalCount"] = static_cast<int>(models.size());
  response["indexEntitiesResponse"]["value"] = value_array;
  response["indexEntitiesResponse"]["nextSkip"] = 0;
  response["indexEntitiesResponse"]["continuationToken"] = "";
  return response.dump();
}

TEST(AzureCatalogClientTest, WithCachedModels_NoCachedIds_BehavesLikeRegularFetch) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int http_call_count = 0;

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              http_call_count++;
                              return MakeOkResponse(MakeMockCatalogResponse({{"phi-4-mini", 3}}));
                            });

  auto result = FetchAllModelInfosWithCachedModels(client, {}, logger);

  // Only the primary FetchAllModelInfos call — no extra fetch for cached models.
  EXPECT_EQ(http_call_count, 1);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].model_id, "phi-4-mini:3");
}

TEST(AzureCatalogClientTest, WithCachedModels_AlreadyInCatalog_NoExtraFetch) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int http_call_count = 0;

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              http_call_count++;
                              return MakeOkResponse(MakeMockCatalogResponse({{"phi-4-mini", 3}}));
                            });

  // The cached ID matches what's already in the catalog — no extra fetch needed.
  auto result = FetchAllModelInfosWithCachedModels(client, {"phi-4-mini:3"}, logger);

  EXPECT_EQ(http_call_count, 1);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].model_id, "phi-4-mini:3");
}

TEST(AzureCatalogClientTest, WithCachedModels_UnresolvedId_TriggersSecondFetch) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int http_call_count = 0;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              http_call_count++;

                              if (http_call_count == 1) {
                                // Primary catalog fetch — returns phi-4-mini only.
                                return MakeOkResponse(MakeMockCatalogResponse({{"phi-4-mini", 3}}));
                              } else {
                                // Second fetch — looking up the unresolved model by ID.
                                auto req = nlohmann::json::parse(body);
                                const auto& filters = req["indexEntitiesRequest"]["filters"];

                                // Verify the second call uses properties/id filter.
                                bool has_id_filter = false;
                                for (const auto& f : filters) {
                                  if (f["field"] == "properties/id") {
                                    has_id_filter = true;
                                    EXPECT_EQ(f["values"], nlohmann::json({"old-model:1"}));
                                  }
                                }

                                EXPECT_TRUE(has_id_filter);

                                return MakeOkResponse(MakeMockCatalogResponse({{"old-model", 1}}));
                              }
                            });

  auto result = FetchAllModelInfosWithCachedModels(client, {"old-model:1"}, logger);

  EXPECT_EQ(http_call_count, 2);
  ASSERT_EQ(result.size(), 2u);

  // Verify both models are present.
  bool found_phi = false;
  bool found_old = false;
  for (const auto& info : result) {
    if (info.model_id == "phi-4-mini:3") {
      found_phi = true;
    }

    if (info.model_id == "old-model:1") {
      found_old = true;
    }
  }

  EXPECT_TRUE(found_phi);
  EXPECT_TRUE(found_old);
}

TEST(AzureCatalogClientTest, WithCachedModels_FullyUnresolved_CreatesBYOEntry) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int http_call_count = 0;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              http_call_count++;

                              if (http_call_count == 1) {
                                // Primary catalog — returns nothing matching.
                                return MakeOkResponse(MakeMockCatalogResponse({{"phi-4-mini", 3}}));
                              } else {
                                // FetchModelsByIds — also returns nothing for the custom model.
                                return MakeOkResponse(
                                    R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})");
                              }
                            });

  auto result = FetchAllModelInfosWithCachedModels(client, {"custom-model:0"}, logger);

  EXPECT_EQ(http_call_count, 2);

  // Find the BYO entry.
  const ModelInfo* byo = nullptr;
  for (const auto& info : result) {
    if (info.model_id == "custom-model:0") {
      byo = &info;
    }
  }

  ASSERT_NE(byo, nullptr);
  EXPECT_EQ(byo->name, "custom-model");
  EXPECT_EQ(byo->alias, "custom-model");
  EXPECT_EQ(byo->uri, "local://custom-model");
  EXPECT_EQ(byo->version, 0);
  EXPECT_EQ(byo->string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR), "Local");
  EXPECT_EQ(byo->string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR), "ONNX");
}

// ========================================================================
// Reasoning field parsing tests
// ========================================================================

TEST(AzureCatalogClientTest, ParsesReasoningFieldsCorrectly) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;

  const char* mock_response = R"({
    "indexEntitiesResponse": {
      "totalCount": 1,
      "value": [{
        "assetId": "azureml://registries/azureml/models/Phi-4-mini-reasoning-generic-cpu/versions/1",
        "entityId": "Phi-4-mini-reasoning-generic-cpu:1",
        "annotations": {
          "tags": {
            "alias": "Phi-4-mini-reasoning",
            "foundryLocal": "",
            "task": "chat-completion",
            "supportsToolCalling": "true",
            "supportsReasoning": "true",
            "reasoningStart": "<think>",
            "reasoningEnd": "</think>"
          },
          "systemCatalogData": {
            "publisher": "Microsoft",
            "displayName": "Phi-4 Mini Reasoning"
          }
        },
        "properties": {
          "name": "Phi-4-mini-reasoning-generic-cpu",
          "version": 1,
          "variantInfo": {
            "parents": [{"assetId": "azureml://registries/azureml/models/Phi-4-mini-reasoning/versions/1"}],
            "variantMetadata": {
              "modelType": "ONNX",
              "device": "cpu",
              "executionProvider": "CPUExecutionProvider"
            }
          }
        }
      }],
      "nextSkip": 0,
      "continuationToken": ""
    }
  })";

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(mock_response);
                            });

  auto model_infos = client.FetchAllModelInfos();
  ASSERT_EQ(model_infos.size(), 1u);

  const auto& info = model_infos[0];

  // Core identity
  EXPECT_EQ(info.model_id, "Phi-4-mini-reasoning-generic-cpu:1");
  EXPECT_EQ(info.alias, "Phi-4-mini-reasoning");

  // Reasoning fields
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 1);
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR), "<think>");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR), "</think>");

  // Tool calling
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);
}

// Verify that mixed-case device tags and bool tag values parse the same as canonical case.
TEST(AzureCatalogClientTest, ParsesTagsCaseInsensitively) {
  AllDevicesEpDetector ep;
  StderrLogger logger;

  // Three models with mixed-case device strings ("GpU", "NPU", "Cpu") and mixed-case
  // bool strings ("TRUE", "False", "tRuE") to exercise the case-insensitive paths in
  // ParseDeviceType and the bool-tag parser.
  // Each request carries a device filter — return only the matching model so the
  // 3 per-device requests (AllDevicesEpDetector) produce exactly 3 results total.
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              auto req = nlohmann::json::parse(body);

                              std::string device_filter;
                              for (const auto& f : req["indexEntitiesRequest"]["filters"]) {
                                if (f["field"] == "properties/variantInfo/variantMetadata/device") {
                                  device_filter = f["values"][0].get<std::string>();
                                  break;
                                }
                              }

                              if (device_filter == "gpu") {
                                return MakeOkResponse(R"({
        "indexEntitiesResponse": {
          "totalCount": 1,
          "value": [{
            "assetId": "azureml://registries/azureml/models/m-gpu/versions/1",
            "entityId": "m-gpu:1",
            "annotations": {"tags": {"alias": "m-gpu", "supportsToolCalling": "TRUE"}},
            "properties": {
              "name": "m-gpu", "version": 1,
              "variantInfo": {
                "parents": [],
                "variantMetadata": {"device": "GpU", "executionProvider": "CUDAExecutionProvider"}
              }
            }
          }],
          "nextSkip": 0,
          "continuationToken": ""
        }
      })");
                              }

                              if (device_filter == "npu") {
                                return MakeOkResponse(R"({
        "indexEntitiesResponse": {
          "totalCount": 1,
          "value": [{
            "assetId": "azureml://registries/azureml/models/m-npu/versions/1",
            "entityId": "m-npu:1",
            "annotations": {"tags": {"alias": "m-npu", "supportsToolCalling": "False"}},
            "properties": {
              "name": "m-npu", "version": 1,
              "variantInfo": {
                "parents": [],
                "variantMetadata": {"device": "NPU", "executionProvider": "QNNExecutionProvider"}
              }
            }
          }],
          "nextSkip": 0,
          "continuationToken": ""
        }
      })");
                              }

                              // cpu
                              return MakeOkResponse(R"({
      "indexEntitiesResponse": {
        "totalCount": 1,
        "value": [{
          "assetId": "azureml://registries/azureml/models/m-cpu/versions/1",
          "entityId": "m-cpu:1",
          "annotations": {"tags": {"alias": "m-cpu", "supportsReasoning": "tRuE"}},
          "properties": {
            "name": "m-cpu", "version": 1,
            "variantInfo": {
              "parents": [],
              "variantMetadata": {"device": "Cpu", "executionProvider": "CPUExecutionProvider"}
            }
          }
        }],
        "nextSkip": 0,
        "continuationToken": ""
      }
    })");
                            });

  auto model_infos = client.FetchAllModelInfos();
  ASSERT_EQ(model_infos.size(), 3u);

  std::map<std::string, const ModelInfo*> by_id;
  for (const auto& info : model_infos) {
    by_id[info.model_id] = &info;
  }

  ASSERT_TRUE(by_id.count("m-gpu:1"));
  EXPECT_EQ(by_id["m-gpu:1"]->device_type, DeviceType::kGPU);
  EXPECT_EQ(by_id["m-gpu:1"]->int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);

  ASSERT_TRUE(by_id.count("m-npu:1"));
  EXPECT_EQ(by_id["m-npu:1"]->device_type, DeviceType::kNPU);
  EXPECT_EQ(by_id["m-npu:1"]->int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 0);

  ASSERT_TRUE(by_id.count("m-cpu:1"));
  EXPECT_EQ(by_id["m-cpu:1"]->device_type, DeviceType::kCPU);
  EXPECT_EQ(by_id["m-cpu:1"]->int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 1);
}

// ========================================================================
// Region detection tests
// ========================================================================

namespace {

// Build an HTTP response with a single cluster header and the given status.
http::HttpResponse MakeProbeResponse(int status, const std::string& cluster_header) {
  http::HttpResponse resp;
  resp.status = status;
  if (!cluster_header.empty()) {
    resp.headers["azureml-served-by-cluster"] = cluster_header;
  }
  resp.body = R"({"value":[]})";
  return resp;
}

}  // namespace

TEST(AzureCatalogClientTest, DetectRegionParsesClusterHeader) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string probe_url;
  std::string catalog_url;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger,
                            [&](const std::string& url, const std::string&) {
                              if (url == "https://api.catalog.azureml.ms/asset-gallery/v1.0/models") {
                                probe_url = url;
                                return MakeProbeResponse(200, "vienna-westus2-01");
                              }

                              catalog_url = url;
                              return MakeOkResponse(
                                  R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})");
                            });

  client.FetchAllModels();
  EXPECT_EQ(probe_url, "https://api.catalog.azureml.ms/asset-gallery/v1.0/models");
  EXPECT_EQ(catalog_url, "https://ai.azure.com/api/westus2/ux/v1.0/entities/crossRegion");
}

TEST(AzureCatalogClientTest, DetectRegionMissingHeaderDefaultsToCentralUs) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string catalog_url;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger,
                            [&](const std::string& url, const std::string&) {
                              if (url == "https://api.catalog.azureml.ms/asset-gallery/v1.0/models") {
                                return MakeProbeResponse(200, /*cluster_header=*/"");
                              }

                              catalog_url = url;
                              return MakeOkResponse(
                                  R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})");
                            });

  client.FetchAllModels();
  EXPECT_EQ(catalog_url, "https://ai.azure.com/api/centralus/ux/v1.0/entities/crossRegion");
}

TEST(AzureCatalogClientTest, DetectRegionMalformedHeaderDefaultsToCentralUs) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string catalog_url;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger,
                            [&](const std::string& url, const std::string&) {
                              if (url == "https://api.catalog.azureml.ms/asset-gallery/v1.0/models") {
                                return MakeProbeResponse(200, "not-a-cluster-name");
                              }

                              catalog_url = url;
                              return MakeOkResponse(
                                  R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})");
                            });

  client.FetchAllModels();
  EXPECT_EQ(catalog_url, "https://ai.azure.com/api/centralus/ux/v1.0/entities/crossRegion");
}

TEST(AzureCatalogClientTest, DetectRegionProbeFailureDefaultsToCentralUs) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string catalog_url;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger,
                            [&](const std::string& url, const std::string&) {
                              if (url == "https://api.catalog.azureml.ms/asset-gallery/v1.0/models") {
                                return MakeProbeResponse(503, "vienna-westus2-01");
                              }

                              catalog_url = url;
                              return MakeOkResponse(
                                  R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})");
                            });

  client.FetchAllModels();
  EXPECT_EQ(catalog_url, "https://ai.azure.com/api/centralus/ux/v1.0/entities/crossRegion");
}

TEST(AzureCatalogClientTest, ExplicitRegionOverridesDetection) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  bool probe_called = false;
  std::string catalog_url;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger, [&](const std::string& url, const std::string&) {
                              if (url == "https://api.catalog.azureml.ms/asset-gallery/v1.0/models") {
                                probe_called = true;
                              }

                              catalog_url = url;
                              return MakeOkResponse(
                                  R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})"); }, "westeurope");

  client.FetchAllModels();
  EXPECT_FALSE(probe_called);
  EXPECT_EQ(catalog_url, "https://ai.azure.com/api/westeurope/ux/v1.0/entities/crossRegion");
}

// ========================================================================
// Region-aware catalog URL tests
// ========================================================================

TEST(AzureCatalogClientTest, ActiveRegionDrivesCatalogUrl) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string captured_url;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger, [&](const std::string& url, const std::string&) {
                              captured_url = url;
                              return MakeOkResponse(
                                  R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})"); }, "westus2");

  client.FetchAllModels();
  EXPECT_EQ(captured_url, "https://ai.azure.com/api/westus2/ux/v1.0/entities/crossRegion");
}

TEST(AzureCatalogClientTest, NonRegionalUrlUsedVerbatimEvenWithRegion) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string captured_url;
  AzureCatalogClient client("https://custom.example.com/catalog", "", ep, logger, [&](const std::string& url, const std::string&) {
                              captured_url = url;
                              return MakeOkResponse(
                                  R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})"); }, "westus2");

  client.FetchAllModels();
  EXPECT_EQ(captured_url, "https://custom.example.com/catalog/entities/crossRegion");
}

TEST(AzureCatalogClientTest, DetectedRegionStampedOnModels) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger, [&](const std::string&, const std::string&) { return MakeOkResponse(MakeMockCatalogResponse({{"phi-4-mini", 3}})); }, "westus2");

  auto infos = client.FetchAllModelInfos();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].detected_region, "westus2");
}

TEST(AzureCatalogClientTest, RegionStampedPerFilterSetAfterFallback) {
  CpuGpuEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger, [&](const std::string& url, const std::string& body) {
    http::HttpResponse resp;
    if (body.find("\"cpu\"") != std::string::npos && url.find("/api/eastus/") != std::string::npos) {
      resp.status = 503;
      return resp;
    }

    if (body.find("\"cpu\"") != std::string::npos && url.find("/api/eastus2/") != std::string::npos) {
      resp.status = 200;
      resp.body = MakeMockCatalogResponse({{"cpu-model", 1}});
      return resp;
    }

    if (body.find("\"gpu\"") != std::string::npos && url.find("/api/eastus2/") != std::string::npos) {
      resp.status = 503;
      return resp;
    }

    if (body.find("\"gpu\"") != std::string::npos && url.find("/api/eastus/") != std::string::npos) {
      resp.status = 200;
      resp.body = MakeMockCatalogResponse({{"gpu-model", 1}});
      return resp;
    }

    resp.status = 500;
    return resp; }, "eastus");

  auto infos = client.FetchAllModelInfos();
  ASSERT_EQ(infos.size(), 2u);

  std::map<std::string, std::string> region_by_id;
  for (const auto& info : infos) {
    region_by_id[info.model_id] = info.detected_region;
  }

  EXPECT_EQ(region_by_id["cpu-model:1"], "eastus2");
  EXPECT_EQ(region_by_id["gpu-model:1"], "eastus");
}

// ========================================================================
// Catalog region fallback
// ========================================================================

TEST(AzureCatalogClientTest, Fallback_RetriesNextRegionAndPinsPagination) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::vector<std::string> attempted_urls;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger, [&](const std::string& url, const std::string&) {
    attempted_urls.push_back(url);
    http::HttpResponse resp;
    // First attempt (eastus) is region-unhealthy; subsequent regions are healthy.
    if (attempted_urls.size() == 1) {
      resp.status = 503;
      return resp;
    }
    resp.status = 200;
    resp.body = R"({"indexEntitiesResponse":{"totalCount":0,"value":[],"nextSkip":0,"continuationToken":""}})";
    return resp; }, "eastus");

  auto models = client.FetchAllModels();
  ASSERT_GE(attempted_urls.size(), 2u);
  EXPECT_TRUE(attempted_urls[0].find("/api/eastus/") != std::string::npos);
  EXPECT_TRUE(attempted_urls[1].find("/api/eastus2/") != std::string::npos)
      << "Expected fallback to the first proximal region. Got: " << attempted_urls[1];
}

TEST(AzureCatalogClientTest, Fallback_DisabledDoesNotRetry) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int calls = 0;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger, [&](const std::string&, const std::string&) {
    ++calls;
    http::HttpResponse resp;
    resp.status = 503;  // unhealthy, but fallback is off → no retry, filter set is skipped
    return resp; }, "eastus", /*region_fallback_enabled=*/false);

  auto models = client.FetchAllModels();
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(models.empty());
}

TEST(AzureCatalogClientTest, Fallback_PermanentCatalogErrorThrows) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int calls = 0;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger, [&](const std::string&, const std::string&) {
    ++calls;
    http::HttpResponse resp;
    resp.status = 404;
    return resp; }, "eastus");

  try {
    client.FetchAllModels();
    FAIL() << "Expected fl::Exception for permanent catalog HTTP failure";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_NETWORK);
  }
  EXPECT_EQ(calls, 1);
}

TEST(AzureCatalogClientTest, Fallback_MidPaginationFailureDoesNotCommitPartialFilterSet) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int calls = 0;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger, [&](const std::string&, const std::string&) {
    ++calls;
    http::HttpResponse resp;
    if (calls == 1) {
      resp.status = 200;
      resp.body = R"({
        "indexEntitiesResponse": {
          "totalCount": 1,
          "value": [{
            "assetId": "azureml://registries/azureml/models/page-one-model/versions/1",
            "entityId": "page-one-model:1",
            "annotations": {"tags": {"alias": "page-one-model"}},
            "properties": {
              "name": "page-one-model",
              "version": 1,
              "variantInfo": {
                "parents": [],
                "variantMetadata": {"device": "cpu", "executionProvider": "CPUExecutionProvider"}
              }
            }
          }],
          "nextSkip": 50,
          "continuationToken": "next"
        }
      })";
      return resp;
    }

    resp.status = 503;
    return resp; }, "eastus");

  auto models = client.FetchAllModels();
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(models.empty());
}

// ========================================================================
// AzureModelCatalog compatibility filtering tests
// ========================================================================

TEST_F(AzureModelCatalogCompatibilityTest, NormalListShowsModelWhenAnyRelevantPackageVariantIsCompatible) {
  auto state = std::make_shared<FakeCatalogClientState>();
  state->all_models.push_back(MakeCatalogModelInfo(
      "compatible-model:1",
      "compatible-model",
      1,
      "compatible-model",
      "CPUExecutionProvider",
      DeviceType::kCPU,
      MakeModelPackageMetadata({
          MakePackageVariant("cpu-a", "CPUExecutionProvider", DeviceType::kCPU, "unsupported-cpu"),
          MakePackageVariant("cpu-b", "CPUExecutionProvider", DeviceType::kCPU, "prefer-recompile"),
      })));

  CompatibilityEpDetector detector({
      {"unsupported-cpu", CompiledModelCompatibility::kUnsupported},
      {"prefer-recompile", CompiledModelCompatibility::kSupportedPreferRecompilation},
  });

  auto catalog = MakeCatalog(detector, /*cache_only=*/false, state);
  auto models = catalog->ListModels();

  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0]->Info().model_id, "compatible-model:1");
  ASSERT_EQ(detector.calls_.size(), 2u);
  EXPECT_EQ(detector.calls_[0].execution_provider, "CPUExecutionProvider");
  ASSERT_TRUE(detector.calls_[0].device_type.has_value());
  EXPECT_EQ(*detector.calls_[0].device_type, DeviceType::kCPU);
}

TEST_F(AzureModelCatalogCompatibilityTest, NormalListHidesModelWhenAllRelevantPackageVariantsUnsupported) {
  auto state = std::make_shared<FakeCatalogClientState>();
  state->all_models.push_back(MakeCatalogModelInfo(
      "unsupported-model:1",
      "unsupported-model",
      1,
      "unsupported-model",
      "CPUExecutionProvider",
      DeviceType::kCPU,
      MakeModelPackageMetadata({
          MakePackageVariant("cpu-a", "CPUExecutionProvider", DeviceType::kCPU, "unsupported-a"),
          MakePackageVariant("cpu-b", "CPUExecutionProvider", DeviceType::kCPU, "unsupported-b"),
          MakePackageVariant("gpu-a", "CUDAExecutionProvider", DeviceType::kGPU, "supported-but-irrelevant"),
      })));

  CompatibilityEpDetector detector({
      {"unsupported-a", CompiledModelCompatibility::kUnsupported},
      {"unsupported-b", CompiledModelCompatibility::kUnsupported},
      {"supported-but-irrelevant", CompiledModelCompatibility::kSupportedOptimal},
  });

  auto catalog = MakeCatalog(detector, /*cache_only=*/false, state);
  auto models = catalog->ListModels();

  EXPECT_TRUE(models.empty());
  ASSERT_EQ(detector.calls_.size(), 2u);
}

TEST_F(AzureModelCatalogCompatibilityTest, NormalListFailsOpenForNotApplicableAndEmptyCompatibilityString) {
  auto state = std::make_shared<FakeCatalogClientState>();
  state->all_models.push_back(MakeCatalogModelInfo(
      "not-applicable-model:1",
      "not-applicable-model",
      1,
      "not-applicable-model",
      "CPUExecutionProvider",
      DeviceType::kCPU,
      MakeModelPackageMetadata({
          MakePackageVariant("cpu-a", "CPUExecutionProvider", DeviceType::kCPU, "not-applicable"),
      })));
  state->all_models.push_back(MakeCatalogModelInfo(
      "empty-compat-model:1",
      "empty-compat-model",
      1,
      "empty-compat-model",
      "CPUExecutionProvider",
      DeviceType::kCPU,
      MakeModelPackageMetadata({
          MakePackageVariant("cpu-a", "CPUExecutionProvider", DeviceType::kCPU, ""),
      })));

  CompatibilityEpDetector detector({
      {"not-applicable", CompiledModelCompatibility::kUnknown},
  });

  auto catalog = MakeCatalog(detector, /*cache_only=*/false, state);
  auto models = catalog->ListModels();

  ASSERT_EQ(models.size(), 2u);
  ASSERT_EQ(detector.calls_.size(), 1u);
  EXPECT_EQ(detector.calls_[0].compatibility_string, "not-applicable");
}

TEST_F(AzureModelCatalogCompatibilityTest, NormalListLeavesFlatModelsVisible) {
  auto state = std::make_shared<FakeCatalogClientState>();

  ModelVariantMetadata flat_metadata;
  flat_metadata.model_format = "flat";

  state->all_models.push_back(MakeCatalogModelInfo(
      "flat-model:1",
      "flat-model",
      1,
      "flat-model",
      "CPUExecutionProvider",
      DeviceType::kCPU,
      flat_metadata));

  CompatibilityEpDetector detector({
      {"unused", CompiledModelCompatibility::kUnsupported},
  });

  auto catalog = MakeCatalog(detector, /*cache_only=*/false, state);
  auto models = catalog->ListModels();

  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0]->Info().model_id, "flat-model:1");
  EXPECT_TRUE(detector.calls_.empty());
}

TEST_F(AzureModelCatalogCompatibilityTest, CacheOnlyModeRefiltersCachedMetadataOnEachRead) {
  SaveCache({
      MakeCatalogModelInfo(
          "cached-package:1",
          "cached-package",
          1,
          "cached-package",
          "CPUExecutionProvider",
          DeviceType::kCPU,
          MakeModelPackageMetadata({
              MakePackageVariant("cpu-a", "CPUExecutionProvider", DeviceType::kCPU, "cached-compat"),
          })),
  });

  CompatibilityEpDetector unsupported_detector({
      {"cached-compat", CompiledModelCompatibility::kUnsupported},
  });
  auto hidden_catalog = MakeCatalog(unsupported_detector, /*cache_only=*/true);
  EXPECT_TRUE(hidden_catalog->ListModels().empty());

  CompatibilityEpDetector supported_detector({
      {"cached-compat", CompiledModelCompatibility::kSupportedOptimal},
  });
  auto visible_catalog = MakeCatalog(supported_detector, /*cache_only=*/true);
  auto visible_models = visible_catalog->ListModels();

  ASSERT_EQ(visible_models.size(), 1u);
  EXPECT_EQ(visible_models[0]->Info().model_id, "cached-package:1");
}

TEST_F(AzureModelCatalogCompatibilityTest, FilteredCachedCatalogModelIsNotSynthesizedAsLocalByo) {
  CreateScannedFlatModel("cached-package:1");

  auto state = std::make_shared<FakeCatalogClientState>();
  state->id_models.push_back(MakeCatalogModelInfo(
      "cached-package:1",
      "cached-package",
      1,
      "cached-package",
      "CPUExecutionProvider",
      DeviceType::kCPU,
      MakeModelPackageMetadata({
          MakePackageVariant("cpu-a", "CPUExecutionProvider", DeviceType::kCPU, "cached-hidden"),
      })));

  CompatibilityEpDetector detector({
      {"cached-hidden", CompiledModelCompatibility::kUnsupported},
  });

  auto catalog = MakeCatalog(detector, /*cache_only=*/false, state);
  auto models = catalog->ListModels();

  EXPECT_TRUE(models.empty());
  EXPECT_EQ(state->fetch_all_calls, 1);
  EXPECT_EQ(state->fetch_models_by_id_calls, 1);
  ASSERT_EQ(state->requested_id_batches.size(), 1u);
  ASSERT_EQ(state->requested_id_batches[0].size(), 1u);
  EXPECT_EQ(state->requested_id_batches[0][0], "cached-package:1");
}

TEST_F(AzureModelCatalogCompatibilityTest, DirectIdLookupFiltersUnsupportedPackageModel) {
  auto state = std::make_shared<FakeCatalogClientState>();
  state->id_models.push_back(MakeCatalogModelInfo(
      "lookup-model:1",
      "lookup-model",
      1,
      "lookup-model",
      "CPUExecutionProvider",
      DeviceType::kCPU,
      MakeModelPackageMetadata({
          MakePackageVariant("cpu-a", "CPUExecutionProvider", DeviceType::kCPU, "lookup-hidden"),
      })));

  CompatibilityEpDetector detector({
      {"lookup-hidden", CompiledModelCompatibility::kUnsupported},
  });

  auto catalog = MakeCatalog(detector, /*cache_only=*/false, state);
  EXPECT_EQ(catalog->GetModelVariant("lookup-model:1"), nullptr);
  EXPECT_EQ(state->fetch_models_by_id_calls, 1);
}

TEST_F(AzureModelCatalogCompatibilityTest, VersionListingFiltersUnsupportedPackageVersions) {
  auto state = std::make_shared<FakeCatalogClientState>();
  state->version_models.push_back(MakeCatalogModelInfo(
      "versioned-model:1",
      "versioned-model",
      1,
      "versioned-model",
      "CPUExecutionProvider",
      DeviceType::kCPU,
      MakeModelPackageMetadata({
          MakePackageVariant("cpu-a", "CPUExecutionProvider", DeviceType::kCPU, "version-hidden"),
      })));
  state->version_models.push_back(MakeCatalogModelInfo(
      "versioned-model:2",
      "versioned-model",
      2,
      "versioned-model",
      "CPUExecutionProvider",
      DeviceType::kCPU,
      MakeModelPackageMetadata({
          MakePackageVariant("cpu-a", "CPUExecutionProvider", DeviceType::kCPU, "version-visible"),
      })));

  CompatibilityEpDetector detector({
      {"version-hidden", CompiledModelCompatibility::kUnsupported},
      {"version-visible", CompiledModelCompatibility::kSupportedOptimal},
  });

  auto catalog = MakeCatalog(detector, /*cache_only=*/false, state);
  auto versions = catalog->GetModelVersions("versioned-model", "", 0);

  ASSERT_EQ(versions.size(), 1u);
  EXPECT_EQ(versions[0]->Info().model_id, "versioned-model:2");
  EXPECT_EQ(state->fetch_versions_calls, 1);
}
