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
#include "catalog/catalog_client.h"
#include "ep_detection/ep_detector.h"
#include "exception.h"
#include "logger.h"
#include "model_info.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace fl;

namespace {

constexpr const char* kAssetGalleryModelsUrl = "https://api.catalog.azureml.ms/asset-gallery/v1.0/models";

http::HttpResponse MakeOkResponse(std::string body) {
  http::HttpResponse response;
  response.status = 200;
  response.body = std::move(body);
  return response;
}

bool IsRegionProbeRequest(const std::string& body) {
  const auto request = nlohmann::json::parse(body);
  return request.contains("filters") && request["filters"].is_array() && request["filters"].empty() &&
         request.value("pageSize", 0) == 1;
}

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

// Verify the generated request JSON has the expected per-device structure.
TEST(AzureCatalogClientTest, RequestFormatMatchesKnownGood) {
  AllDevicesEpDetector ep;
  StderrLogger logger;
  std::vector<std::string> captured_urls;
  std::vector<nlohmann::json> captured_bodies;

  AzureCatalogClient client(
    kAssetGalleryModelsUrl, "", ep, logger,
      [&](const std::string& url, const std::string& body) {
        captured_urls.push_back(url);
        captured_bodies.push_back(nlohmann::json::parse(body));
      return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})");
      },
      "eastus");

  client.FetchAllModels();

  // One query is issued per detected device.
  ASSERT_EQ(captured_bodies.size(), 3u);

  for (const auto& url : captured_urls) {
    EXPECT_EQ(url, kAssetGalleryModelsUrl);
  }

  // Verify the first request (cpu) matches the expected structure.
  // The client splits requests per device.
  const auto& cpu_req = captured_bodies[0];

  ASSERT_TRUE(cpu_req.contains("filters"));
  EXPECT_EQ(cpu_req["pageSize"], 50);

  // Verify filters match deployment options + device + execution provider.
  const auto& filters = cpu_req["filters"];
  ASSERT_EQ(filters.size(), 3u);

  EXPECT_EQ(filters[0]["field"], "DeploymentOptions");
  EXPECT_EQ(filters[0]["operator"], "eq");
  EXPECT_EQ(filters[0]["values"], nlohmann::json({"Foundry Local on Devices"}));

  EXPECT_EQ(filters[1]["field"], "VariantInformation/VariantMetadata/Device");
  EXPECT_EQ(filters[1]["operator"], "eq");
  EXPECT_EQ(filters[1]["values"], nlohmann::json({"cpu"}));

  EXPECT_EQ(filters[2]["field"], "VariantInformation/VariantMetadata/ExecutionProvider");
  EXPECT_EQ(filters[2]["operator"], "eq");
  EXPECT_EQ(filters[2]["values"], nlohmann::json({"CPUExecutionProvider"}));
}

// Verify page size default is 50.
TEST(AzureCatalogClientTest, DefaultPageSizeIs50) {
  AllDevicesEpDetector ep;
  StderrLogger logger;
  nlohmann::json captured;

  AzureCatalogClient client(
      kAssetGalleryModelsUrl, "", ep, logger,
      [&](const std::string&, const std::string& body) {
        captured = nlohmann::json::parse(body);
        return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})");
      },
      "eastus");

  client.FetchAllModels();
  EXPECT_EQ(captured["pageSize"], 50);
}

// ========================================================================
// Response parsing tests
// ========================================================================

TEST(AzureCatalogClientTest, ParsesModelResponseCorrectly) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;

  // Realistic single-model response matching the V2 asset-gallery schema.
  const char* mock_response = R"({
    "totalCount": 1,
    "summaries": [
      {
        "assetId": "azureml://registries/azureml/models/Phi-4-mini-instruct-generic-cpu/versions/2",
        "name": "Phi-4-mini-instruct-generic-cpu",
        "displayName": "Phi-4 Mini Instruct",
        "version": "2",
        "publisher": "Microsoft",
        "createdTime": "2026-05-11T22:02:42.9141627+00:00",
        "license": "MIT",
        "licenseDescription": "MIT License",
        "alias": "Phi-4-mini-instruct",
        "minFLVersion": "0.3.0",
        "inferenceTasks": ["chat-completion"],
        "modelCapabilities": ["tool-calling", "reasoning"],
        "modelLimits": {
          "textLimits": {
            "inputContextWindow": 8192,
            "maxOutputTokens": 4096
          },
          "supportedInputModalities": ["text", "image"],
          "supportedOutputModalities": ["text"]
        },
        "variantInformation": {
          "parents": [{"assetId": "azureml://registries/azureml/models/Phi-4-mini-instruct/versions/3"}],
          "variantMetadata": {
            "modelType": "ONNX",
            "quantization": ["RTN"],
            "device": "cpu",
            "executionProvider": "CPUExecutionProvider",
            "fileSizeBytes": 4294967296,
            "vRamFootprintBytes": 2147483648
          }
        }
      }
    ],
    "continuationToken": ""
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

  // Metadata properties
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR), "chat-completion");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR), "MIT");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR), "MIT License");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR), "Microsoft");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR), "Phi-4 Mini Instruct");
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 1);
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR), "0.3.0");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR), "FoundryLocal");
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT), 4096);
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT), 8192);
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT), 4096);  // 4GB → 4096 MB
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR), "text,image");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR), "text");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_CAPABILITIES_STR), "tool-calling,reasoning");
}

// Verify that invalid models (missing required fields) are filtered out
TEST(AzureCatalogClientTest, SkipsInvalidModels) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;

  // Response with one valid model and one missing assetId.
  const char* mock_response = R"({
    "totalCount": 2,
    "summaries": [
        {
          "name": "bad-model",
          "version": "1",
          "variantInformation": { "parents": [] }
        },
        {
          "assetId": "azureml://registries/azureml/models/good-model/versions/1",
          "alias": "good",
          "name": "good-model",
          "version": "1",
          "variantInformation": {
              "parents": [],
              "variantMetadata": { "device": "cpu", "executionProvider": "CPUExecutionProvider" }
          }
        }
      ],
      "continuationToken": ""
  })";

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(mock_response);
                            });

  auto infos = client.FetchAllModelInfos();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].alias, "good");
}

TEST(AzureCatalogClientTest, FiltersByMinFlVersionAndDefaultsMissingToZero) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;

  // One model is explicitly gated to an unreachable future FL version and should be filtered.
  // One model omits minFLVersion and should be treated as 0.0.0 (compatible).
  const char* mock_response = R"({
    "totalCount": 2,
    "summaries": [
      {
        "assetId": "azureml://registries/azureml/models/future-model/versions/1",
        "name": "future-model",
        "version": "1",
        "alias": "future",
        "variantInformation": {
          "parents": [],
          "variantMetadata": { "device": "cpu", "executionProvider": "CPUExecutionProvider" }
        },
        "annotations": {
          "systemCatalogData": {
            "minFLVersion": "999.0.0"
          }
        }
      },
      {
        "assetId": "azureml://registries/azureml/models/defaulted-model/versions/1",
        "name": "defaulted-model",
        "version": "1",
        "alias": "defaulted",
        "variantInformation": {
          "parents": [],
          "variantMetadata": { "device": "cpu", "executionProvider": "CPUExecutionProvider" }
        }
      }
    ],
    "continuationToken": ""
  })";

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(mock_response);
                            });

  const auto infos = client.FetchAllModelInfos();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].name, "defaulted-model");
  EXPECT_EQ(infos[0].alias, "defaulted");
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
                              EXPECT_EQ(req["skip"], 1);
                              EXPECT_EQ(req["continuationToken"], "token123");
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
// Live integration test — fetches real models from the asset-gallery endpoint
// Disabled by default. Run with: --gtest_also_run_disabled_tests
// ========================================================================

TEST(AzureCatalogClientTest, DISABLED_LiveFetchModelsFromAzure) {
  AllDevicesEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client(kAssetGalleryModelsUrl, "''", ep, logger);

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
                              return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})");
                            });

  client.FetchModelsByIds({"phi-4-mini:3", "llama-3:1"});

  // Should have made exactly one HTTP call (single filter set, no pagination).
  ASSERT_FALSE(captured.is_null());

  const auto& filters = captured["filters"];
  ASSERT_EQ(filters.size(), 2u);

  EXPECT_EQ(filters[0]["field"], "DeploymentOptions");
  EXPECT_EQ(filters[0]["values"], nlohmann::json({"Foundry Local on Devices"}));

  EXPECT_EQ(filters[1]["field"], "Name");
  EXPECT_EQ(filters[1]["values"], nlohmann::json({"phi-4-mini", "llama-3"}));

  for (const auto& f : filters) {
    EXPECT_NE(f["field"], "VariantInformation/VariantMetadata/Device");
    EXPECT_NE(f["field"], "VariantInformation/VariantMetadata/ExecutionProvider");
  }
}

TEST(AzureCatalogClientTest, FetchModelsByIdsEmptyReturnsEmptyNoHttp) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  bool http_called = false;

  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              http_called = true;
                              return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})");
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
                                const auto& filters = req["filters"];

                                bool has_name_filter = false;
                                for (const auto& f : filters) {
                                  if (f["field"] == "Name") {
                                    has_name_filter = true;
                                    EXPECT_EQ(f["values"], nlohmann::json({"old-model"}));
                                  }
                                }

                                EXPECT_TRUE(has_name_filter);

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

TEST(AzureCatalogClientTest, WithCachedModels_FullyUnresolved_DefersBYOEntryToCatalogMerge) {
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
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].model_id, "phi-4-mini:3");
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
                              for (const auto& f : req["filters"]) {
                                if (f["field"] == "VariantInformation/VariantMetadata/Device") {
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
  AzureCatalogClient client(kAssetGalleryModelsUrl, "", ep, logger,
                            [&](const std::string& url, const std::string& body) {
                              if (IsRegionProbeRequest(body)) {
                                probe_url = url;
                                return MakeProbeResponse(200, "vienna-westus2-01");
                              }

                              catalog_url = url;
                              return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})");
                            });

  client.FetchAllModels();
  EXPECT_EQ(probe_url, "https://api.catalog.azureml.ms/asset-gallery/v1.0/models");
  EXPECT_EQ(catalog_url, "https://api.catalog.azureml.ms/asset-gallery/v1.0/models");
}

TEST(AzureCatalogClientTest, DetectRegionMissingHeaderDefaultsToCentralUs) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string catalog_url;
  AzureCatalogClient client(kAssetGalleryModelsUrl, "", ep, logger,
                            [&](const std::string& url, const std::string& body) {
                              if (IsRegionProbeRequest(body)) {
                                return MakeProbeResponse(200, /*cluster_header=*/"");
                              }

                              catalog_url = url;
                              return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})");
                            });

  client.FetchAllModels();
  EXPECT_EQ(catalog_url, "https://api.catalog.azureml.ms/asset-gallery/v1.0/models");
}

TEST(AzureCatalogClientTest, DetectRegionMalformedHeaderDefaultsToCentralUs) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string catalog_url;
  AzureCatalogClient client(kAssetGalleryModelsUrl, "", ep, logger,
                            [&](const std::string& url, const std::string& body) {
                              if (IsRegionProbeRequest(body)) {
                                return MakeProbeResponse(200, "not-a-cluster-name");
                              }

                              catalog_url = url;
                              return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})");
                            });

  client.FetchAllModels();
  EXPECT_EQ(catalog_url, "https://api.catalog.azureml.ms/asset-gallery/v1.0/models");
}

TEST(AzureCatalogClientTest, DetectRegionProbeFailureDefaultsToCentralUs) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string catalog_url;
  AzureCatalogClient client(kAssetGalleryModelsUrl, "", ep, logger,
                            [&](const std::string& url, const std::string& body) {
                              if (IsRegionProbeRequest(body)) {
                                return MakeProbeResponse(503, "vienna-westus2-01");
                              }

                              catalog_url = url;
                              return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})");
                            });

  client.FetchAllModels();
  EXPECT_EQ(catalog_url, "https://api.catalog.azureml.ms/asset-gallery/v1.0/models");
}

TEST(AzureCatalogClientTest, ExplicitRegionOverridesDetection) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  bool probe_called = false;
  std::string catalog_url;
  AzureCatalogClient client(kAssetGalleryModelsUrl, "", ep, logger, [&](const std::string& url, const std::string& body) {
                              if (IsRegionProbeRequest(body)) {
                                probe_called = true;
                              }

                              catalog_url = url;
                              return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})"); }, "westeurope");

  client.FetchAllModels();
  EXPECT_FALSE(probe_called);
  EXPECT_EQ(catalog_url, "https://api.catalog.azureml.ms/asset-gallery/v1.0/models");
}

// ========================================================================
// Region-aware catalog URL tests
// ========================================================================

TEST(AzureCatalogClientTest, ActiveRegionDrivesCatalogUrl) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string captured_url;
  AzureCatalogClient client(kAssetGalleryModelsUrl, "", ep, logger, [&](const std::string& url, const std::string&) {
                              captured_url = url;
                              return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})"); }, "westus2");

  client.FetchAllModels();
  EXPECT_EQ(captured_url, "https://api.catalog.azureml.ms/asset-gallery/v1.0/models");
}

TEST(AzureCatalogClientTest, NonRegionalUrlUsedVerbatimEvenWithRegion) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  std::string captured_url;
  AzureCatalogClient client("https://custom.example.com/catalog", "", ep, logger, [&](const std::string& url, const std::string&) {
                              captured_url = url;
                              return MakeOkResponse(R"({"totalCount":0,"summaries":[],"continuationToken":""})"); }, "westus2");

  client.FetchAllModels();
  EXPECT_EQ(captured_url, "https://custom.example.com/catalog");
}

TEST(AzureCatalogClientTest, DetectedRegionStampedOnModels) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client(kAssetGalleryModelsUrl, "", ep, logger, [&](const std::string&, const std::string&) { return MakeOkResponse(MakeMockCatalogResponse({{"phi-4-mini", 3}})); }, "westus2");

  auto infos = client.FetchAllModelInfos();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].detected_region, "westus2");
}

TEST(AzureCatalogClientTest, CatalogHttpFailureThrows) {
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

TEST(AzureCatalogClientTest, MidPaginationFailureThrowsWithoutPartialCommit) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int calls = 0;
  AzureCatalogClient client("https://ai.azure.com/api/eastus/ux/v1.0", "", ep, logger, [&](const std::string&, const std::string&) {
    ++calls;
    http::HttpResponse resp;
    if (calls == 1) {
      resp.status = 200;
      resp.body = R"({
        "totalCount": 1,
        "summaries": [{
            "assetId": "azureml://registries/azureml/models/page-one-model/versions/1",
            "alias": "page-one-model",
            "name": "page-one-model",
            "version": "1",
            "variantInformation": {
                "parents": [],
                "variantMetadata": {"device": "cpu", "executionProvider": "CPUExecutionProvider"}
              }
          }],
          "nextSkip": 50,
          "continuationToken": "next"
      })";
      return resp;
    }

    resp.status = 503;
    return resp; }, "eastus");

  EXPECT_THROW(client.FetchAllModels(), fl::Exception);
  EXPECT_EQ(calls, 2);
}
