// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_catalog_client.h"
#include "catalog/catalog_client.h"
#include "c_api_types.h"
#include "exception.h"
#include "logger.h"
#include "telemetry/telemetry_logger.h"

#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace fl;

namespace {

class CatalogRecordingLogger final : public ILogger {
 public:
  void Log(LogLevel level, std::string_view message) override {
    entries.emplace_back(level, std::string(message));
  }

  std::vector<std::pair<LogLevel, std::string>> entries;
};

http::HttpResponse MakeOkResponse(std::string body) {
  http::HttpResponse response;
  response.status = 200;
  response.body = std::move(body);
  return response;
}

std::string MakeSummaryResponse(const std::vector<std::pair<std::string, int>>& models,
                                std::string continuation_token = "") {
  nlohmann::json summaries = nlohmann::json::array();
  for (const auto& [name, version] : models) {
    summaries.push_back({
        {"assetId", "azureml://registries/azureml/models/" + name + "/versions/" + std::to_string(version)},
        {"name", name},
        {"alias", name},
        {"version", std::to_string(version)},
        {"variantInformation", {
                                   {"parents", {{{"assetId", "azureml://registries/azureml/models/" + name + "/versions/1"}}}},
                                   {"variantMetadata", {
                                                           {"modelType", "ONNX"},
                                                           {"device", "cpu"},
                                                           {"executionProvider", "CPUExecutionProvider"},
                                                       }},
                               }},
    });
  }

  return nlohmann::json{
      {"totalCount", static_cast<int>(models.size())},
      {"continuationToken", std::move(continuation_token)},
      {"summaries", std::move(summaries)},
  }
      .dump();
}

class CpuOnlyEpDetector final : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {{"CPU", {"CPUExecutionProvider"}}};
  }
};

class AllDevicesEpDetector final : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {
        {"CPU", {"CPUExecutionProvider"}},
        {"GPU", {"CUDAExecutionProvider"}},
        {"NPU", {"QNNExecutionProvider"}},
    };
  }
};

}  // namespace

TEST(AzureCatalogClientTest, RequestUsesAssetGalleryContract) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  nlohmann::json captured;
  AzureCatalogClient client("https://api.catalog.azureml.ms/asset-gallery/v1.0/models", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              captured = nlohmann::json::parse(body);
                              return MakeOkResponse(R"({"totalCount":0,"continuationToken":"","summaries":[]})");
                            });

  client.FetchAllModels();

  EXPECT_EQ(captured["pageSize"], 50);
  const auto& filters = captured["filters"];
  ASSERT_EQ(filters.size(), 7u);
  EXPECT_EQ(filters[0]["field"], "type");
  EXPECT_EQ(filters[0]["values"], nlohmann::json({"models"}));
  EXPECT_EQ(filters[1]["field"], "kind");
  EXPECT_EQ(filters[1]["values"], nlohmann::json({"Versioned"}));
  EXPECT_EQ(filters[2]["field"], "annotations/systemCatalogData/deploymentOptions");
  EXPECT_EQ(filters[2]["values"], nlohmann::json({"Foundry Local on Devices"}));
  EXPECT_EQ(filters[3]["field"], "annotations/archived");
  EXPECT_EQ(filters[3]["operator"], "NotEquals");
  EXPECT_EQ(filters[3]["values"], nlohmann::json({"true"}));
  EXPECT_EQ(filters[4]["field"], "labels");
  EXPECT_EQ(filters[4]["values"], nlohmann::json({"latest"}));
  EXPECT_EQ(filters[5]["field"], "properties/variantInfo/variantMetadata/device");
  EXPECT_EQ(filters[5]["values"], nlohmann::json({"cpu"}));
  EXPECT_EQ(filters[6]["field"], "properties/variantInfo/variantMetadata/executionProvider");
}

TEST(AzureCatalogClientTest, OverrideReplacesDeploymentOptionFilter) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  nlohmann::json captured;
  AzureCatalogClient client("https://test.com", "deploymentOptions=Custom One, 'Custom Two'", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              captured = nlohmann::json::parse(body);
                              return MakeOkResponse(R"({"summaries":[]})");
                            });

  client.FetchAllModels();

  EXPECT_EQ(captured["filters"][2]["values"], nlohmann::json({"Custom One", "Custom Two"}));
}

TEST(AzureCatalogClientTest, UnqualifiedOverrideRetainsLegacyFoundryLocalFilter) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  nlohmann::json captured;
  AzureCatalogClient client("https://test.com", "test", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              captured = nlohmann::json::parse(body);
                              return MakeOkResponse(R"({"summaries":[]})");
                            });

  client.FetchAllModels();

  EXPECT_EQ(captured["filters"][2]["field"], "annotations/tags/foundryLocal");
  EXPECT_EQ(captured["filters"][2]["values"], nlohmann::json({"test"}));
}

TEST(AzureCatalogClientTest, QuotedEmptyLegacyOverrideUsesDefaultDeploymentOption) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  nlohmann::json captured;
  AzureCatalogClient client("https://test.com", "''", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              captured = nlohmann::json::parse(body);
                              return MakeOkResponse(R"({"summaries":[]})");
                            });

  client.FetchAllModels();

  EXPECT_EQ(captured["filters"][2]["field"], "annotations/systemCatalogData/deploymentOptions");
  EXPECT_EQ(captured["filters"][2]["values"], nlohmann::json({"Foundry Local on Devices"}));
}

TEST(AzureCatalogClientTest, UsesOneFilterSetPerDeviceAndProvider) {
  AllDevicesEpDetector ep;
  StderrLogger logger;
  std::vector<nlohmann::json> requests;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              requests.push_back(nlohmann::json::parse(body));
                              return MakeOkResponse(R"({"summaries":[]})");
                            });

  client.FetchAllModels();

  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests[0]["filters"][5]["values"], nlohmann::json({"cpu"}));
  EXPECT_EQ(requests[1]["filters"][5]["values"], nlohmann::json({"gpu"}));
  EXPECT_EQ(requests[2]["filters"][5]["values"], nlohmann::json({"npu"}));
}

TEST(AzureCatalogClientTest, ParsesFlatAssetGalleryResponse) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  const char* response = R"({
    "totalCount": 1,
    "continuationToken": "",
    "summaries": [{
      "assetId": "azureml://registries/azureml/models/phi-4-mini-generic-cpu/versions/2",
      "name": "phi-4-mini-generic-cpu",
      "alias": "phi-4-mini",
      "displayName": "Phi-4 Mini",
      "version": "2",
      "publisher": "Microsoft",
      "license": "MIT",
      "minFLVersion": "0.1.0",
      "isTestModel": true,
      "createdTime": "2026-06-02T07:03:01.3390586+00:00",
      "inferenceTasks": ["chat-completion"],
      "modelCapabilities": ["tool-calling", "reasoning"],
      "modelLimits": {"textLimits": {"inputContextWindow": 4096, "maxOutputTokens": 2048}},
      "variantInformation": {
        "parents": [{"assetId": "azureml://registries/azureml/models/phi-4-mini/versions/1"}],
        "variantMetadata": {
          "modelType": "ONNX", "quantization": ["RTN"], "device": "cpu",
          "executionProvider": "CPUExecutionProvider", "fileSizeBytes": 4294967296
        }
      }
    }]
  })";
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) { return MakeOkResponse(response); });

  const auto model_infos = client.FetchAllModelInfos();
  ASSERT_EQ(model_infos.size(), 1u);
  const auto& info = model_infos.front();
  EXPECT_EQ(info.model_id, "phi-4-mini-generic-cpu:2");
  EXPECT_EQ(info.alias, "phi-4-mini");
  EXPECT_EQ(info.device_type, DeviceType::kCPU);
  EXPECT_EQ(info.execution_provider, "CPUExecutionProvider");
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_QUANTIZATION_STR), "RTN");
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT), 4096);
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 1);
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT), 4096);
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT), 2048);
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR), "0.1.0");
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_IS_TEST_MODEL_INT), 1);
}

TEST(AzureCatalogClientTest, SkipsModelsWithInvalidVersions) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  const char* response = R"({"summaries":[
    {"assetId":"azureml://registries/azureml/models/missing/versions/1","name":"missing","alias":"test","variantInformation":{}},
    {"assetId":"azureml://registries/azureml/models/negative/versions/-1","name":"negative","alias":"test","version":"-1","variantInformation":{}},
    {"assetId":"azureml://registries/azureml/models/nonnumeric/versions/a","name":"nonnumeric","alias":"test","version":"abc","variantInformation":{}},
    {"assetId":"azureml://registries/azureml/models/trailing/versions/1x","name":"trailing","alias":"test","version":"1x","variantInformation":{}},
    {"assetId":"azureml://registries/azureml/models/overflow/versions/999999999999999999999","name":"overflow","alias":"test","version":"999999999999999999999","variantInformation":{}},
    {"assetId":"azureml://registries/azureml/models/valid/versions/7","name":"valid","alias":"test","version":"7","variantInformation":{}}
  ]})";
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) { return MakeOkResponse(response); });

  const auto model_infos = client.FetchAllModelInfos();
  ASSERT_EQ(model_infos.size(), 1u);
  EXPECT_EQ(model_infos.front().model_id, "valid:7");
}

TEST(AzureCatalogClientTest, ParsesFullServiceMetadataWithoutPromptOrDelimiterFields) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  const char* response = R"({
    "value": [{
      "assetId": "azureml://registries/azureml/models/phi-4-mini-generic-cpu/versions/2",
      "annotations": {
        "tags": {"supportsReasoning": "false", "foundryLocal": "test", "promptTemplate": "{}", "toolCallStart": "<tool>"},
        "systemCatalogData": {
          "alias": "phi-4-mini", "license": "MIT", "licenseDescription": "License terms",
          "minFLVersion": "0.1.0", "supportsToolCalling": true,
          "reasoningStart": "<think>", "inferenceTasks": ["chat-completion"],
          "deploymentOptions": ["Foundry Local on Devices"],
          "textContextWindow": 4096, "maxOutputTokens": 2048,
          "inputModalities": ["text", "image"], "outputModalities": ["text"]
        }
      },
      "properties": {
        "name": "phi-4-mini-generic-cpu", "version": 2,
        "creationContext": {"createdTime": "2026-06-02T07:03:01Z"},
        "variantInfo": {"variantMetadata": {"modelType": "ONNX", "device": "cpu"}}
      }
    }]
  })";
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) { return MakeOkResponse(response); });

  const auto models = client.FetchAllModels();
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front().deployment_options,
            std::vector<std::string>({"Foundry Local on Devices"}));
  EXPECT_EQ(models.front().foundry_local, "test");
  const auto converted = CatalogModelToModelInfo(models.front());
  ASSERT_TRUE(converted.has_value());
  const auto& info = *converted;
  EXPECT_EQ(info.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR), "License terms");
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);
  EXPECT_EQ(info.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 0);
  EXPECT_FALSE(info.string_properties.contains(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR));
  EXPECT_FALSE(info.string_properties.contains(FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR));

  const foundry_local::ModelInfo public_info(*AsHandle<flModelInfo>(&info));
  EXPECT_EQ(public_info.MaxOutputTokens(), 2048);
  EXPECT_EQ(public_info.InputModalities(), "text,image");
  EXPECT_EQ(public_info.OutputModalities(), "text");
  EXPECT_TRUE(public_info.IsTestModel());
}

TEST(AzureCatalogClientTest, ParsesToolCallingCapabilityFromHistoricalTag) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  const char* response = R"({
    "value": [{
      "assetId": "azureml://registries/azureml/models/qwen2.5-coder-3b/versions/1",
      "annotations": {
        "tags": {"supportsToolCalling": " true "},
        "systemCatalogData": {"alias": "qwen2.5-coder-3b"}
      },
      "properties": {
        "name": "qwen2.5-coder-3b", "version": 1,
        "variantInfo": {"variantMetadata": {"modelType": "ONNX", "device": "cpu"}}
      }
    }, {
      "assetId": "azureml://registries/azureml/models/modern-model/versions/2",
      "annotations": {
        "tags": {"supportsToolCalling": "true"},
        "systemCatalogData": {"alias": "modern-model", "supportsToolCalling": false}
      },
      "properties": {
        "name": "modern-model", "version": 2,
        "variantInfo": {"variantMetadata": {"modelType": "ONNX", "device": "cpu"}}
      }
    }]
  })";
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(response);
                            });

  const auto model_infos = client.FetchAllModelInfos();
  ASSERT_EQ(model_infos.size(), 2u);
  EXPECT_EQ(model_infos[0].model_id, "qwen2.5-coder-3b:1");
  EXPECT_EQ(model_infos[0].int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);
  EXPECT_FALSE(model_infos[0].string_properties.contains(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR));
  EXPECT_FALSE(model_infos[0].string_properties.contains(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR));
  EXPECT_EQ(model_infos[1].int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 0);
}

TEST(AzureCatalogClientTest, PreservesMinFlVersionForCatalogLevelFiltering) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(R"({"summaries":[
                                {
                                  "assetId":"azureml://registries/azureml/models/future/versions/1",
                                  "name":"future", "alias":"future", "version":"1",
                                  "minFLVersion":"999.0.0", "variantInformation":{}
                                },
                                {
                                  "assetId":"azureml://registries/azureml/models/current/versions/1",
                                  "name":"current", "alias":"current", "version":"1",
                                  "variantInformation":{}
                                }
                              ]})");
                            });

  const auto models = client.FetchAllModelInfos();
  ASSERT_EQ(models.size(), 2u);
  EXPECT_EQ(models[0].name, "future");
  EXPECT_EQ(models[0].string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR), "999.0.0");
  EXPECT_EQ(models[1].name, "current");
}

TEST(AzureCatalogClientTest, StampsModelsWithServingRegion) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              auto response = MakeOkResponse(MakeSummaryResponse({{"phi-4-mini", 1}}));
                              response.headers["azureml-served-by-cluster"] = "vienna-WestUS2-01";
                              return response;
                            });

  const auto models = client.FetchAllModelInfos();
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front().detected_region, "westus2");
}

TEST(AzureCatalogClientTest, RetriesTransientCatalogFailures) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int attempts = 0;
  http::RetryConfig retry_config;
  retry_config.max_retries = 1;
  retry_config.base_delay = std::chrono::milliseconds::zero();
  AzureCatalogClient client("https://test.com", "", ep, logger, [&](const std::string&, const std::string&) {
                              ++attempts;
                              if (attempts == 1) {
                                http::HttpResponse response;
                                response.status = 429;
                                return response;
                              }
                              return MakeOkResponse(R"({"summaries":[]})"); }, retry_config);

  EXPECT_TRUE(client.FetchAllModels().empty());
  EXPECT_EQ(attempts, 2);
}

TEST(AzureCatalogClientTest, RetriesThrownCatalogTransportFailure) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int attempts = 0;
  http::RetryConfig retry_config;
  retry_config.max_retries = 1;
  retry_config.base_delay = std::chrono::milliseconds::zero();
  AzureCatalogClient client("https://test.com", "", ep, logger, [&](const std::string&, const std::string&) {
                              ++attempts;
                              if (attempts == 1) {
                                throw std::runtime_error("connection reset");
                              }
                              return MakeOkResponse(R"({"summaries":[]})"); }, retry_config);

  EXPECT_TRUE(client.FetchAllModels().empty());
  EXPECT_EQ(attempts, 2);
}

TEST(AzureCatalogClientTest, ExhaustsRetryBudgetForThrownCatalogTransportFailures) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int attempts = 0;
  http::RetryConfig retry_config;
  retry_config.max_retries = 1;
  retry_config.base_delay = std::chrono::milliseconds::zero();
  AzureCatalogClient client("https://test.com", "", ep, logger, [&](const std::string&, const std::string&) -> http::HttpResponse {
                              ++attempts;
                              throw std::runtime_error("connection reset"); }, retry_config);

  try {
    client.FetchAllModels();
    FAIL() << "Expected catalog request to exhaust its retry budget";
  } catch (const fl::Exception& exception) {
    EXPECT_EQ(exception.code(), FOUNDRY_LOCAL_ERROR_NETWORK);
  }
  EXPECT_EQ(attempts, 2);
}

TEST(AzureCatalogClientTest, ComparesPipelineSemVerPrereleaseAndBuildVersions) {
  EXPECT_TRUE(IsFoundryLocalVersionCompatible("0.1.0-dev.202605111234", "0.1.0-dev.202605111000"));
  EXPECT_TRUE(IsFoundryLocalVersionCompatible("2.0.0-dev.202609230000", "2.0.0"));
  EXPECT_FALSE(IsFoundryLocalVersionCompatible("2.0.0-dev.202609230000", "2.0.0-rc.1"));
  EXPECT_FALSE(IsFoundryLocalVersionCompatible("2.0.0-dev.202609230000", "2.0.1"));
  EXPECT_TRUE(IsFoundryLocalVersionCompatible("2.0.1-rc.1", "2.0.1-rc.1+build.42"));
  EXPECT_TRUE(IsFoundryLocalVersionCompatible("2.0.1", "2.0.1-rc.1"));
}

TEST(AzureCatalogClientTest, SkipsAbstractParentModels) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(R"({"summaries":[{
                                "assetId":"azureml://registries/azureml/models/parent/versions/1",
                                "name":"parent", "version":"1"
                              }]})");
                            });

  EXPECT_TRUE(client.FetchAllModelInfos().empty());
}

TEST(AzureCatalogClientTest, DerivesAliasFromParentWhenExplicitAliasIsMissing) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(R"({"summaries":[{
                                "assetId":"azureml://registries/azureml/models/child/versions/1",
                                "name":"child", "version":"1",
                                "variantInformation":{"parents":[{
                                  "assetId":"azureml://registries/azureml/models/parent/versions/1"
                                }]}
                              }]})");
                            });

  const auto models = client.FetchAllModelInfos();
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front().alias, "parent");
}

TEST(AzureCatalogClientTest, SkipsEntriesMissingAssetIdOrName) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(R"({"summaries":[
                                {"name":"missing-asset","version":"1","variantInformation":{}},
                                {"assetId":"azureml://registries/azureml/models/missing-name/versions/1",
                                 "version":"1","variantInformation":{}}
                              ]})");
                            });

  EXPECT_TRUE(client.FetchAllModelInfos().empty());
}

TEST(AzureCatalogClientTest, FollowsContinuationToken) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int calls = 0;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              ++calls;
                              const auto request = nlohmann::json::parse(body);
                              if (calls == 1) {
                                return MakeOkResponse(MakeSummaryResponse({{"model-a", 1}}, "next"));
                              }
                              EXPECT_EQ(request["continuationToken"], "next");
                              return MakeOkResponse(MakeSummaryResponse({{"model-b", 1}}));
                            });

  EXPECT_EQ(client.FetchAllModels().size(), 2u);
  EXPECT_EQ(calls, 2);
}

TEST(AzureCatalogClientTest, FollowsContinuationTokenAfterEmptyPage) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int calls = 0;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              ++calls;
                              const auto request = nlohmann::json::parse(body);
                              if (calls == 1) {
                                return MakeOkResponse(MakeSummaryResponse({}, "next"));
                              }
                              EXPECT_EQ(request["continuationToken"], "next");
                              return MakeOkResponse(MakeSummaryResponse({{"model-a", 1}}));
                            });

  const auto models = client.FetchAllModels();
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front().name, "model-a");
  EXPECT_EQ(calls, 2);
}

TEST(AzureCatalogClientTest, RejectsRepeatedContinuationToken) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  int calls = 0;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              ++calls;
                              return MakeOkResponse(MakeSummaryResponse({{"model-a", 1}}, "loop"));
                            });

  EXPECT_THROW(client.FetchAllModels(), fl::Exception);
  EXPECT_EQ(calls, 2);
}

TEST(AzureCatalogClientTest, FetchModelsByIdsUsesNamesButReturnsExactVersions) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  nlohmann::json captured;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              captured = nlohmann::json::parse(body);
                              return MakeOkResponse(MakeSummaryResponse({{"phi-4-mini", 1}, {"phi-4-mini", 2}}));
                            });

  const auto models = client.FetchModelsByIds({"phi-4-mini:1"});
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front().model_id, "phi-4-mini:1");
  ASSERT_EQ(captured["filters"].size(), 3u);
  EXPECT_EQ(captured["filters"][2]["field"], "name");
  EXPECT_EQ(captured["filters"][2]["values"], nlohmann::json({"phi-4-mini"}));
}

TEST(AzureCatalogClientTest, FetchModelsByIdsHonorsDeploymentScopeAndKeepsLegacyRecords) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  for (const auto* filter_override : {"", "deploymentOptions=Private Ring"}) {
    SCOPED_TRACE(filter_override);
    nlohmann::json request;
    auto response = nlohmann::json::parse(MakeSummaryResponse(
        {{"phi", 1}, {"phi", 2}, {"phi", 3}, {"phi", 4}, {"phi", 5}}));
    const auto* matching_scope = std::string(filter_override).empty()
                                     ? "Foundry Local on Devices"
                                     : "Private Ring";
    response["summaries"][0]["deploymentOptions"] = {"Other Ring", matching_scope};
    response["summaries"][1]["deploymentOptions"] = {"Other Ring"};
    response["summaries"][3]["deploymentOptions"] = nlohmann::json::array();
    response["summaries"][4]["deploymentOptions"] = {matching_scope};
    AzureCatalogClient client("https://test.com", filter_override, ep, logger,
                              [&](const std::string&, const std::string& body) {
                                request = nlohmann::json::parse(body);
                                return MakeOkResponse(response.dump());
                              });

    const auto models = client.FetchModelsByIds({"phi:1", "phi:2", "phi:3", "phi:4"});

    ASSERT_EQ(models.size(), 2u);
    EXPECT_EQ(models[0].model_id, "phi:1");
    EXPECT_EQ(models[1].model_id, "phi:3");
    ASSERT_EQ(request["filters"].size(), 3u);
    EXPECT_EQ(request["filters"][2]["field"], "name");
  }
}

TEST(AzureCatalogClientTest, FetchModelsByIdsHonorsLegacyScopeAndKeepsRecordsWithoutMetadata) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  for (const auto* filter_override : {"foundryLocal=Private Ring", "Private Ring"}) {
    SCOPED_TRACE(filter_override);
    nlohmann::json request;
    auto response = nlohmann::json::parse(MakeSummaryResponse(
        {{"phi", 1}, {"phi", 2}, {"phi", 3}, {"phi", 4}}));
    response["summaries"][0]["foundryLocal"] = "Private Ring";
    response["summaries"][1]["foundryLocal"] = "Other Ring";
    response["summaries"][3]["foundryLocal"] = "Private Ring";
    AzureCatalogClient client("https://test.com", filter_override, ep, logger,
                              [&](const std::string&, const std::string& body) {
                                request = nlohmann::json::parse(body);
                                return MakeOkResponse(response.dump());
                              });

    const auto models = client.FetchModelsByIds({"phi:1", "phi:2", "phi:3"});

    ASSERT_EQ(models.size(), 2u);
    EXPECT_EQ(models[0].model_id, "phi:1");
    EXPECT_EQ(models[1].model_id, "phi:3");
    ASSERT_EQ(request["filters"].size(), 3u);
    EXPECT_EQ(request["filters"][2]["field"], "name");
  }
}

TEST(AzureCatalogClientTest, FetchModelsByIdsEmptyDoesNotIssueRequest) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  bool called = false;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              called = true;
                              return MakeOkResponse(R"({"summaries":[]})");
                            });

  EXPECT_TRUE(client.FetchModelsByIds({}).empty());
  EXPECT_FALSE(called);
}

TEST(AzureCatalogClientTest, ArchivedCachedVersionIsResolvedOnce) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  TelemetryLogger telemetry("catalog-test", logger);
  int calls = 0;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              ++calls;
                              if (calls == 1) {
                                return MakeOkResponse(MakeSummaryResponse({{"phi-4-mini", 2}}));
                              }

                              const auto request = nlohmann::json::parse(body);
                              const auto& filters = request["filters"];
                              const auto has_filter = [&filters](const std::string& field) {
                                return std::any_of(filters.begin(), filters.end(), [&field](const auto& filter) {
                                  return filter["field"] == field;
                                });
                              };
                              EXPECT_FALSE(has_filter("annotations/systemCatalogData/deploymentOptions"));
                              EXPECT_FALSE(has_filter("annotations/archived"));
                              return MakeOkResponse(MakeSummaryResponse({{"phi-4-mini", 1}, {"phi-4-mini", 2}}));
                            });

  const auto models =
      FetchAllModelInfosWithCachedModels(client, {"phi-4-mini:1"}, logger, telemetry, CatalogFetchInfo{});
  ASSERT_EQ(models.size(), 2u);
  EXPECT_EQ(calls, 2);
}

TEST(AzureCatalogClientTest, UnknownCachedModelIsOmitted) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  TelemetryLogger telemetry("catalog-test", logger);
  int calls = 0;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              ++calls;
                              return MakeOkResponse(calls == 1 ? MakeSummaryResponse({{"phi-4-mini", 2}})
                                                               : R"({"summaries":[]})");
                            });

  const auto models =
      FetchAllModelInfosWithCachedModels(client, {"custom-model:1"}, logger, telemetry, CatalogFetchInfo{});
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front().model_id, "phi-4-mini:2");
  EXPECT_EQ(calls, 2);
}

TEST(AzureCatalogClientTest, FetchAllVersionsOmitsLatestFilter) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  nlohmann::json captured;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              captured = nlohmann::json::parse(body);
                              return MakeOkResponse(MakeSummaryResponse({{"phi-4-mini", 1}, {"phi-4-mini", 2}}));
                            });

  const auto models = client.FetchAllVersionsByAlias("phi-4-mini");
  EXPECT_EQ(models.size(), 2u);
  for (const auto& filter : captured["filters"]) {
    EXPECT_NE(filter["field"], "labels");
  }
}

TEST(AzureCatalogClientTest, FetchAllVersionsUsesLegacyTagAliasFromFullServiceResponse) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  const char* response = R"({
    "value": [
      {
        "assetId": "azureml://registries/azureml/models/qwen2.5-0.5b-cpu/versions/1",
        "annotations": {
          "tags": {"alias": "qwen2.5-0.5b"},
          "systemCatalogData": {}
        },
        "properties": {
          "name": "qwen2.5-0.5b-cpu", "version": 1,
          "variantInfo": {
            "parents": [{"assetId": "azureml://registries/azureml/models/legacy-parent/versions/1"}],
            "variantMetadata": {"modelType": "ONNX", "device": "cpu"}
          }
        }
      },
      {
        "assetId": "azureml://registries/azureml/models/qwen2.5-0.5b-cpu/versions/2",
        "annotations": {"systemCatalogData": {"alias": "qwen2.5-0.5b"}},
        "properties": {
          "name": "qwen2.5-0.5b-cpu", "version": 2,
          "variantInfo": {"variantMetadata": {"modelType": "ONNX", "device": "cpu"}}
        }
      },
      {
        "assetId": "azureml://registries/azureml/models/qwen2.5-0.5b-cpu/versions/3",
        "annotations": {"systemCatalogData": {"alias": "qwen2.5-0.5b"}},
        "properties": {
          "name": "qwen2.5-0.5b-cpu", "version": 3,
          "variantInfo": {"variantMetadata": {"modelType": "ONNX", "device": "cpu"}}
        }
      }
    ]
  })";
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) { return MakeOkResponse(response); });

  const auto models = client.FetchAllVersionsByAlias("qwen2.5-0.5b");

  ASSERT_EQ(models.size(), 3u);
  EXPECT_EQ(models[0].version, 3);
  EXPECT_EQ(models[1].version, 2);
  EXPECT_EQ(models[2].version, 1);
  EXPECT_TRUE(std::all_of(models.begin(), models.end(), [](const ModelInfo& info) {
    return info.alias == "qwen2.5-0.5b";
  }));
}

TEST(AzureCatalogClientTest, FetchAllVersionsSortsDeduplicatesAndLimitsPerVariant) {
  AllDevicesEpDetector ep;
  StderrLogger logger;
  int calls = 0;
  const auto make_response = [](const std::vector<std::pair<std::string, int>>& entries) {
    auto response = nlohmann::json::parse(MakeSummaryResponse(entries));
    for (auto& summary : response["summaries"]) {
      summary["alias"] = "phi-4-mini";
    }
    return MakeOkResponse(response.dump());
  };
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              ++calls;
                              if (calls == 1) {
                                return make_response(
                                    {{"phi-4-mini", 1}, {"phi-4-mini", 3}, {"phi-4-mini-vision", 2}});
                              }
                              if (calls == 2) {
                                return make_response(
                                    {{"phi-4-mini", 3}, {"phi-4-mini", 2}, {"phi-4-mini-vision", 1}});
                              }
                              return make_response({{"phi-4-mini", 2}, {"phi-4-mini-vision", 2}});
                            });

  const auto models = client.FetchAllVersionsByAlias("phi-4-mini", "", 2);

  ASSERT_EQ(calls, 3);
  ASSERT_EQ(models.size(), 4u);
  EXPECT_EQ(models[0].model_id, "phi-4-mini:3");
  EXPECT_EQ(models[1].model_id, "phi-4-mini:2");
  EXPECT_EQ(models[2].model_id, "phi-4-mini-vision:2");
  EXPECT_EQ(models[3].model_id, "phi-4-mini-vision:1");
}

TEST(AzureCatalogClientTest, FetchAllVersionsLimitsAfterRemovingIncompatibleVersions) {
  AllDevicesEpDetector ep;
  StderrLogger logger;
  auto response = nlohmann::json::parse(
      MakeSummaryResponse({{"phi-4-mini", 3}, {"phi-4-mini", 2}}));
  for (auto& summary : response["summaries"]) {
    summary["alias"] = "phi-4-mini";
  }
  response["summaries"][0]["minFLVersion"] = "999.0.0";
  response["summaries"][1]["minFLVersion"] = "0.0.0";
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [&](const std::string&, const std::string&) {
                              return MakeOkResponse(response.dump());
                            });

  const auto models = client.FetchAllVersionsByAlias("phi-4-mini", "", 1);

  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0].model_id, "phi-4-mini:2");
}

TEST(AzureCatalogClientTest, AcceptsEmptyRecognizedResponseArrays) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  for (const auto* response_body : {R"({"value":[]})", R"({"summaries":[]})"}) {
    AzureCatalogClient client("https://test.com", "", ep, logger,
                              [response_body](const std::string&, const std::string&) {
                                return MakeOkResponse(response_body);
                              });
    EXPECT_TRUE(client.FetchAllModels().empty());
  }
}

TEST(AzureCatalogClientTest, SkipsMalformedRecordWithoutDiscardingValidRecords) {
  CpuOnlyEpDetector ep;
  for (const auto* field : {"value", "summaries"}) {
    SCOPED_TRACE(field);
    CatalogRecordingLogger logger;
    auto records = nlohmann::json::parse(MakeSummaryResponse(
        {{"valid-before", 1}, {"malformed", 1}, {"also-malformed", 1}, {"valid-after", 1}}))["summaries"];
    records[1]["inferenceTasks"] = {"chat-completion", 42};
    records[2]["deploymentOptions"] = {false};
    const nlohmann::json response = {{field, records}};
    auto parsed = response.get<AzureCatalogResponse>();
    EXPECT_EQ(parsed.skipped_record_count, 2u);
    from_json(nlohmann::json{{field, nlohmann::json::array()}}, parsed);
    EXPECT_EQ(parsed.skipped_record_count, 0u);
    EXPECT_TRUE(parsed.models.empty());
    AzureCatalogClient client("https://test.com", "", ep, logger,
                              [&](const std::string&, const std::string&) {
                                return MakeOkResponse(response.dump());
                              });

    const auto models = client.FetchAllModels();

    ASSERT_EQ(models.size(), 2u);
    EXPECT_EQ(models[0].name, "valid-before");
    EXPECT_EQ(models[1].name, "valid-after");
    ASSERT_EQ(logger.entries.size(), 1u);
    EXPECT_EQ(logger.entries[0].first, LogLevel::Warning);
    EXPECT_EQ(logger.entries[0].second,
              "catalog response skipped 2 of 4 records due to malformed metadata");
  }
}

TEST(AzureCatalogClientTest, RejectsAllMalformedPageEvenAfterValidPage) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  for (const auto* field : {"value", "summaries"}) {
    for (const bool valid_first_page : {false, true}) {
      SCOPED_TRACE(field);
      SCOPED_TRACE(valid_first_page);
      auto records = nlohmann::json::parse(MakeSummaryResponse({{"malformed", 1}}))["summaries"];
      records[0]["inferenceTasks"] = {42};
      const nlohmann::json response = {{field, records}};
      int calls = 0;
      AzureCatalogClient client("https://test.com", "", ep, logger,
                                [&](const std::string&, const std::string&) {
                                  ++calls;
                                  if (valid_first_page && calls == 1) {
                                    return MakeOkResponse(MakeSummaryResponse({{"valid", 1}}, "next"));
                                  }
                                  return MakeOkResponse(response.dump());
                                });

      try {
        client.FetchAllModelInfos();
        FAIL() << "Expected all-malformed page to fail the endpoint";
      } catch (const fl::Exception& exception) {
        EXPECT_EQ(exception.code(), FOUNDRY_LOCAL_ERROR_INTERNAL);
        EXPECT_NE(std::string(exception.what()).find("skipped 1 of 1 records"), std::string::npos);
        EXPECT_NE(std::string(exception.what()).find("no valid records on non-empty page"),
                  std::string::npos);
      }
      EXPECT_EQ(calls, valid_first_page ? 2 : 1);
    }
  }
}

TEST(AzureCatalogClientTest, RejectsMalformedSuccessfulResponseShapes) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  for (const auto* response_body : {R"({})", R"([])", R"({"error":{"code":"bad"}})",
                                    R"({"summaries":"not an array"})"}) {
    AzureCatalogClient client("https://test.com", "", ep, logger,
                              [response_body](const std::string&, const std::string&) {
                                return MakeOkResponse(response_body);
                              });
    try {
      client.FetchAllModels();
      FAIL() << "Expected malformed catalog response to throw";
    } catch (const fl::Exception& exception) {
      EXPECT_EQ(exception.code(), FOUNDRY_LOCAL_ERROR_INTERNAL);
    }
  }
}

TEST(AzureCatalogClientTest, RaisesNetworkErrorForFailedRequest) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient client("https://test.com", "", ep, logger,
                            [](const std::string&, const std::string&) {
                              http::HttpResponse response;
                              response.status = 503;
                              return response;
                            });

  try {
    client.FetchAllModels();
    FAIL() << "Expected catalog request to throw";
  } catch (const fl::Exception& exception) {
    EXPECT_EQ(exception.code(), FOUNDRY_LOCAL_ERROR_NETWORK);
  }
}

TEST(AzureCatalogClientTest, FetchAllVersionsHonorsDeploymentOptionAndKeepsLegacyRecords) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  nlohmann::json request;
  auto response = nlohmann::json::parse(
      MakeSummaryResponse({{"matching", 3}, {"different", 2}, {"legacy", 1}}));
  for (auto& summary : response["summaries"]) {
    summary["alias"] = "phi";
  }
  response["summaries"][0]["deploymentOptions"] = {"Private Ring"};
  response["summaries"][1]["deploymentOptions"] = {"Other Ring"};

  AzureCatalogClient client("https://test.com", "deploymentOptions=Private Ring", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              request = nlohmann::json::parse(body);
                              return MakeOkResponse(response.dump());
                            });

  const auto models = client.FetchAllVersionsByAlias("phi");

  ASSERT_EQ(models.size(), 2u);
  EXPECT_TRUE(std::any_of(models.begin(), models.end(), [](const ModelInfo& model) {
    return model.name == "matching";
  }));
  EXPECT_TRUE(std::any_of(models.begin(), models.end(), [](const ModelInfo& model) {
    return model.name == "legacy";
  }));
  EXPECT_TRUE(std::none_of(request["filters"].begin(), request["filters"].end(),
                           [](const nlohmann::json& filter) {
                             return filter["field"] ==
                                    "annotations/systemCatalogData/deploymentOptions";
                           }));
}

TEST(AzureCatalogClientTest, FetchAllVersionsHonorsLegacyFilterAndKeepsRecordsWithoutMetadata) {
  CpuOnlyEpDetector ep;
  StderrLogger logger;
  nlohmann::json request;
  auto response = nlohmann::json::parse(
      MakeSummaryResponse({{"matching", 3}, {"different", 2}, {"legacy", 1}}));
  for (auto& summary : response["summaries"]) {
    summary["alias"] = "phi";
  }
  response["summaries"][0]["foundryLocal"] = "Private Ring";
  response["summaries"][1]["foundryLocal"] = "Other Ring";

  AzureCatalogClient client("https://test.com", "foundryLocal=Private Ring", ep, logger,
                            [&](const std::string&, const std::string& body) {
                              request = nlohmann::json::parse(body);
                              return MakeOkResponse(response.dump());
                            });

  const auto models = client.FetchAllVersionsByAlias("phi");

  ASSERT_EQ(models.size(), 2u);
  EXPECT_TRUE(std::any_of(models.begin(), models.end(), [](const ModelInfo& model) {
    return model.name == "matching";
  }));
  EXPECT_TRUE(std::any_of(models.begin(), models.end(), [](const ModelInfo& model) {
    return model.name == "legacy";
  }));
  EXPECT_TRUE(std::none_of(request["filters"].begin(), request["filters"].end(),
                           [](const nlohmann::json& filter) {
                             return filter["field"] == "annotations/tags/foundryLocal";
                           }));
}