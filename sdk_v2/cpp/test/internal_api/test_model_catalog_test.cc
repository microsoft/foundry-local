// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/test_model_catalog.h"

#include "catalog/catalog_cache.h"
#include "exception.h"
#include "internal_api/test_helpers.h"
#include "util/sha256.h"
#include "utils/temp_path.h"

#include <azure/core/exception.hpp>
#include <azure/core/http/http.hpp>
#include <azure/core/http/transport.hpp>
#include <azure/core/io/body_stream.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace fl;
using Json = nlohmann::json;
using Azure::Core::Http::HttpMethod;
using Azure::Core::Http::HttpStatusCode;

constexpr auto kRegistry = "mock-registry";
constexpr auto kApi = "?api-version=2021-10-01-dataplanepreview";
const std::string kBase =
    "https://region.experiments.azureml.net/mferp/managementfrontend/subscriptions/sub/resourceGroups/group"
    "/providers/Microsoft.MachineLearningServices/registries/mock-registry";
const std::string kCpuName = "tiny-generic-cpu";

class FakeCredential final : public Azure::Core::Credentials::TokenCredential {
 public:
  FakeCredential() : TokenCredential("mock") {}

  Azure::Core::Credentials::AccessToken GetToken(
      const Azure::Core::Credentials::TokenRequestContext& context,
      const Azure::Core::Context&) const override {
    scopes.insert(context.Scopes.begin(), context.Scopes.end());
    if (fail) throw Azure::Core::Credentials::AuthenticationException("mock authentication failure");
    return {"mock-token", Azure::DateTime(std::chrono::system_clock::now() + std::chrono::hours(1))};
  }

  bool fail = false;
  mutable std::set<std::string> scopes;
};

class MockRegistryTransport final : public Azure::Core::Http::HttpTransport {
 public:
  struct Response {
    std::string url;
    std::string body;
    HttpMethod method;
    HttpStatusCode status;
    std::optional<Json> request_body;
  };

  void Queue(const std::string& url, const Json& body, HttpMethod method = HttpMethod::Get,
             HttpStatusCode status = HttpStatusCode::Ok, std::optional<Json> request_body = std::nullopt) {
    responses.push_back({url, body.dump(), method, status, std::move(request_body)});
  }

  std::unique_ptr<Azure::Core::Http::RawResponse> Send(
      Azure::Core::Http::Request& request, const Azure::Core::Context& context) override {
    if (calls >= responses.size()) throw std::runtime_error("unexpected mocked Azure ML request");
    const auto& expected = responses[calls++];
    EXPECT_EQ(request.GetUrl().GetAbsoluteUrl(), expected.url);
    EXPECT_EQ(request.GetMethod(), expected.method);
    if (expected.request_body) {
      const auto bytes = request.GetBodyStream()->ReadToEnd(context);
      EXPECT_EQ(Json::parse(bytes.begin(), bytes.end()), *expected.request_body);
    }
    auto response = std::make_unique<Azure::Core::Http::RawResponse>(1, 1, expected.status, "mock");
    response->SetHeader("x-ms-request-id", "mock-request-id");
    body_.assign(expected.body.begin(), expected.body.end());
    response->SetBodyStream(std::make_unique<Azure::Core::IO::MemoryBodyStream>(body_));
    return response;
  }

  size_t calls = 0;
  std::vector<Response> responses;

 private:
  std::vector<uint8_t> body_;
};

class FakeCatalogDevices final : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override { return devices; }
  std::map<std::string, std::vector<std::string>> devices{{"CPU", {"CPUExecutionProvider"}}};
};

Json LegacyModel(const std::string& name, int version = 1, const std::string& alias = "tiny") {
  return {{"name", name},
          {"id", "azureml://registries/mock-registry/models/" + name + "/versions/" + std::to_string(version)},
          {"properties", {{"tags", {{"foundryLocal", "true"}, {"alias", alias}, {"task", "chat-completion"}}},
                          {"modelUri", "https://storage.blob.core.windows.net/container/v1"}}}};
}

Json TaggedModel(int version = 1) {
  auto model = LegacyModel(kCpuName, version);
  auto& tags = model["properties"]["tags"];
  tags.erase("foundryLocal");
  tags["device"] = "CPU";
  tags["execution_provider"] = "CPUExecutionProvider";
#if defined(_WIN32)
  tags["os"] = "windows";
#elif defined(__APPLE__)
  tags["os"] = "macos";
#elif defined(__ANDROID__)
  tags["os"] = "android";
#else
  tags["os"] = "linux";
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
  tags["architecture"] = "arm64";
#else
  tags["architecture"] = "x64";
#endif
  return model;
}

std::set<std::string> VariantIds(const std::vector<Model*>& groups) {
  std::set<std::string> ids;
  for (const auto* group : groups) {
    for (const auto* variant : group->Variants()) ids.insert(variant->Id());
  }
  return ids;
}

class TestModelCatalogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.app_name = "MockTestCatalog";
    config_.model_cache_dir = directory_.string();
    config_.additional_options["TestModelCredential"] = "invalid-if-credentials-are-initialized";
  }

  void TearDown() override { EXPECT_EQ(transport_->calls, transport_->responses.size()); }

  void Discovery(const std::string& endpoint = "https://region.experiments.azureml.net/") {
    transport_->Queue("https://eastus.api.azureml.ms/registrymanagement/v1.0/registries/mock-registry/discovery",
                      {{"subscriptionId", "sub"}, {"resourceGroup", "group"},
                       {"primaryRegionResourceProviderUri", endpoint}});
  }

  void Inventory(const std::vector<Json>& models) {
    Discovery();
    Json names = Json::array();
    std::set<std::string> seen;
    for (const auto& model : models) {
      const auto name = model.at("name").get<std::string>();
      if (seen.insert(name).second) names.push_back({{"name", name}});
    }
    transport_->Queue(kBase + "/models" + kApi, {{"value", names}});
    seen.clear();
    for (const auto& model : models) {
      const auto name = model.at("name").get<std::string>();
      if (!seen.insert(name).second) continue;
      Json versions = Json::array();
      for (const auto& candidate : models) {
        if (candidate.at("name") == name) versions.push_back(candidate);
      }
      transport_->Queue(kBase + "/models/" + name + "/versions" + kApi, {{"value", versions}});
    }
  }

  std::unique_ptr<AzureMlRegistryClient> Client() {
    return std::make_unique<AzureMlRegistryClient>(kRegistry, credential_, transport_);
  }

  std::unique_ptr<TestModelCatalog> Catalog(bool inject_client = true) {
    return std::make_unique<TestModelCatalog>(kRegistry, config_, devices_, services_.model_load_manager,
                                              services_.logger, services_.telemetry,
                                              inject_client ? Client() : nullptr);
  }

  std::filesystem::path CachePath() const {
    return directory_.path() / (std::string(kRegistry) + "-" + Sha256String("azureml://registries/mock-registry"));
  }

  ModelInfo CachedInfo(int version) const {
    ModelInfo info;
    info.name = kCpuName;
    info.version = version;
    info.model_id = kCpuName + ":" + std::to_string(version);
    info.alias = "tiny";
    info.device_type = DeviceType::kCPU;
    info.execution_provider = "CPUExecutionProvider";
    info.string_properties[TestModelCatalog::kRegistryProperty] = kRegistry;
    return info;
  }

  void Snapshot(const std::vector<ModelInfo>& models) {
    CatalogCache(CachePath().string(), services_.logger).Save(models, true);
  }

  std::filesystem::path Package(int version, bool complete = true) {
    const auto path = CachePath() / (kCpuName + "-" + std::to_string(version));
    std::filesystem::create_directories(path);
    std::ofstream(path / "genai_config.json") << "{}";
    if (complete) std::ofstream(path / "inference_model.json") << "{}";
    return path;
  }

  fl::test::TempPath directory_ = fl::test::TempPath::CreateTempDir("fl_test_catalog_");
  fl::test::FakeServiceBindings services_;
  FakeCatalogDevices devices_;
  Configuration config_;
  std::shared_ptr<FakeCredential> credential_ = std::make_shared<FakeCredential>();
  std::shared_ptr<MockRegistryTransport> transport_ = std::make_shared<MockRegistryTransport>();
};

TEST_F(TestModelCatalogTest, LatestDiscoveryUsesNumericVersionsPerHardwareVariant) {
  devices_.devices["GPU"] = {"WebGpuExecutionProvider"};
  Inventory({LegacyModel(kCpuName, 2), LegacyModel(kCpuName, 10), LegacyModel(kCpuName, 1),
             LegacyModel("tiny-generic-gpu", 3), LegacyModel("tiny-generic-gpu", 1)});
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()),
            (std::set<std::string>{"tiny-generic-cpu:10", "tiny-generic-gpu:3"}));
  CatalogCache snapshot(CachePath().string(), services_.logger);
  snapshot.Load();
  ASSERT_TRUE(snapshot.GetCachedModels().has_value());
  EXPECT_EQ(snapshot.GetCachedModels()->size(), 5);
}

TEST_F(TestModelCatalogTest, OlderExactIdFetchesOnlyTheRequestedVersion) {
  Inventory({LegacyModel(kCpuName, 10)});
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, LegacyModel(kCpuName, 2));
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{"tiny-generic-cpu:10"}));
  auto* older = catalog->GetModelVariant("tiny-generic-cpu:2");
  ASSERT_NE(older, nullptr);
  EXPECT_EQ(older->Info().version, 2);
}

TEST_F(TestModelCatalogTest, VersionHistoryPreservesOlderVersionsAndAppliesPerNameLimit) {
  Inventory({LegacyModel(kCpuName, 2), LegacyModel(kCpuName, 10), LegacyModel(kCpuName, 1)});
  auto catalog = Catalog();
  auto versions = catalog->GetModelVersions("tiny", kCpuName, 2);
  ASSERT_EQ(versions.size(), 2);
  EXPECT_EQ(versions[0]->Info().version, 10);
  EXPECT_EQ(versions[1]->Info().version, 2);
}

TEST_F(TestModelCatalogTest, StrictTagsMapMetadataAndIgnoreUnrelatedAssets) {
  auto unrelated = LegacyModel("unrelated");
  unrelated["properties"]["tags"] = {{"alias", "other"}, {"task", "chat-completion"}};
  Inventory({TaggedModel(3), unrelated});
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{"tiny-generic-cpu:3"}));
  const auto* model = catalog->GetModelVariant("tiny-generic-cpu:3");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().alias, "tiny");
  EXPECT_EQ(model->Info().execution_provider, "CPUExecutionProvider");
}

TEST_F(TestModelCatalogTest, MissingStrictTagFailsInsteadOfReturningAnEmptyCatalog) {
  auto model = TaggedModel();
  model["properties"]["tags"].erase("alias");
  Inventory({model});
  auto catalog = Catalog();
  EXPECT_THROW(catalog->ListModels(), Exception);
}

TEST_F(TestModelCatalogTest, MalformedTagsAndCrossRegistryAssetIdsFailExplicitly) {
  auto malformed = TaggedModel();
  malformed["properties"]["tags"] = Json::array();
  Inventory({malformed});
  auto catalog = Catalog();
  EXPECT_THROW(catalog->ListModels(), Exception);

  auto wrong_registry = TaggedModel();
  wrong_registry["id"] = "azureml://registries/another-registry/models/" + kCpuName + "/versions/1";
  transport_->Queue(kBase + "/models" + kApi, {{"value", {{{"name", kCpuName}}}}});
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions" + kApi, {{"value", {wrong_registry}}});
  EXPECT_THROW(catalog->ListModels(), Exception);
}

TEST_F(TestModelCatalogTest, IncompatiblePlatformIsExcluded) {
  auto model = TaggedModel();
  model["properties"]["tags"]["architecture"] =
      model["properties"]["tags"]["architecture"] == "arm64" ? "x64" : "arm64";
  Inventory({model});
  auto catalog = Catalog();
  EXPECT_TRUE(catalog->ListModels().empty());
}

TEST_F(TestModelCatalogTest, UnknownBackendAndIncompleteLegacyMetadataAreSkipped) {
  auto missing_alias = LegacyModel("other-generic-cpu");
  missing_alias["properties"]["tags"].erase("alias");
  Inventory({LegacyModel("tiny-mystery-npu"), missing_alias, LegacyModel(kCpuName)});
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{"tiny-generic-cpu:1"}));
}

TEST_F(TestModelCatalogTest, HardwareFilteringMatchesProviderToDeviceAndRefreshesAfterRegistration) {
  devices_.devices["GPU"] = {"OpenVINOExecutionProvider"};
  Inventory({LegacyModel("tiny-openvino-gpu"), LegacyModel("tiny-openvino-npu")});
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{"tiny-openvino-gpu:1"}));
  devices_.devices["NPU"] = {"OpenVINOExecutionProvider"};
  transport_->Queue(kBase + "/models" + kApi,
                    {{"value", {{{"name", "tiny-openvino-gpu"}}, {{"name", "tiny-openvino-npu"}}}}});
  transport_->Queue(kBase + "/models/tiny-openvino-gpu/versions" + kApi,
                    {{"value", {LegacyModel("tiny-openvino-gpu")}}});
  transport_->Queue(kBase + "/models/tiny-openvino-npu/versions" + kApi,
                    {{"value", {LegacyModel("tiny-openvino-npu")}}});
  catalog->InvalidateCache();
  EXPECT_EQ(VariantIds(catalog->ListModels()),
            (std::set<std::string>{"tiny-openvino-gpu:1", "tiny-openvino-npu:1"}));
}

TEST_F(TestModelCatalogTest, UnregisteredProvidersAreExcludedEvenWhenTheDeviceTypeExists) {
  devices_.devices["GPU"] = {"WebGpuExecutionProvider"};
  Inventory({LegacyModel(kCpuName), LegacyModel("tiny-cuda-gpu"), LegacyModel("tiny-qnn-npu"),
             LegacyModel("tiny-generic-gpu")});
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()),
            (std::set<std::string>{"tiny-generic-cpu:1", "tiny-generic-gpu:1"}));
}

struct Backend {
  const char* suffix;
  const char* device;
  const char* provider;
};

class TestModelCatalogBackendTest : public TestModelCatalogTest, public ::testing::WithParamInterface<Backend> {};

TEST_P(TestModelCatalogBackendTest, RecognizesLegacyBackendAndRegisteredDevice) {
  const auto backend = GetParam();
  devices_.devices[backend.device] = {backend.provider};
  const auto name = std::string("tiny-") + backend.suffix;
  Inventory({LegacyModel(name)});
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{name + ":1"}));
  const auto* model = catalog->GetModelVariant(name + ":1");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Info().execution_provider, backend.provider);
}

INSTANTIATE_TEST_SUITE_P(
    AllBackends, TestModelCatalogBackendTest,
    ::testing::Values(Backend{"generic-cpu", "CPU", "CPUExecutionProvider"},
                      Backend{"generic-gpu", "GPU", "DmlExecutionProvider"},
                      Backend{"cuda-gpu", "GPU", "CUDAExecutionProvider"},
                      Backend{"webgpu-gpu", "GPU", "WebGpuExecutionProvider"},
                      Backend{"openvino-gpu", "GPU", "OpenVINOExecutionProvider"},
                      Backend{"openvino-npu", "NPU", "OpenVINOExecutionProvider"},
                      Backend{"qnn-npu", "NPU", "QNNExecutionProvider"},
                      Backend{"trtrtx-gpu", "GPU", "NvTensorRTRTXExecutionProvider"},
                      Backend{"vitis-npu", "NPU", "VitisAIExecutionProvider"},
                      Backend{"ryzenai-npu", "NPU", "RyzenAIExecutionProvider"},
                      Backend{"amdgpu-gpu", "GPU", "AMDGPUExecutionProvider"}));

TEST_F(TestModelCatalogTest, CompletedCacheServesExactAndAliasWithoutCreatingCredentials) {
  Snapshot({CachedInfo(2)});
  const auto path = Package(2);
  auto catalog = Catalog(false);
  auto* exact = catalog->GetModelVariant("tiny-generic-cpu:2");
  ASSERT_NE(exact, nullptr);
  EXPECT_EQ(exact->LocalPathSnapshot(), path.string());
  ASSERT_NE(catalog->GetModel("tiny"), nullptr);
  EXPECT_EQ(catalog->GetModel("tiny")->Id(), "tiny-generic-cpu:2");
  EXPECT_EQ(path.parent_path().parent_path(), directory_.path());
  EXPECT_EQ(path.parent_path().filename().string().find("mock-registry-"), 0);
}

TEST_F(TestModelCatalogTest, IncompleteCacheFallsBackToRegistryForExactId) {
  Snapshot({CachedInfo(2)});
  Package(2, false);
  Discovery();
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, LegacyModel(kCpuName, 2));
  auto catalog = Catalog();
  auto* model = catalog->GetModelVariant("tiny-generic-cpu:2");
  ASSERT_NE(model, nullptr);
  EXPECT_TRUE(model->LocalPathSnapshot().empty());
}

TEST_F(TestModelCatalogTest, AliasCacheMissQueriesRegistryAndGroupsLatestVariant) {
  Inventory({LegacyModel(kCpuName, 1), LegacyModel(kCpuName, 10)});
  auto catalog = Catalog();
  auto* model = catalog->GetModel("tiny");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), "tiny-generic-cpu:10");
}

TEST_F(TestModelCatalogTest, ExactLookupMergesSnapshotAndSurvivesRestartWithoutCredentials) {
  Snapshot({CachedInfo(10)});
  Discovery();
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, LegacyModel(kCpuName, 2));
  auto catalog = Catalog();
  ASSERT_NE(catalog->GetModelVariant("tiny-generic-cpu:2"), nullptr);
  CatalogCache snapshot(CachePath().string(), services_.logger);
  snapshot.Load();
  ASSERT_TRUE(snapshot.GetCachedModels().has_value());
  EXPECT_EQ(snapshot.GetCachedModels()->size(), 2);
  Package(2);
  auto restarted = Catalog(false);
  auto* cached = restarted->GetModelVariant("tiny-generic-cpu:2");
  ASSERT_NE(cached, nullptr);
  EXPECT_FALSE(cached->LocalPathSnapshot().empty());
}

TEST_F(TestModelCatalogTest, CacheOnlyListsLatestDownloadedButAllowsOlderDownloadedExactIds) {
  config_.additional_options["TestModelCacheOnly"] = "true";
  Snapshot({CachedInfo(1), CachedInfo(2), CachedInfo(10)});
  Package(1);
  Package(2);
  auto catalog = Catalog(false);
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{"tiny-generic-cpu:2"}));
  ASSERT_NE(catalog->GetModelVariant("tiny-generic-cpu:1"), nullptr);
  EXPECT_EQ(catalog->GetModelVariant("tiny-generic-cpu:10"), nullptr);
}

TEST_F(TestModelCatalogTest, MissingOfflineSnapshotAndCrossRegistrySnapshotFailExplicitly) {
  config_.additional_options["TestModelCacheOnly"] = "true";
  auto missing = Catalog(false);
  EXPECT_THROW(missing->ListModels(), Exception);
  auto info = CachedInfo(1);
  info.string_properties[TestModelCatalog::kRegistryProperty] = "another-registry";
  Snapshot({info});
  auto mismatched = Catalog(false);
  EXPECT_THROW(mismatched->ListModels(), Exception);
}

TEST_F(TestModelCatalogTest, PaginationUsesBothModelAndVersionNextLinksAndCorrectTokenScopes) {
  Discovery();
  transport_->Queue(kBase + "/models" + kApi, {{"value", Json::array()}, {"nextLink", kBase + "/models?page=2"}});
  transport_->Queue(kBase + "/models?page=2", {{"value", {{{"name", kCpuName}}}}});
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions" + kApi,
                    {{"value", {LegacyModel(kCpuName, 1)}}, {"nextLink", kBase + "/models/versions?page=2"}});
  transport_->Queue(kBase + "/models/versions?page=2", {{"value", {LegacyModel(kCpuName, 2)}}});
  auto client = Client();
  const auto versions = client->ListModelVersions();
  ASSERT_EQ(versions.size(), 2);
  EXPECT_EQ(versions[1].at("foundryVariantName"), kCpuName);
  EXPECT_EQ(credential_->scopes,
            (std::set<std::string>{"https://ml.azure.com/.default", "https://management.azure.com/.default"}));
}

TEST_F(TestModelCatalogTest, RejectsPaginationOutsideRegistryAndCycles) {
  Discovery();
  transport_->Queue(kBase + "/models" + kApi,
                    {{"value", Json::array()}, {"nextLink", "https://unrelated.example/models"}});
  auto client = Client();
  EXPECT_THROW(client->ListModelVersions(), Exception);
  transport_->Queue(kBase + "/models" + kApi,
                    {{"value", Json::array()}, {"nextLink", kBase + "/models" + kApi}});
  EXPECT_THROW(client->ListModelVersions(), Exception);
}

TEST_F(TestModelCatalogTest, RegistryHttpFailurePropagatesStatusAndRequestIdWithoutResponseSecrets) {
  Discovery();
  transport_->Queue(kBase + "/models" + kApi, {{"message", "private-response-marker"}},
                    HttpMethod::Get, HttpStatusCode::Forbidden);
  auto catalog = Catalog();
  try {
    catalog->ListModels();
    FAIL() << "Expected registry read failure";
  } catch (const Exception& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("403"), std::string::npos);
    EXPECT_NE(message.find("mock-request-id"), std::string::npos);
    EXPECT_EQ(message.find("private-response-marker"), std::string::npos);
  }
}

TEST_F(TestModelCatalogTest, MalformedJsonAndAuthenticationFailureAreReported) {
  Discovery();
  transport_->responses.back().body = "{invalid-json";
  auto client = Client();
  EXPECT_THROW(client->ListModelVersions(), Exception);
  credential_->fail = true;
  auto unauthenticated = Client();
  EXPECT_THROW(unauthenticated->ListModelVersions(), Exception);
}

TEST_F(TestModelCatalogTest, ArtifactAccessUsesReadGrantAndPreservesBlobPrefix) {
  Discovery();
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, LegacyModel(kCpuName, 2));
  const auto sas = "https://storage.blob.core.windows.net/container?sig=mock-signature";
  const Json request{{"assetId", "azureml://registries/mock-registry/models/" + kCpuName + "/versions/2"},
                     {"blobUri", "https://storage.blob.core.windows.net/container/v1"}};
  transport_->Queue(kBase + "/datarefs/" + kCpuName + "/versions/2" + kApi,
                    {{"blobReferenceForConsumption", {{"credential", {{"credentialType", "SAS"}, {"sasUri", sas}}}}}},
                    HttpMethod::Post, HttpStatusCode::Ok, request);
  auto client = Client();
  const auto artifacts = client->ResolveArtifacts(kCpuName, 2);
  EXPECT_EQ(artifacts.path_prefix, "v1/");
  EXPECT_EQ(artifacts.blob_sas_uri, sas);
}

TEST_F(TestModelCatalogTest, LegacyArtifactsAcceptMatchingTestProxyAndDirectoryPath) {
  Discovery("https://region.experiments.azureml-test.net/");
  auto base = kBase;
  base.replace(base.find(".azureml.net"), std::string(".azureml.net").size(), ".azureml-test.net");
  auto model = LegacyModel(kCpuName);
  model["properties"]["modelUri"] = "https://storage.blob.core.windows.net/container";
  model["properties"]["tags"]["directoryPath"] = "v4";
  transport_->Queue(base + "/models/" + kCpuName + "/versions/1" + kApi, model);
  transport_->Queue(base + "/datarefs/" + kCpuName + "/versions/1" + kApi,
                    {{"blobReferenceForConsumption",
                      {{"credential", {{"credentialType", "SAS"},
                                       {"sasUri", "https://storage-blob.dataproxy.int.api.azureml-test.ms/"
                                                  "container?sig=mock-signature"}}}}}},
                    HttpMethod::Post);
  auto client = Client();
  EXPECT_EQ(client->ResolveArtifacts(kCpuName, 1).path_prefix, "v4/");
}

TEST_F(TestModelCatalogTest, ArtifactGrantForAnotherContainerIsRejected) {
  Discovery();
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/1" + kApi, LegacyModel(kCpuName));
  transport_->Queue(kBase + "/datarefs/" + kCpuName + "/versions/1" + kApi,
                    {{"blobReferenceForConsumption",
                      {{"credential", {{"credentialType", "SAS"},
                                       {"sasUri", "https://storage.blob.core.windows.net/other?sig=mock-signature"}}}}}},
                    HttpMethod::Post);
  auto client = Client();
  EXPECT_THROW(client->ResolveArtifacts(kCpuName, 1), Exception);
}

}  // namespace
