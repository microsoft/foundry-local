// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "catalog/azure_model_catalog.h"
#include "catalog/azure_catalog_client.h"
#include "catalog/catalog_cache.h"
#include "catalog/catalog_client.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "model.h"
#include "model_info.h"
#include "util/scope_guard.h"
#include "utils.h"
#include "utils/temp_path.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <onnxruntime_c_api.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
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
  std::vector<ModelInfo> all_versions;
  int fetch_all_calls = 0;
  int fetch_by_id_calls = 0;
  int fetch_all_versions_calls = 0;
  std::vector<int> requested_max_versions;
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

  std::vector<ModelInfo> FetchAllVersionsByAlias(
      const std::string& model_alias,
      const std::string& model_name,
      int max_versions) override {
    ++behavior_->fetch_all_versions_calls;
    behavior_->requested_max_versions.push_back(max_versions);
    std::vector<ModelInfo> result;
    for (const auto& info : behavior_->all_versions) {
      if (info.alias == model_alias && (model_name.empty() || info.name == model_name)) {
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
                        ITelemetry& telemetry,
                        ClientFactory client_factory)
      : AzureModelCatalog(std::move(catalog_urls), std::move(cache_dir), std::move(model_factory), ep_detector, logger,
                          cache_only, telemetry),
        client_factory_(std::move(client_factory)) {}

 protected:
  std::unique_ptr<ICatalogClient> CreateCatalogClient(const std::string& url,
                                                      const std::string& filter) const override {
    return client_factory_(url, filter);
  }

 private:
  ClientFactory client_factory_;
};

class RecordingCatalogTelemetry final : public TelemetryLogger {
 public:
  RecordingCatalogTelemetry() : TelemetryLogger("catalog-test", fl::test::NullLog()) {}

  void RecordCatalogFetch(const CatalogFetchInfo& info) override { calls.push_back(info); }

  std::vector<CatalogFetchInfo> calls;
};

class RecordingCatalogLogger final : public ILogger {
 public:
  void Log(LogLevel level, std::string_view message) override { messages.emplace_back(level, message); }

  std::vector<std::pair<LogLevel, std::string>> messages;
};

class CountingCatalogEpDetector final : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    ++calls;
    return devices;
  }

  mutable int calls = 0;
  mutable int capacity_calls = 0;
  std::map<std::string, std::vector<DeviceCapacity>> GetDeviceCapacities() const override {
    ++capacity_calls;
    return capacities;
  }
  std::map<std::string, std::vector<DeviceCapacity>> capacities;
  std::map<std::string, std::vector<std::string>> devices{{"CPU", {"CPUExecutionProvider"}}};
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

std::vector<ModelInfo> MakeMinVersionModelInfos() {
  auto compatible = MakeModelInfo("compatible:1", "compatible", 1, "compatible", "SnapshotProvider");
  compatible.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR] = "0.0.0";
  auto future = MakeModelInfo("future:1", "future", 1, "future", "SnapshotProvider");
  future.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR] = "999.0.0";
  auto missing = MakeModelInfo("missing:1", "missing", 1, "missing", "SnapshotProvider");
  auto malformed = MakeModelInfo("malformed:1", "malformed", 1, "malformed", "SnapshotProvider");
  malformed.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR] = "not-a-version";
  return {std::move(compatible), std::move(future), std::move(missing), std::move(malformed)};
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

void ExpectCompatibleMinVersionModels(const std::vector<Model*>& models) {
  EXPECT_NE(FindVariant(models, "compatible:1"), nullptr);
  EXPECT_NE(FindVariant(models, "missing:1"), nullptr);
  EXPECT_EQ(FindVariant(models, "future:1"), nullptr);
  EXPECT_EQ(FindVariant(models, "malformed:1"), nullptr);
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
      bool cache_only = false,
      const IEpDetector* ep_detector = nullptr) {
    auto model_factory = [this](ModelInfo info, std::string local_path) {
      ++model_handle_factory_calls_;
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
                                                   std::move(model_factory),
                                                   ep_detector ? *ep_detector : services_.ep_detector, catalog_logger_,
                                                   cache_only, telemetry_, std::move(client_factory));
  }

  fl::test::TempPath cache_directory_ = fl::test::TempPath::CreateTempDir("fl_azure_model_catalog_");
  fl::test::FakeServiceBindings services_;
  RecordingCatalogTelemetry telemetry_;
  RecordingCatalogLogger catalog_logger_;
  std::unordered_map<std::string, std::shared_ptr<CatalogBehavior>> behaviors_;
  int factory_calls_ = 0;
  int model_handle_factory_calls_ = 0;
};

TEST(AzureModelCatalogCapacityEndToEndTest, RealCudaCapacityFiltersParsedCatalogAndOfflineSnapshot) {
  const auto provider_library = Utils::GetEnv("FOUNDRY_LOCAL_TEST_CUDA_EP_LIBRARY");
  if (!provider_library || provider_library->empty()) {
    GTEST_SKIP() << "Set FOUNDRY_LOCAL_TEST_CUDA_EP_LIBRARY to the matching CUDA provider library";
  }
  const auto* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
  ASSERT_NE(api, nullptr);
  const auto check_status = [api](OrtStatus* status) {
    if (!status) {
      return ::testing::AssertionSuccess();
    }
    const std::string message = api->GetErrorMessage(status);
    api->ReleaseStatus(status);
    return ::testing::AssertionFailure() << message;
  };
  OrtEnv* env = nullptr;
  ASSERT_TRUE(check_status(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "catalog-capacity-e2e", &env)));
  ScopeGuard release_env([&]() noexcept { api->ReleaseEnv(env); });
  const auto library_path = fs::path(*provider_library);
  ASSERT_TRUE(check_status(api->RegisterExecutionProviderLibrary(env, "catalog-capacity-cuda", library_path.c_str())));
  ScopeGuard unregister_provider([&]() noexcept {
    if (auto* status = api->UnregisterExecutionProviderLibrary(env, "catalog-capacity-cuda")) {
      api->ReleaseStatus(status);
    }
  });

  fl::test::FakeServiceBindings services;
  EpDetector detector(*api, *env, {}, services.logger, services.telemetry);
  const auto capacities = detector.GetDeviceCapacities();
  const auto cuda = capacities.find("CUDAExecutionProvider");
  ASSERT_NE(cuda, capacities.end()) << "Configured CUDA provider must expose at least one visible GPU";
  ASSERT_FALSE(cuda->second.empty());
  const OrtEpDevice* const* ort_devices = nullptr;
  size_t device_count = 0;
  ASSERT_TRUE(check_status(api->GetEpDevices(env, &ort_devices, &device_count)));
  uint64_t largest_total = 0;
  size_t cuda_index = 0;
  for (size_t index = 0; index < device_count; ++index) {
    const auto* device = ort_devices[index];
    const char* provider = api->EpDevice_EpName(device);
    if (!provider || std::string_view(provider) != "CUDAExecutionProvider" ||
        api->HardwareDevice_Type(api->EpDevice_Device(device)) != OrtHardwareDeviceType_GPU) {
      continue;
    }
    uint64_t total = 0;
    ASSERT_TRUE(check_status(api->EpDevice_GetMemoryCapacity(device, OrtDeviceMemoryType_DEFAULT, &total, nullptr)));
    ASSERT_LT(cuda_index, cuda->second.size());
    const auto& capacity = cuda->second[cuda_index++];
    ASSERT_TRUE(capacity.total_vram_bytes.has_value()) << "Configured CUDA provider must support total capacity";
    EXPECT_EQ(*capacity.total_vram_bytes, total);
    largest_total = std::max(largest_total, total);
  }
  ASSERT_EQ(cuda_index, cuda->second.size());
  ASSERT_GT(largest_total, 0u);
  ASSERT_LT(largest_total, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
  const auto oversized = static_cast<int64_t>(largest_total + 1);

  nlohmann::json summaries = nlohmann::json::array();
  const auto add_variant = [&](const std::string& name, const std::string& alias, const std::string& device,
                               const std::string& provider, std::optional<int64_t> required) {
    nlohmann::json metadata = {{"device", device}, {"executionProvider", provider}, {"modelType", "ONNX"}};
    if (required) {
      metadata["vRamFootprintBytes"] = *required;
    }
    summaries.push_back({
        {"assetId", "azureml://registries/azureml/models/" + name + "/versions/1"},
        {"name", name},
        {"alias", alias},
        {"version", "1"},
        {"foundryLocal", ""},
        {"deploymentOptions", {"Foundry Local on Devices"}},
        {"variantInformation", {{"variantMetadata", std::move(metadata)}}},
    });
  };
  add_variant("exact-cuda-gpu", "exact", "gpu", "CUDAExecutionProvider", static_cast<int64_t>(largest_total));
  add_variant("mixed-cuda-gpu", "mixed", "gpu", "CUDAExecutionProvider", oversized);
  add_variant("mixed-generic-cpu", "mixed", "cpu", "CPUExecutionProvider", oversized);
  add_variant("hidden-cuda-gpu", "hidden", "gpu", "CUDAExecutionProvider", oversized);
  add_variant("unknown-cuda-gpu", "unknown", "gpu", "CUDAExecutionProvider", std::nullopt);
  add_variant("zero-cuda-gpu", "zero", "gpu", "CUDAExecutionProvider", 0);
  const auto body = nlohmann::json({{"summaries", summaries}}).dump();
  int requests = 0;
  const auto client_factory = [&](const std::string& url, const std::string& filter) {
    return std::make_unique<AzureCatalogClient>(url, filter, detector, services.logger,
                                                [&](const std::string&, const std::string&) {
                                                  ++requests;
                                                  http::HttpResponse response;
                                                  response.status = 200;
                                                  response.body = body;
                                                  return response;
                                                });
  };
  const auto model_factory = [&](ModelInfo info, std::string local_path) {
    return Model::FromModelInfo(std::move(info), std::move(local_path), services.download_manager,
                                services.model_load_manager);
  };
  const auto cache = fl::test::TempPath::CreateTempDir("fl_capacity_e2e_");
  const std::vector<std::pair<std::string, std::optional<std::string>>> urls{{"https://catalog.test", std::nullopt}};
  const auto verify = [&](AzureModelCatalog& catalog) {
    const auto models = catalog.ListModels();
    ASSERT_EQ(models.size(), 4u);
    EXPECT_EQ(FindVariant(models, "mixed-cuda-gpu:1"), nullptr);
    EXPECT_EQ(FindVariant(models, "hidden-cuda-gpu:1"), nullptr);
    EXPECT_EQ(catalog.GetModel("hidden"), nullptr);
    auto* exact = catalog.GetModel("exact");
    ASSERT_NE(exact, nullptr);
    EXPECT_EQ(exact->Id(), "exact-cuda-gpu:1");
    const auto* requirement = exact->Info().GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT);
    ASSERT_NE(requirement, nullptr);
    EXPECT_EQ(*requirement, static_cast<int64_t>(largest_total));
    auto* mixed = catalog.GetModel("mixed");
    ASSERT_NE(mixed, nullptr);
    EXPECT_EQ(mixed->Id(), "mixed-generic-cpu:1");
    EXPECT_NE(catalog.GetModel("unknown"), nullptr);
    EXPECT_NE(catalog.GetModel("zero"), nullptr);
  };
  TestAzureModelCatalog live(urls, cache.string(), model_factory, detector, services.logger, false,
                             services.telemetry, client_factory);
  ASSERT_NO_FATAL_FAILURE(verify(live));
  EXPECT_GT(requests, 0);

  CatalogCache snapshot(cache.string(), services.logger);
  snapshot.Load();
  const auto persisted = snapshot.GetCachedModels();
  ASSERT_TRUE(persisted.has_value());
  EXPECT_EQ(persisted->size(), summaries.size());
  const int live_requests = requests;
  TestAzureModelCatalog offline(urls, cache.string(), model_factory, detector, services.logger, true,
                                services.telemetry, client_factory);
  ASSERT_NO_FATAL_FAILURE(verify(offline));
  EXPECT_EQ(requests, live_requests) << "Offline filtering must not fetch catalog metadata";
}

TEST(AzureModelCatalogRuntimeTest, HostedDeviceChangesInvalidateMetadataWithoutChangingLocalProviderAvailability) {
  static std::atomic<uint64_t> sequence{0};
  const auto scratch = fs::path(FOUNDRY_LOCAL_TEST_SCRATCH_DIR);
  ASSERT_TRUE(scratch.is_absolute());
  fs::create_directories(scratch);
  ASSERT_FALSE(fs::is_symlink(scratch));
  fs::path root;
  do {
    root = scratch / ("catalog-devices-" + std::to_string(test::CurrentPid()) + "-" + std::to_string(sequence++));
  } while (!fs::create_directory(root));

  ScopeGuard cleanup([&]() noexcept {
    std::error_code error;
    fs::remove_all(root, error);
  });
  test::FakeServiceBindings services;
  RecordingCatalogTelemetry telemetry;
  auto behavior = std::make_shared<CatalogBehavior>();
  behavior->all_models = {MakeModelInfo("cpu:1", "cpu", 1, "cpu", "test")};
  TestAzureModelCatalog catalog(
      {{"https://catalog.example", std::nullopt}}, root.string(),
      [&](ModelInfo info, std::string path) {
        return Model::FromModelInfo(std::move(info), std::move(path), services.download_manager,
                                    services.model_load_manager);
      },
      services.ep_detector, services.logger, false, telemetry,
      [&](const auto&, const auto&) { return std::make_unique<FakeCatalogClient>(behavior); });
  EXPECT_NE(FindVariant(catalog.ListModels(), "cpu:1"), nullptr);
  EXPECT_EQ(behavior->fetch_all_calls, 1);
  const auto local_devices = services.ep_detector.GetAvailableDevicesToEPs();
  const std::map<std::string, std::vector<std::string>> hosted_devices{
      {"CPU", {"CPUExecutionProvider"}}, {"GPU", {"host-only-provider"}}};
  catalog.SetRuntimeDevices(hosted_devices);
  behavior->all_models = {MakeModelInfo("gpu:1", "gpu", 1, "gpu", "test")};
  EXPECT_NE(FindVariant(catalog.ListModels(), "gpu:1"), nullptr);
  EXPECT_EQ(behavior->fetch_all_calls, 2);
  catalog.SetRuntimeDevices(hosted_devices);
  static_cast<void>(catalog.ListModels());
  EXPECT_EQ(behavior->fetch_all_calls, 2);
  catalog.SetRuntimeDevices(local_devices);
  static_cast<void>(catalog.ListModels());
  EXPECT_EQ(behavior->fetch_all_calls, 3);
  EXPECT_EQ(services.ep_detector.GetAvailableDevicesToEPs(), local_devices);
}

TEST_F(AzureModelCatalogTest, LiveFetchBucketsConfiguredCatalogsAsCustom) {
  const std::string azure_url = "https://ai.azure.com/api/eastus/ux/v1.0?token=private";
  const std::string custom_url = "https://private.example/tenant/private-models?token=private";
  AddBehavior(azure_url);
  AddBehavior(custom_url);
  auto catalog = CreateCatalog({{azure_url, std::nullopt}, {custom_url, std::nullopt}});

  catalog->ListModels();

  ASSERT_EQ(telemetry_.calls.size(), 2u);
  const auto& azure = telemetry_.calls[0];
  EXPECT_EQ(azure.operation, "FetchAll");
  EXPECT_EQ(azure.endpoint, "custom");
  EXPECT_TRUE(azure.region.empty());
  EXPECT_TRUE(azure.format.empty());
  EXPECT_EQ(azure.status, ActionStatus::kSuccess);
  EXPECT_EQ(azure.model_count, 0);
  EXPECT_GE(azure.duration_ms, 0);
  EXPECT_FALSE(azure.user_agent.empty());
  EXPECT_FALSE(azure.correlation_id.empty());
  const auto& custom = telemetry_.calls[1];
  EXPECT_EQ(custom.endpoint, "custom");
  EXPECT_TRUE(custom.region.empty());
  EXPECT_TRUE(custom.format.empty());
  EXPECT_EQ(custom.correlation_id, azure.correlation_id);
}

TEST_F(AzureModelCatalogTest, LiveFetchRecordsDimensionsForDefaultCatalogOnly) {
  const std::string default_url = "https://API.CATALOG.AZUREML.MS/asset-gallery/v1.0/models?ignored=true";
  AddBehavior(default_url);
  auto catalog = CreateCatalog({{default_url, std::nullopt}});

  catalog->ListModels();

  ASSERT_EQ(telemetry_.calls.size(), 1u);
  EXPECT_EQ(telemetry_.calls[0].endpoint, "api.catalog.azureml.ms");
  EXPECT_TRUE(telemetry_.calls[0].region.empty());
  EXPECT_EQ(telemetry_.calls[0].format, "asset-gallery/v1.0/models");
}

TEST_F(AzureModelCatalogTest, TelemetryRejectsOversizedUrlsAndPrivatePathsOnThePublicHost) {
  const std::string oversized_url = "https://private.example/" + std::string(1024 * 1024, 'x');
  const std::string private_url = "https://api.catalog.azureml.ms/tenant/private-models";
  AddBehavior(oversized_url);
  AddBehavior(private_url);
  auto catalog = CreateCatalog({{oversized_url, std::nullopt}, {private_url, std::nullopt}});
  catalog->ListModels();

  ASSERT_EQ(telemetry_.calls.size(), 2u);
  for (const auto& info : telemetry_.calls) {
    EXPECT_EQ(info.endpoint, "custom");
    EXPECT_TRUE(info.format.empty());
    EXPECT_TRUE(info.region.empty());
  }
}

TEST_F(AzureModelCatalogTest, FailedLiveFetchRecordsFailureBeforeSnapshotFallback) {
  const std::string url = "https://private.example/models";
  AddBehavior(url, true);
  auto catalog = CreateCatalog({{url, std::nullopt}});

  catalog->ListModels();

  ASSERT_EQ(telemetry_.calls.size(), 1u);
  EXPECT_EQ(telemetry_.calls[0].status, ActionStatus::kDependencyFailure);
  EXPECT_EQ(telemetry_.calls[0].error_message, "catalog request failed");
  EXPECT_EQ(telemetry_.calls[0].endpoint, "custom");
}

TEST_F(AzureModelCatalogTest, CachedModelLookupSharesRefreshCorrelation) {
  const std::string url = "https://ai.azure.com/api/eastus/ux/v1.0";
  auto behavior = AddBehavior(url);
  behavior->models_by_id = {MakeModelInfo("old-model:1", "old-model", 1, "old", "Azure")};
  AddLocalModel("old-model:1", "old-model-1");
  auto catalog = CreateCatalog({{url, std::nullopt}});

  catalog->ListModels();

  ASSERT_EQ(telemetry_.calls.size(), 2u);
  EXPECT_EQ(telemetry_.calls[0].operation, "FetchAll");
  EXPECT_EQ(telemetry_.calls[1].operation, "FetchByIds");
  EXPECT_EQ(telemetry_.calls[1].status, ActionStatus::kSuccess);
  EXPECT_EQ(telemetry_.calls[1].model_count, 1);
  EXPECT_EQ(telemetry_.calls[1].correlation_id, telemetry_.calls[0].correlation_id);
}

TEST_F(AzureModelCatalogTest, GpuCapacityFiltersDefaultWithoutHidingCpuFallback) {
  const std::string url = "https://catalog.test";
  auto gpu = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "CUDAExecutionProvider";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  auto cpu = MakeModelInfo("model-cpu:1", "model-cpu", 1, "model", "Azure");
  cpu.device_type = DeviceType::kCPU;
  cpu.execution_provider = "CPUExecutionProvider";
  cpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  auto gpu_only = gpu;
  gpu_only.model_id = "gpu-only:1";
  gpu_only.name = "gpu-only";
  gpu_only.alias = "gpu-only";
  auto behavior = AddBehavior(url);
  behavior->all_models = {gpu, cpu, gpu_only};
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{8}};
  auto catalog = CreateCatalog({{url, std::nullopt}}, false, &detector);

  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), cpu.model_id);
  EXPECT_EQ(catalog->GetModel("gpu-only"), nullptr);
  EXPECT_EQ(FindVariant(catalog->ListModels(), gpu.model_id), nullptr);
  EXPECT_EQ(catalog->ListModels().size(), 1u);

  detector.capacities["CUDAExecutionProvider"] = {{8}, {16}};
  catalog->InvalidateCache();
  ASSERT_NE(catalog->GetModel("gpu-only"), nullptr);
  EXPECT_EQ(catalog->GetModel("gpu-only")->Id(), gpu_only.model_id);

  detector.capacities["CUDAExecutionProvider"] = {{8}, {std::nullopt}};
  catalog->InvalidateCache();
  EXPECT_NE(catalog->GetModel("gpu-only"), nullptr);

  detector.capacities.clear();
  catalog->InvalidateCache();
  EXPECT_NE(catalog->GetModel("gpu-only"), nullptr);

  detector.capacities["CUDAExecutionProvider"] = {{8}};
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("gpu-only"), nullptr);
  EXPECT_EQ(catalog->ListModels().size(), 1u);
  ASSERT_NE(catalog->GetModel("model"), nullptr);
  EXPECT_EQ(catalog->GetModel("model")->Id(), cpu.model_id);
}

TEST_F(AzureModelCatalogTest, InitialPopulationRetainsCapacityHiddenCachedVariants) {
  const std::string url = "https://catalog.test";
  auto gpu = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "CUDAExecutionProvider";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  AddBehavior(url)->all_models = {gpu};
  const auto local_path = AddLocalModel(gpu.model_id, "oversized-cuda");
  WriteSnapshot({gpu});
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{8}};

  for (const bool cache_only : {false, true}) {
    SCOPED_TRACE(cache_only);
    auto catalog = CreateCatalog({{url, std::nullopt}}, cache_only, &detector);
    EXPECT_TRUE(catalog->ListModels().empty());
    EXPECT_EQ(catalog->GetModel(gpu.alias), nullptr);
    const auto cached = catalog->GetCachedModels();
    ASSERT_EQ(cached.size(), 1u);
    EXPECT_EQ(cached.front()->Id(), gpu.model_id);
    EXPECT_EQ(cached.front()->LocalPathSnapshot(), local_path.string());
    EXPECT_EQ(catalog->GetModelVariant(gpu.model_id), cached.front());
  }
}

TEST_F(AzureModelCatalogTest, ExplicitFetchRetainsCapacityHiddenVariantWithoutChangingDefault) {
  const std::string url = "https://catalog.test";
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.device_type = DeviceType::kCPU;
  cpu.execution_provider = "CPUExecutionProvider";
  auto gpu = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "CUDAExecutionProvider";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  auto behavior = AddBehavior(url);
  behavior->all_models = {cpu};
  behavior->models_by_id = {gpu};
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{8}};
  auto catalog = CreateCatalog({{url, std::nullopt}}, false, &detector);
  auto* model = catalog->GetModel(cpu.alias);
  ASSERT_NE(model, nullptr);

  auto* fetched = catalog->GetModelVariant(gpu.model_id);
  ASSERT_NE(fetched, nullptr);
  EXPECT_EQ(fetched->Id(), gpu.model_id);
  EXPECT_EQ(catalog->GetModelVariant(gpu.model_id), fetched);
  EXPECT_EQ(behavior->fetch_by_id_calls, 1);
  EXPECT_EQ(model->Id(), cpu.model_id);
  EXPECT_EQ(model->Variants().size(), 1u);
  EXPECT_EQ(FindVariant(catalog->ListModels(), gpu.model_id), nullptr);

  detector.capacities["CUDAExecutionProvider"] = {{16}};
  catalog->InvalidateCache();
  EXPECT_EQ(FindVariant(catalog->ListModels(), gpu.model_id), fetched);
}

TEST_F(AzureModelCatalogTest, RefreshHidesIncompatibleVariantsWithoutInvalidatingHandles) {
  const std::string url = "https://catalog.test";
  auto gpu = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "CUDAExecutionProvider";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.device_type = DeviceType::kCPU;
  cpu.execution_provider = "CPUExecutionProvider";
  AddBehavior(url)->all_models = {gpu, cpu};
  AddLocalModel(gpu.model_id, "retained-cuda");
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{16}};
  auto catalog = CreateCatalog({{url, std::nullopt}}, false, &detector);
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  auto* retained_gpu = FindVariant(catalog->ListModels(), gpu.model_id);
  ASSERT_NE(retained_gpu, nullptr);
  ASSERT_EQ(model->Variants().size(), 2u);

  detector.capacities["CUDAExecutionProvider"] = {{8}};
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(model->Id(), cpu.model_id);
  EXPECT_EQ(model->Variants().size(), 1u);
  EXPECT_EQ(FindVariant(catalog->ListModels(), gpu.model_id), nullptr);
  EXPECT_EQ(retained_gpu->Info().model_id, gpu.model_id);
  EXPECT_EQ(catalog->GetModelVariant(gpu.model_id), retained_gpu);
  EXPECT_NE(FindVariant(catalog->GetCachedModels(), gpu.model_id), nullptr);

  detector.capacities["CUDAExecutionProvider"] = {{16}};
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(model->Variants().size(), 2u);
  EXPECT_EQ(FindVariant(catalog->ListModels(), gpu.model_id), retained_gpu);
}

TEST_F(AzureModelCatalogTest, GpuCapacityReevaluatesFullSnapshotOffline) {
  const std::string url = "https://catalog.test";
  auto gpu = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "CUDAExecutionProvider";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  AddBehavior(url)->all_models = {gpu};
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{8}};
  auto live = CreateCatalog({{url, std::nullopt}}, false, &detector);
  EXPECT_TRUE(live->ListModels().empty());

  auto offline = CreateCatalog({{url, std::nullopt}}, true, &detector);
  EXPECT_TRUE(offline->ListModels().empty());
  detector.capacities["CUDAExecutionProvider"] = {{16}};
  offline->InvalidateCache();
  auto* model = offline->GetModel("model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), gpu.model_id);
  const auto* requirement = model->Info().GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT);
  ASSERT_NE(requirement, nullptr);
  EXPECT_EQ(*requirement, 16);
}

TEST_F(AzureModelCatalogTest, GpuCapacityDoesNotUseOtherProviderOrFilterMissingRequirements) {
  const std::string url = "https://catalog.test";
  auto gpu = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "CUDA";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  auto unknown = gpu;
  unknown.model_id = "unknown:1";
  unknown.name = "unknown";
  unknown.alias = "unknown";
  unknown.int_properties.clear();
  auto npu = gpu;
  npu.model_id = "npu:1";
  npu.name = "npu";
  npu.alias = "npu";
  npu.device_type = DeviceType::kNPU;
  npu.execution_provider = "QNNExecutionProvider";
  AddBehavior(url)->all_models = {gpu, unknown, npu};
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider", "WebGpuExecutionProvider"};
  detector.devices["NPU"] = {"QNNExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{8}, {8}};
  detector.capacities["WebGpuExecutionProvider"] = {{64}};
  auto catalog = CreateCatalog({{url, std::nullopt}}, false, &detector);

  EXPECT_EQ(catalog->GetModel("model"), nullptr);
  EXPECT_NE(catalog->GetModel("unknown"), nullptr);
  EXPECT_NE(catalog->GetModel("npu"), nullptr);
}

TEST_F(AzureModelCatalogTest, GenericGpuCapacityChecksEachEligibleProvider) {
  const std::string url = "https://catalog.test";
  auto gpu = MakeModelInfo("model-generic-gpu:1", "model-generic-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "DmlExecutionProvider";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  AddBehavior(url)->all_models = {gpu};
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider", "WebGpuExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{8}};
  detector.capacities["WebGpuExecutionProvider"] = {{8}};
  auto catalog = CreateCatalog({{url, std::nullopt}}, false, &detector);
  EXPECT_EQ(catalog->GetModel("model"), nullptr);

  detector.capacities["WebGpuExecutionProvider"] = {{16}};
  catalog->InvalidateCache();
  EXPECT_NE(catalog->GetModel("model"), nullptr);
}

TEST_F(AzureModelCatalogTest, MultimodalGenericGpuCapacityUsesDefaultWebGpuRoute) {
  const std::string url = "https://catalog.test";
  auto gpu = MakeModelInfo("model-generic-gpu:1", "model-generic-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "WebGpuExecutionProvider";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  gpu.string_properties[FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR] = " text, IMAGE ";
  auto cpu = MakeModelInfo("model-cpu:1", "model-cpu", 1, "model", "Azure");
  cpu.device_type = DeviceType::kCPU;
  cpu.execution_provider = "CPUExecutionProvider";
  AddBehavior(url)->all_models = {gpu, cpu};
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider", "WebGpuExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{24}};
  detector.capacities["WebGpuExecutionProvider"] = {{8}};
  auto catalog = CreateCatalog({{url, std::nullopt}}, false, &detector);
  auto* alias = catalog->GetModel("model");
  ASSERT_NE(alias, nullptr);
  EXPECT_EQ(alias->Id(), cpu.model_id);
  EXPECT_EQ(alias->Variants().size(), 1u);

  detector.capacities["WebGpuExecutionProvider"] = {{16}};
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), alias);
  EXPECT_EQ(alias->Id(), gpu.model_id);
  EXPECT_EQ(alias->Variants().size(), 2u);

  detector.devices["GPU"] = {"CUDAExecutionProvider"};
  detector.capacities.erase("WebGpuExecutionProvider");
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), alias);
  EXPECT_EQ(alias->Id(), cpu.model_id);
}

TEST_F(AzureModelCatalogTest, CachedGenericGpuUsesMultimodalConfigWithoutCatalogModalities) {
  const std::string url = "https://catalog.test";
  auto gpu = MakeModelInfo("model-generic-gpu:1", "model-generic-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "WebGpuExecutionProvider";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  const auto path = AddLocalModel(gpu.model_id, "model-generic-gpu-1");
  std::ofstream(path / "genai_config.json") << R"({
    "model": {
      "type": "phi3v",
      "decoder": {"session_options": {"provider_options": [{"webgpu": {}}]}}
    }
  })";
  AddBehavior(url)->all_models = {gpu};
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider", "WebGpuExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{24}};
  detector.capacities["WebGpuExecutionProvider"] = {{8}};
  auto catalog = CreateCatalog({{url, std::nullopt}}, false, &detector);
  EXPECT_EQ(catalog->GetModel("model"), nullptr);
  EXPECT_NE(catalog->GetModelVariant(gpu.model_id), nullptr);
}

TEST_F(AzureModelCatalogTest, HostedCapacityUsesOnlyHostTargetsAndNamesOnlyHostRemainsUnknown) {
  const std::string url = "https://catalog.test";
  auto gpu = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  gpu.device_type = DeviceType::kGPU;
  gpu.execution_provider = "CUDAExecutionProvider";
  gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  auto behavior = AddBehavior(url);
  behavior->all_models = {gpu};
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{64}};
  auto catalog = CreateCatalog({{url, std::nullopt}}, false, &detector);
  const std::map<std::string, std::vector<std::string>> names{{"GPU", {"CUDAExecutionProvider"}}};
  const std::vector<ExecutionTarget> small{{"CUDAExecutionProvider", 7, "GPU", {8}}};
  const std::vector<ExecutionTarget> large{{"CUDAExecutionProvider", 7, "GPU", {24}}};
  catalog->SetRuntimeDevices(names, small);
  EXPECT_EQ(catalog->GetModel("model"), nullptr);
  catalog->SetRuntimeDevices(names, large);
  EXPECT_NE(catalog->GetModel("model"), nullptr);
  const auto fetches = behavior->fetch_all_calls;
  catalog->SetRuntimeDevices(names, large);
  EXPECT_NE(catalog->GetModel("model"), nullptr);
  EXPECT_EQ(behavior->fetch_all_calls, fetches);

  detector.capacities["CUDAExecutionProvider"] = {{8}};
  catalog->SetRuntimeDevices(names, large);
  EXPECT_NE(catalog->GetModel("model"), nullptr);
  catalog->SetRuntimeDevices(names, small);
  EXPECT_EQ(catalog->GetModel("model"), nullptr);
  catalog->SetRuntimeDevices(names);
  EXPECT_NE(catalog->GetModel("model"), nullptr);
  EXPECT_EQ(detector.calls, 0);
  EXPECT_EQ(detector.capacity_calls, 0);
}

TEST_F(AzureModelCatalogTest, UnavailableCachedEpRemainsVisibleWithoutOverridingCompatibleDefault) {
  const std::string url = "https://catalog.test";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDAExecutionProvider";
  auto webgpu = MakeModelInfo("model-generic-gpu:1", "model-generic-gpu", 1, "model", "Azure");
  webgpu.execution_provider = "WebGPUExecutionProvider";
  auto behavior = AddBehavior(url);
  behavior->all_models = {webgpu};
  behavior->models_by_id = {cuda};
  const auto cuda_path = AddLocalModel(cuda.model_id, "cuda");
  auto catalog = CreateCatalog({{url, std::nullopt}});
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"WebGpuExecutionProvider"}}});

  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), webgpu.model_id);
  EXPECT_FALSE(model->IsCached());
  auto* cached_variant = FindVariant(catalog->ListModels(), cuda.model_id);
  ASSERT_NE(cached_variant, nullptr);
  EXPECT_TRUE(cached_variant->IsCached());
  EXPECT_EQ(cached_variant->LocalPath(), cuda_path.string());
  EXPECT_EQ(FindVariant(catalog->GetCachedModels(), cuda.model_id), cached_variant);
  EXPECT_TRUE(fs::exists(cuda_path / "genai_config.json"));
  EXPECT_TRUE(std::any_of(catalog_logger_.messages.begin(), catalog_logger_.messages.end(), [&](const auto& entry) {
    return entry.first == LogLevel::Information && entry.second.find("skipping variant '" + cuda.model_id + "'") != std::string::npos &&
           entry.second.find("CUDAExecutionProvider") != std::string::npos;
  }));
  EXPECT_TRUE(std::any_of(catalog_logger_.messages.begin(), catalog_logger_.messages.end(), [&](const auto& entry) {
    return entry.first == LogLevel::Information &&
           entry.second.find("selected default variant '" + webgpu.model_id + "'") != std::string::npos;
  }));

  CatalogCache persisted_cache(cache_directory_.string(), services_.logger);
  persisted_cache.Load();
  const auto persisted_models = persisted_cache.GetCachedModels();
  ASSERT_TRUE(persisted_models.has_value());
  EXPECT_EQ(persisted_models->size(), 2u);

  auto* explicit_variant = catalog->GetModelVariant(cuda.model_id);
  ASSERT_NE(explicit_variant, nullptr);
  EXPECT_EQ(explicit_variant->LocalPath(), cuda_path.string());
  EXPECT_THROW(explicit_variant->Load(), std::exception);
  EXPECT_EQ(model->Id(), webgpu.model_id);
  model->SelectVariant(*explicit_variant);
  EXPECT_EQ(model->Id(), cuda.model_id);
}

TEST_F(AzureModelCatalogTest, SnapshotFallbackPreservesUnavailableCachedVariantsButSelectsCompatibleDefault) {
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDAExecutionProvider";
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "CPUExecutionProvider";
  auto unavailable = cuda;
  unavailable.model_id = "unavailable-cuda-gpu:1";
  unavailable.name = "unavailable-cuda-gpu";
  unavailable.alias = "unavailable";
  WriteSnapshot({cuda, cpu, unavailable});
  AddLocalModel(cuda.model_id, "cuda");
  AddLocalModel(cpu.model_id, "cpu");
  AddLocalModel(unavailable.model_id, "unavailable");
  AddBehavior("https://catalog.test", true);

  for (bool cache_only : {false, true}) {
    SCOPED_TRACE(cache_only);
    auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, cache_only);
    auto* model = catalog->GetModel("model");
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->Id(), cpu.model_id);
    EXPECT_TRUE(model->IsCached());
    EXPECT_EQ(catalog->GetModel("unavailable"), nullptr);
    EXPECT_NE(FindVariant(catalog->ListModels(), unavailable.model_id), nullptr);
    EXPECT_NE(FindVariant(catalog->GetCachedModels(), unavailable.model_id), nullptr);
    EXPECT_NE(FindVariant(catalog->GetCachedModels(), cuda.model_id), nullptr);
    EXPECT_NE(catalog->GetModelVariant(unavailable.model_id), nullptr);
  }

  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), cuda.model_id);
  EXPECT_TRUE(model->IsCached());
}

TEST_F(AzureModelCatalogTest, MissingProviderUsesCachedConfigBeforeModelIdFallback) {
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  auto gpu = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  auto configured = MakeModelInfo("configured:1", "configured", 1, "configured", "Azure");
  WriteSnapshot({cpu, gpu, configured});
  const auto gpu_path = AddLocalModel(gpu.model_id, "gpu");
  const auto configured_path = AddLocalModel(configured.model_id, "configured");
  const auto write_provider = [](const fs::path& path, const std::string& provider) {
    nlohmann::json contents;
    contents["model"]["decoder"]["session_options"]["provider_options"] =
        nlohmann::json::array({{{provider, nlohmann::json::object()}}});
    std::ofstream config(path / "genai_config.json");
    config << contents;
  };
  write_provider(configured_path, "CUDA");

  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), cpu.model_id);
  EXPECT_EQ(catalog->GetModel("configured"), nullptr);
  EXPECT_NE(catalog->GetModelVariant(configured.model_id), nullptr);
  EXPECT_NE(FindVariant(catalog->GetCachedModels(), gpu.model_id), nullptr);

  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(model->Id(), gpu.model_id);
  EXPECT_NE(catalog->GetModel("configured"), nullptr);

  // An explicit CPU config takes precedence over the CUDA hint in the ID, just as in the loader.
  write_provider(gpu_path, "cpu");
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}});
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(model->Id(), gpu.model_id);
}

TEST_F(AzureModelCatalogTest, ProviderAliasesMatchCanonicalRegistrations) {
  for (const auto& [provider, registration] :
       std::vector<std::pair<std::string, std::string>>{{"cpu", "CPUExecutionProvider"},
                                                        {"CUDA", "CUDAExecutionProvider"},
                                                        {"dml", "DmlExecutionProvider"},
                                                        {"WebGPU", "WebGpuExecutionProvider"},
                                                        {"openvino", "OpenVINOExecutionProvider"},
                                                        {"qnn", "QNNExecutionProvider"}}) {
    SCOPED_TRACE(provider);
    auto info = MakeModelInfo("model:1", "model", 1, "model", "Azure");
    info.execution_provider = provider;
    WriteSnapshot({info});
    auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
    catalog->SetRuntimeDevices({{"device", {registration}}});
    EXPECT_NE(catalog->GetModel("model"), nullptr);
  }
}

TEST_F(AzureModelCatalogTest, GenericGpuSnapshotCanUseAvailableAlternativeRegardlessOfMetadata) {
  auto gpu = MakeModelInfo("model-generic-gpu:1", "model-generic-gpu", 1, "model", "Azure");
  AddLocalModel(gpu.model_id, "gpu");

  for (const auto* metadata : {"DmlExecutionProvider", "WebGPUExecutionProvider", "CUDAExecutionProvider", ""}) {
    SCOPED_TRACE(metadata);
    gpu.execution_provider = metadata;
    WriteSnapshot({gpu});
    for (const auto* provider : {"CUDAExecutionProvider", "WebGpuExecutionProvider"}) {
      SCOPED_TRACE(provider);
      auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
      catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {provider}}});
      auto* model = catalog->GetModel("model");
      ASSERT_NE(model, nullptr);
      EXPECT_EQ(model->Id(), gpu.model_id);
      EXPECT_TRUE(model->IsCached());
    }
  }

  gpu.execution_provider = "DmlExecutionProvider";
  WriteSnapshot({gpu});
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"DmlExecutionProvider"}}});
  EXPECT_NE(catalog->GetModel("model"), nullptr);
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}});
  EXPECT_EQ(catalog->GetModel("model"), nullptr);
  EXPECT_NE(FindVariant(catalog->GetCachedModels(), gpu.model_id), nullptr);
  EXPECT_TRUE(std::any_of(catalog_logger_.messages.begin(), catalog_logger_.messages.end(), [](const auto& entry) {
    return entry.first == LogLevel::Information &&
           entry.second.find("no variant for alias 'model' has an available execution provider") != std::string::npos;
  }));
}

TEST_F(AzureModelCatalogTest, ProviderChangesRefreshImplicitDefaultsWithoutReplacingExplicitSelection) {
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDAExecutionProvider";
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "CPUExecutionProvider";
  WriteSnapshot({cuda, cpu});
  AddLocalModel(cuda.model_id, "cuda");
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), cpu.model_id);

  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(model->Id(), cuda.model_id);

  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}});
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(model->Id(), cpu.model_id);

  auto* explicit_variant = catalog->GetModelVariant(cuda.model_id);
  ASSERT_NE(explicit_variant, nullptr);
  model->SelectVariant(*explicit_variant);
  catalog->InvalidateCache();
  catalog->ListModels();
  EXPECT_EQ(model->Id(), cuda.model_id);
  EXPECT_TRUE(explicit_variant->IsCached());
}

TEST_F(AzureModelCatalogTest, RetainedHandleRefreshesBeforeDownloadAndExecutionCapture) {
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "cpu";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  WriteSnapshot({cpu, cuda});
  AddLocalModel(cpu.model_id, "cpu");
  AddLocalModel(cuda.model_id, "cuda");
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), cpu.model_id);

  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
  float progress = 0;
  model->Download([&](float percent) {
    progress = percent;
    return 0;
  });
  EXPECT_EQ(progress, 100);
  EXPECT_EQ(model->Id(), cuda.model_id);

  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}});
  EXPECT_EQ(model->CaptureExecutionModel().Id(), cpu.model_id);
  EXPECT_EQ(model->Id(), cpu.model_id);
}

TEST_F(AzureModelCatalogTest, RetainedHandleRejectsImplicitDownloadWhenNoProviderRemains) {
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  WriteSnapshot({cuda});
  AddLocalModel(cuda.model_id, "cuda");
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  catalog->SetRuntimeDevices({{"GPU", {"CUDAExecutionProvider"}}});
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);

  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}});
  EXPECT_THROW(model->Download(), std::exception);
  auto* variant = model->Variants().front();
  model->SelectVariant(*variant);
  EXPECT_NO_THROW(model->Download());
  EXPECT_EQ(catalog->GetModel("model"), nullptr);

  catalog->SetRuntimeDevices({{"GPU", {"CUDAExecutionProvider"}}});
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(model->Id(), cuda.model_id);
}

TEST_F(AzureModelCatalogTest, ProviderRefreshPreservesLoadedArtifactUntilItIsUnloaded) {
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "cpu";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  WriteSnapshot({cpu, cuda});
  auto package = fl::test::TempPath::CreateTempDir("fl_completed_model_");
  const auto path = package.path() / "tiny-paged-attention";
  fs::copy(fl::test::GetTestDataPath("tiny-paged-attention"), path, fs::copy_options::recursive);
  std::ofstream(path / "inference_model.json") << nlohmann::json({{"Name", cpu.model_id}}).dump();
  ASSERT_TRUE(fs::exists(path / "genai_config.json"));
  for (int release = 0; release < 3; ++release) {
    SCOPED_TRACE(release);
    AzureModelCatalog catalog(
        {{"https://catalog.test", std::nullopt}}, cache_directory_.string(),
        [&](ModelInfo info, std::string) {
          return Model::FromModelInfo(std::move(info), path.string(), services_.download_manager,
                                      services_.model_load_manager);
        },
        services_.ep_detector, catalog_logger_, true, telemetry_);
    catalog.SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
    auto* model = catalog.GetModel("model");
    auto* variant = catalog.GetModelVariant(cuda.model_id);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(variant, nullptr);
    auto captured = variant->CaptureExecutionModel();
    model->Load(ExecutionProvider::kCPU);
    ASSERT_TRUE(model->IsLoaded());
    ASSERT_EQ(model->Id(), cuda.model_id);

    catalog.SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}});
    EXPECT_EQ(catalog.GetModel("model"), model);
    EXPECT_EQ(model->Id(), cuda.model_id);
    EXPECT_TRUE(model->IsLoaded());
    (release == 0 ? model : release == 1 ? variant
                                         : &captured)
        ->Unload();
    EXPECT_EQ(services_.model_load_manager.GetLoadedModel(cuda.model_id), nullptr);

    if (release == 0) {
      model->Load(ExecutionProvider::kCPU);
      EXPECT_TRUE(model->IsLoaded());
      model->Unload();
    } else {
      model->Download();
    }
    EXPECT_EQ(model->Id(), cpu.model_id);
  }
}

TEST_F(AzureModelCatalogTest, VisibilitySharesDeviceSnapshotsAcrossAliasesPerPass) {
  const std::string url = "https://catalog.test";
  auto behavior = AddBehavior(url);
  for (const auto& alias : {"first", "second", "third"}) {
    auto gpu = MakeModelInfo(std::string(alias) + "-cuda-gpu:1", alias, 1, alias, "Azure");
    gpu.device_type = DeviceType::kGPU;
    gpu.execution_provider = "CUDAExecutionProvider";
    gpu.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
    behavior->all_models.push_back(std::move(gpu));
  }
  CountingCatalogEpDetector detector;
  detector.devices["GPU"] = {"CUDAExecutionProvider"};
  detector.capacities["CUDAExecutionProvider"] = {{8}};
  auto catalog = CreateCatalog({{url, std::nullopt}}, false, &detector);

  EXPECT_TRUE(catalog->ListModels().empty());
  EXPECT_EQ(detector.calls, 2);
  EXPECT_EQ(detector.capacity_calls, 2);
  EXPECT_EQ(catalog->GetModel("first"), nullptr);
  EXPECT_EQ(detector.calls, 2);
  EXPECT_EQ(detector.capacity_calls, 2);

  detector.capacities["CUDAExecutionProvider"] = {{16}};
  catalog->InvalidateCache();
  EXPECT_NE(catalog->GetModel("first"), nullptr);
  EXPECT_EQ(detector.calls, 4);
  EXPECT_EQ(detector.capacity_calls, 4);
  EXPECT_EQ(catalog->ListModels().size(), 3u);
  EXPECT_EQ(detector.calls, 5);
  EXPECT_EQ(detector.capacity_calls, 5);
}

TEST_F(AzureModelCatalogTest, EligibilityUsesOneProviderSnapshotAndLookupReusesTheResult) {
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "cpu";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  auto unavailable = cuda;
  unavailable.model_id = "unavailable-cuda-gpu:1";
  unavailable.name = "unavailable-cuda-gpu";
  unavailable.alias = "unavailable";
  WriteSnapshot({cpu, cuda, unavailable});
  CountingCatalogEpDetector detector;
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true, &detector);
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), cpu.model_id);
  EXPECT_EQ(catalog->GetModel("unavailable"), nullptr);
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(detector.calls, 1);

  detector.devices["GPU"] = {"CUDAExecutionProvider"};
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(model->Id(), cuda.model_id);
  EXPECT_NE(catalog->GetModel("unavailable"), nullptr);
  EXPECT_EQ(detector.calls, 2);
}

TEST(AzureModelCatalogRuntimeTest, ReconciliationPreservesLoadedImplicitSelectionWithoutMakingItExplicit) {
  fl::test::FakeServiceBindings bindings;
  const auto path = fl::test::GetTestDataPath("tiny-paged-attention").string();
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  auto make_model = [&](const ModelInfo& info) {
    return Model::FromModelInfo(info, path, bindings.download_manager, bindings.model_load_manager);
  };
  auto model = Model::MakeContainer(make_model(cpu));
  model.Load();
  auto incoming = Model::MakeContainer(make_model(cpu));
  incoming.AddVariant(make_model(cuda));

  ASSERT_TRUE(model.TryReconcileVariants(incoming));
  EXPECT_EQ(model.Id(), cpu.model_id);
  EXPECT_TRUE(model.IsLoaded());
  model.Unload();
  model.SelectDefaultVariant({}, /*preserve_explicit_selection=*/true);
  EXPECT_EQ(model.Id(), cuda.model_id);
}

TEST_F(AzureModelCatalogTest, HostedLoadPinPreservesMetadataWithoutLoadingWeightsLocally) {
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "cpu";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  WriteSnapshot({cpu, cuda});
  AddLocalModel(cpu.model_id, "cpu");
  AddLocalModel(cuda.model_id, "cuda");
  for (int release = 0; release < 3; ++release) {
    SCOPED_TRACE(release);
    auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
    catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
    auto* model = catalog->GetModel("model");
    auto* variant = catalog->GetModelVariant(cuda.model_id);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(variant, nullptr);
    auto captured = variant->CaptureExecutionModel();
    EXPECT_FALSE(model->PreserveLoadedSelection(true));
    EXPECT_FALSE(model->IsLoaded());

    catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}});
    model->Download();
    EXPECT_EQ(model->Id(), cuda.model_id);
    EXPECT_TRUE((release == 0 ? model : release == 1 ? variant
                                                     : &captured)
                    ->PreserveLoadedSelection(false));
    model->Download();
    EXPECT_EQ(model->Id(), cpu.model_id);
  }
}

TEST_F(AzureModelCatalogTest, ProviderRefreshDuringDownloadKeepsTheArtifactThatBecomesCached) {
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "CPU";
  cpu.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Microsoft";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  WriteSnapshot({cpu, cuda});
  services_.download_manager.SetCacheDirectory(cache_directory_.string());
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  ASSERT_FALSE(model->IsCached());
  ASSERT_EQ(model->Id(), cpu.model_id);

  const auto path = AddLocalModel(cpu.model_id, "model-generic-cpu-1");
  fs::copy(test::GetTestDataPath("tiny-paged-attention"), path,
           fs::copy_options::recursive | fs::copy_options::overwrite_existing);
  std::ofstream(path / "inference_model.json") << nlohmann::json({{"Name", cpu.model_id}}).dump();
  ASSERT_EQ(services_.download_manager.GetCompletedModelCachePath(cpu), path.string());
  bool refreshed = false;
  model->Download([&](float) {
    catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
    EXPECT_EQ(catalog->GetModel("model"), model);
    EXPECT_EQ(model->Id(), cpu.model_id);
    refreshed = true;
    return 0;
  });
  EXPECT_TRUE(refreshed);
  EXPECT_NO_THROW(model->Load(ExecutionProvider::kCPU));
  EXPECT_EQ(model->Id(), cpu.model_id);
  EXPECT_TRUE(model->IsLoaded());
  model->Unload();
}

TEST_F(AzureModelCatalogTest, ConfigEligibilityIsReusedUntilRefreshAndThenSeesEdits) {
  auto configured = MakeModelInfo("configured:1", "configured", 1, "configured", "Azure");
  WriteSnapshot({configured});
  const auto path = AddLocalModel(configured.model_id, "configured");
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  auto* model = catalog->GetModel("configured");
  ASSERT_NE(model, nullptr);
  {
    std::ofstream config(path / "genai_config.json");
    config << "{";
  }

  EXPECT_EQ(catalog->GetModel("configured"), model);
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("configured"), nullptr);
  {
    std::ofstream config(path / "genai_config.json");
    config << "{}";
  }

  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("configured"), model);
}

TEST_F(AzureModelCatalogTest, ProviderRefreshAddsNewCachedVariantsToExistingAliasWithoutReplacingHandles) {
  const std::string url = "https://catalog.test";
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "cpu";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  auto behavior = AddBehavior(url);
  behavior->all_models = {cpu};
  auto catalog = CreateCatalog({{url, std::nullopt}});
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  auto* cpu_variant = catalog->GetModelVariant(cpu.model_id);
  ASSERT_NE(cpu_variant, nullptr);

  behavior->all_models = {cpu, cuda};
  AddLocalModel(cuda.model_id, "cuda");
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(catalog->GetModelVariant(cpu.model_id), cpu_variant);
  EXPECT_EQ(model->Variants().size(), 2u);
  EXPECT_EQ(model->Id(), cuda.model_id);
  EXPECT_NE(FindVariant(catalog->GetCachedModels(), cuda.model_id), nullptr);
}

TEST_F(AzureModelCatalogTest, RefreshUpdatesAnExistingVariantWhenAnotherProcessCachesIt) {
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "CPU";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  WriteSnapshot({cpu, cuda});
  AddLocalModel(cpu.model_id, "cpu");
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
  auto* model = catalog->GetModel("model");
  auto* cuda_variant = catalog->GetModelVariant(cuda.model_id);
  ASSERT_NE(model, nullptr);
  ASSERT_NE(cuda_variant, nullptr);
  EXPECT_EQ(model->Id(), cpu.model_id);
  EXPECT_FALSE(cuda_variant->IsCached());

  const auto path = AddLocalModel(cuda.model_id, "cuda");
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(catalog->GetModelVariant(cuda.model_id), cuda_variant);
  EXPECT_EQ(model->Id(), cuda.model_id);
  EXPECT_TRUE(cuda_variant->IsCached());
  EXPECT_EQ(cuda_variant->LocalPathSnapshot(), path.string());
}

TEST_F(AzureModelCatalogTest, RefreshUpdatesExistingCachePathsAndForgetsRemovedEntries) {
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "CPU";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  WriteSnapshot({cpu, cuda});
  AddLocalModel(cpu.model_id, "cpu");
  const auto original_path = AddLocalModel(cuda.model_id, "cuda");
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}}, true);
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
  auto* model = catalog->GetModel("model");
  auto* cuda_variant = catalog->GetModelVariant(cuda.model_id);
  ASSERT_NE(model, nullptr);
  ASSERT_NE(cuda_variant, nullptr);
  ASSERT_EQ(model->Id(), cuda.model_id);
  const auto* borrowed_original = cuda_variant->GetPath().c_str();
  catalog->InvalidateCache();
  catalog->ListModels();
  EXPECT_EQ(cuda_variant->GetPath().c_str(), borrowed_original);

  const auto moved_path = original_path.parent_path() / "moved-cuda";
  fs::rename(original_path, moved_path);
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(catalog->GetModelVariant(cuda.model_id), cuda_variant);
  EXPECT_EQ(cuda_variant->LocalPathSnapshot(), moved_path.string());
  EXPECT_TRUE(cuda_variant->IsCached());
  EXPECT_EQ(model->Id(), cuda.model_id);
  EXPECT_STREQ(borrowed_original, original_path.string().c_str());
  const auto* borrowed_moved = cuda_variant->GetPath().c_str();

  fs::remove(moved_path / "genai_config.json");
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(catalog->GetModelVariant(cuda.model_id), cuda_variant);
  EXPECT_FALSE(cuda_variant->IsCached());
  EXPECT_TRUE(cuda_variant->LocalPathSnapshot().empty());
  EXPECT_EQ(model->Id(), cpu.model_id);
  EXPECT_EQ(FindVariant(catalog->GetCachedModels(), cuda.model_id), nullptr);
  EXPECT_STREQ(borrowed_original, original_path.string().c_str());
  EXPECT_STREQ(borrowed_moved, moved_path.string().c_str());
}

TEST_F(AzureModelCatalogTest, RefreshReconcilesOlderIdsMissingFromLiveMetadataAgainstTheDiskInventory) {
  const std::string url = "https://catalog.test";
  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "CPU";
  auto old_cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  old_cuda.execution_provider = "CUDA";
  auto new_cuda = old_cuda;
  new_cuda.model_id = "model-cuda-gpu:2";
  new_cuda.version = 2;
  auto behavior = AddBehavior(url);
  behavior->all_models = {cpu, old_cuda};
  AddLocalModel(cpu.model_id, "cpu");
  const auto old_path = AddLocalModel(old_cuda.model_id, "old-cuda");
  auto catalog = CreateCatalog({{url, std::nullopt}});
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider"}}});
  auto* model = catalog->GetModel("model");
  auto* old_variant = catalog->GetModelVariant(old_cuda.model_id);
  ASSERT_NE(model, nullptr);
  ASSERT_NE(old_variant, nullptr);
  ASSERT_EQ(model->Id(), old_cuda.model_id);

  behavior->all_models = {cpu, new_cuda};
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_TRUE(old_variant->IsCached());
  EXPECT_EQ(model->Id(), old_cuda.model_id);
  EXPECT_EQ(behavior->last_requested_ids, std::vector<std::string>{old_cuda.model_id});

  fs::remove(old_path / "genai_config.json");
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("model"), model);
  EXPECT_EQ(catalog->GetModelVariant(old_cuda.model_id), old_variant);
  EXPECT_FALSE(old_variant->IsCached());
  EXPECT_TRUE(old_variant->GetPath().empty());
  EXPECT_EQ(model->Id(), cpu.model_id);
  EXPECT_EQ(FindVariant(catalog->GetCachedModels(), old_cuda.model_id), nullptr);
}

TEST_F(AzureModelCatalogTest, RefreshForgetsRemovedCacheForAnEntirelyOmittedAliasWithoutDroppingItsHandles) {
  const std::string url = "https://catalog.test";
  auto info = MakeModelInfo("old-generic-cpu:1", "old-generic-cpu", 1, "old", "Azure");
  auto behavior = AddBehavior(url);
  behavior->all_models = {info};
  const auto path = AddLocalModel(info.model_id, "old");
  auto catalog = CreateCatalog({{url, std::nullopt}});
  auto* model = catalog->GetModel("old");
  auto* variant = catalog->GetModelVariant(info.model_id);
  ASSERT_NE(model, nullptr);
  ASSERT_NE(variant, nullptr);
  ASSERT_TRUE(variant->IsCached());

  behavior->all_models.clear();
  fs::remove(path / "genai_config.json");
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->GetModel("old"), model);
  EXPECT_EQ(catalog->GetModelVariant(info.model_id), variant);
  EXPECT_FALSE(variant->IsCached());
  EXPECT_TRUE(catalog->GetCachedModels().empty());
}

TEST_F(AzureModelCatalogTest, DirectIdLookupDoesNotMakeUnavailableAliasEligible) {
  const std::string url = "https://catalog.test";
  auto cuda = MakeModelInfo("model-cuda-gpu:1", "model-cuda-gpu", 1, "model", "Azure");
  cuda.execution_provider = "CUDA";
  auto behavior = AddBehavior(url);
  behavior->models_by_id = {cuda};
  auto catalog = CreateCatalog({{url, std::nullopt}});
  EXPECT_EQ(catalog->GetModel("model"), nullptr);
  EXPECT_NE(catalog->GetModelVariant(cuda.model_id), nullptr);
  EXPECT_EQ(catalog->GetModel("model"), nullptr);
  EXPECT_NE(FindVariant(catalog->ListModels(), cuda.model_id), nullptr);

  auto cpu = MakeModelInfo("model-generic-cpu:1", "model-generic-cpu", 1, "model", "Azure");
  cpu.execution_provider = "cpu";
  behavior->models_by_id.push_back(cpu);
  EXPECT_NE(catalog->GetModelVariant(cpu.model_id), nullptr);
  auto* model = catalog->GetModel("model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), cpu.model_id);
}

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

TEST_F(AzureModelCatalogTest, CacheOnlyFiltersSnapshotByMinFlVersion) {
  WriteSnapshot(MakeMinVersionModelInfos());
  for (const auto* name : {"compatible", "future", "missing", "malformed"}) {
    AddLocalModel(std::string(name) + ":1", name);
  }
  auto catalog = CreateCatalog({{"https://must-not-be-called.test", std::nullopt}}, true);

  const auto models = catalog->GetCachedModels();

  ExpectCompatibleMinVersionModels(models);
  EXPECT_EQ(factory_calls_, 0);
}

TEST_F(AzureModelCatalogTest, NetworkFailureFiltersSnapshotByMinFlVersion) {
  WriteSnapshot(MakeMinVersionModelInfos());
  for (const auto* name : {"compatible", "future", "missing", "malformed"}) {
    AddLocalModel(std::string(name) + ":1", name);
  }
  AddBehavior("https://catalog.test", true);
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}});

  const auto models = catalog->GetCachedModels();

  ExpectCompatibleMinVersionModels(models);
  EXPECT_EQ(factory_calls_, 1);
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

TEST_F(AzureModelCatalogTest, SharedCompatibilityFiltersLiveListingAndVersionHistoryBeforeLimits) {
  const auto behavior = AddBehavior("https://catalog.test");
  auto compatible = MakeModelInfo("model:1", "model", 1, "test-alias", "TestProvider");
  auto future = MakeModelInfo("model:2", "model", 2, "test-alias", "TestProvider");
  future.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR] = "999.0.0";
  behavior->all_models = {compatible, future};
  behavior->all_versions = {compatible, future};
  auto catalog = CreateCatalog({{"https://catalog.test", std::nullopt}});

  const auto models = catalog->ListModels();
  EXPECT_NE(FindVariant(models, "model:1"), nullptr);
  EXPECT_EQ(FindVariant(models, "model:2"), nullptr);

  const auto versions = catalog->GetModelVersions("test-alias", "", 1);
  ASSERT_EQ(versions.size(), 1u);
  EXPECT_NE(FindVariant(versions, "model:1"), nullptr);
  EXPECT_EQ(FindVariant(versions, "model:2"), nullptr);

  CatalogCache persisted_cache(cache_directory_.string(), services_.logger);
  persisted_cache.Load();
  const auto persisted = persisted_cache.GetCachedModels();
  ASSERT_TRUE(persisted.has_value());
  ASSERT_EQ(persisted->size(), 1u);
  EXPECT_EQ(persisted->front().model_id, "model:1");
}

TEST_F(AzureModelCatalogTest, IncompatibleExactResultDoesNotPreventTryingNextEndpoint) {
  auto compatible = MakeModelInfo("model:1", "model", 1, "test-alias", "TestProvider");
  auto future = compatible;
  future.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR] = "999.0.0";
  const auto first = AddBehavior("https://catalog-one.test");
  first->models_by_id = {future};
  const auto second = AddBehavior("https://catalog-two.test");
  second->models_by_id = {compatible};
  auto catalog = CreateCatalog({
      {"https://catalog-one.test", std::nullopt},
      {"https://catalog-two.test", std::nullopt},
  });

  const auto* model = catalog->GetModelVariant("model:1");

  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().model_id, "model:1");
  EXPECT_EQ(first->fetch_by_id_calls, 1);
  EXPECT_EQ(second->fetch_by_id_calls, 1);
}

TEST_F(AzureModelCatalogTest, VersionHistoryDeduplicatesBeforeApplyingLimit) {
  const std::string first_url = "https://catalog-one.test";
  const std::string second_url = "https://catalog-two.test";
  auto shared_first = MakeModelInfo("shared:3", "variant", 3, "test-alias", "FirstProvider");
  auto shared_second = MakeModelInfo("shared:3", "variant", 3, "test-alias", "SecondProvider");
  auto older = MakeModelInfo("older:2", "variant", 2, "test-alias", "SecondProvider");

  const auto first_behavior = AddBehavior(first_url);
  first_behavior->all_versions = {shared_first};
  const auto second_behavior = AddBehavior(second_url);
  second_behavior->all_versions = {shared_second, older};
  auto catalog = CreateCatalog({{first_url, std::nullopt}, {second_url, std::nullopt}});

  const auto versions = catalog->GetModelVersions("test-alias", "", 2);

  ASSERT_EQ(versions.size(), 2u);
  ASSERT_NE(FindVariant(versions, "shared:3"), nullptr);
  EXPECT_NE(FindVariant(versions, "older:2"), nullptr);
  const auto* provider = FindVariant(versions, "shared:3")
                             ->Info()
                             .GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR);
  ASSERT_NE(provider, nullptr);
  EXPECT_EQ(*provider, "FirstProvider");
  EXPECT_EQ(first_behavior->fetch_all_versions_calls, 1);
  EXPECT_EQ(second_behavior->fetch_all_versions_calls, 1);
  EXPECT_EQ(first_behavior->requested_max_versions, std::vector<int>{2});
  EXPECT_EQ(second_behavior->requested_max_versions, std::vector<int>{2});
}

TEST_F(AzureModelCatalogTest, VersionHistoryLimitsBeforeCreatingModelHandles) {
  const std::string url = "https://catalog.test";
  const auto behavior = AddBehavior(url);
  behavior->all_versions = {
      MakeModelInfo("text:1", "text", 1, "test-alias", "TestProvider"),
      MakeModelInfo("vision:1", "vision", 1, "test-alias", "TestProvider"),
      MakeModelInfo("text:3", "text", 3, "test-alias", "TestProvider"),
      MakeModelInfo("vision:2", "vision", 2, "test-alias", "TestProvider"),
      MakeModelInfo("text:2", "text", 2, "test-alias", "TestProvider"),
  };
  auto catalog = CreateCatalog({{url, std::nullopt}});

  const auto first = catalog->GetModelVersions("test-alias", "", 1);
  const auto second = catalog->GetModelVersions("test-alias", "", 1);

  ASSERT_EQ(first.size(), 2u);
  EXPECT_NE(FindVariant(first, "text:3"), nullptr);
  EXPECT_NE(FindVariant(first, "vision:2"), nullptr);
  ASSERT_EQ(second.size(), 2u);
  EXPECT_EQ(model_handle_factory_calls_, 4);
  EXPECT_EQ(behavior->requested_max_versions, (std::vector<int>{1, 1}));
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
