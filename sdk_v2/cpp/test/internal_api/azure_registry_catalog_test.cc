// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_registry_catalog.h"

#include "catalog/catalog_cache.h"
#include "exception.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
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
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
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
    std::lock_guard lock(mutex_);
    scopes.insert(context.Scopes.begin(), context.Scopes.end());
    if (fail) throw Azure::Core::Credentials::AuthenticationException("mock authentication failure");
    return {"mock-token", Azure::DateTime(std::chrono::system_clock::now() + std::chrono::hours(1))};
  }

  bool fail = false;
  mutable std::set<std::string> scopes;
 private:
  mutable std::mutex mutex_;
};

class MockRegistryTransport final : public Azure::Core::Http::HttpTransport {
 public:
  struct Response {
    std::string url;
    std::string body;
    HttpMethod method;
    HttpStatusCode status;
    std::optional<Json> request_body;
    std::vector<uint8_t> bytes;
    bool consumed = false;
  };

  void Queue(const std::string& url, const Json& body, HttpMethod method = HttpMethod::Get,
             HttpStatusCode status = HttpStatusCode::Ok, std::optional<Json> request_body = std::nullopt) {
    responses.push_back({url, body.dump(), method, status, std::move(request_body), {}});
  }

  std::unique_ptr<Azure::Core::Http::RawResponse> Send(
      Azure::Core::Http::Request& request, const Azure::Core::Context& context) override {
    size_t index;
    {
      std::lock_guard lock(mutex_);
      const auto match = std::find_if(responses.begin(), responses.end(), [&](const Response& response) {
        return !response.consumed && response.url == request.GetUrl().GetAbsoluteUrl() &&
            response.method == request.GetMethod();
      });
      if (match == responses.end()) throw std::runtime_error("unexpected mocked Azure ML request");
      index = static_cast<size_t>(match - responses.begin());
      match->consumed = true;
      ++calls;
    }
    if (before_response) before_response(index);
    auto& expected = responses[index];
    EXPECT_EQ(request.GetUrl().GetAbsoluteUrl(), expected.url);
    EXPECT_EQ(request.GetMethod(), expected.method);
    if (expected.request_body) {
      const auto bytes = request.GetBodyStream()->ReadToEnd(context);
      EXPECT_EQ(Json::parse(bytes.begin(), bytes.end()), *expected.request_body);
    }
    auto response = std::make_unique<Azure::Core::Http::RawResponse>(1, 1, expected.status, "mock");
    response->SetHeader("x-ms-request-id", "mock-request-id");
    expected.bytes.assign(expected.body.begin(), expected.body.end());
    response->SetBodyStream(std::make_unique<Azure::Core::IO::MemoryBodyStream>(expected.bytes));
    return response;
  }

  size_t calls = 0;
  std::vector<Response> responses;
  std::function<void(size_t)> before_response;

 private:
  std::mutex mutex_;
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

Json TaggedModel(int version = 1, const std::string& name = kCpuName) {
  auto model = LegacyModel(name, version);
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

class AzureRegistryCatalogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.app_name = "MockAzureRegistryCatalog";
    config_.model_cache_dir = directory_.string();
    config_.additional_options["AzureRegistryModelCredential"] = "invalid-if-credentials-are-initialized";
  }

  void TearDown() override { EXPECT_EQ(transport_->calls, transport_->responses.size()); }

  void Discovery(const std::string& endpoint = "https://region.experiments.azureml.net/",
                 const std::string& resource_group = "group") {
    transport_->Queue("https://eastus.api.azureml.ms/registrymanagement/v1.0/registries/mock-registry/discovery",
                      {{"subscriptionId", "sub"}, {"resourceGroup", resource_group},
                       {"primaryRegionResourceProviderUri", endpoint}});
  }

  void Inventory(const std::vector<Json>& models, bool discover = true) {
    if (discover) Discovery();
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

  std::unique_ptr<AzureRegistryCatalog> Catalog(bool inject_client = true) {
    return std::make_unique<AzureRegistryCatalog>(kRegistry, config_, devices_, services_.model_load_manager,
                                            services_.logger, services_.telemetry,
                                            inject_client ? Client() : nullptr,
                                            "invalid-if-credentials-are-initialized",
                                            config_.additional_options["AzureRegistryModelCacheOnly"] == "true");
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
    info.string_properties[AzureRegistryCatalog::kRegistryProperty] = kRegistry;
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

TEST_F(AzureRegistryCatalogTest, LatestDiscoveryUsesNumericVersionsPerHardwareVariant) {
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

TEST_F(AzureRegistryCatalogTest, SharedCompatibilityPolicyAppliesToDiscoveryExactHistoryAndCachedAlias) {
  auto future = TaggedModel(2);
  future["properties"]["tags"]["minFLVersion"] = "999.0.0";
  Inventory({TaggedModel(1), future});
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{kCpuName + ":1"}));

  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, future);
  EXPECT_EQ(catalog->GetModelVariant(kCpuName + ":2"), nullptr);
  Inventory({TaggedModel(1), future}, false);
  const auto history = catalog->GetModelVersions("tiny", "", 0);
  ASSERT_EQ(history.size(), 1);
  EXPECT_EQ(history.front()->Id(), kCpuName + ":1");

  auto info = CachedInfo(2);
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR] = "999.0.0";
  Snapshot({info});
  Package(2);
  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto offline = Catalog(false);
  EXPECT_EQ(offline->GetModel("tiny"), nullptr);
  EXPECT_EQ(offline->GetModelVariant(kCpuName + ":2"), nullptr);
  EXPECT_TRUE(offline->ListModels().empty());
}

TEST_F(AzureRegistryCatalogTest, RegistryMetadataUsesSharedCapabilitiesAndLimitsConversion) {
  auto model = TaggedModel();
  model["properties"]["tags"]["capabilities"] = "tool-calling,reasoning";
  model["properties"]["tags"]["contextLength"] = "4096";
  model["properties"]["variantInfo"]["variantMetadata"]["fileSizeBytes"] = 10485760;
  Inventory({model});
  auto catalog = Catalog();
  const auto models = catalog->ListModels();
  ASSERT_EQ(models.size(), 1);
  const auto& info = models.front()->Info();
  EXPECT_EQ(info.GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT, int64_t{0}), 1);
  EXPECT_EQ(info.GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, int64_t{0}), 4096);
  EXPECT_EQ(info.GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT, int64_t{0}), 10);
  EXPECT_NE(info.GetPropertyStr("os"), nullptr);
  EXPECT_NE(info.GetPropertyStr("architecture"), nullptr);
}

TEST_F(AzureRegistryCatalogTest, OlderExactIdFetchesOnlyTheRequestedVersion) {
  Inventory({LegacyModel(kCpuName, 10)});
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, LegacyModel(kCpuName, 2));
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{"tiny-generic-cpu:10"}));
  auto* older = catalog->GetModelVariant("tiny-generic-cpu:2");
  ASSERT_NE(older, nullptr);
  EXPECT_EQ(older->Info().version, 2);
}

class AzureRegistryCatalogExactHandleTest : public AzureRegistryCatalogTest,
                                        public ::testing::WithParamInterface<std::pair<bool, bool>> {};

TEST_P(AzureRegistryCatalogExactHandleTest, OlderHandlesSurviveLatestOnlyListingsAndRemainUsable) {
  const auto [listing_first, cached_first] = GetParam();
  const auto package = [&] {
    const auto path = CachePath() / (kCpuName + "-2");
    std::filesystem::copy(test::GetTestDataPath("tiny-paged-attention"), path,
                          std::filesystem::copy_options::recursive);
    std::ofstream(path / "inference_model.json") << "{}";
    return path;
  };
  if (cached_first) {
    Snapshot({CachedInfo(2)});
    package();
  }
  const std::vector<Json> inventory{LegacyModel(kCpuName, 2), LegacyModel(kCpuName, 10)};
  if (!listing_first && !cached_first) {
    Discovery();
    transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, LegacyModel(kCpuName, 2));
    Inventory(inventory, false);
  } else {
    Inventory(inventory);
    if (!cached_first) {
      transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, LegacyModel(kCpuName, 2));
    }
  }
  auto catalog = Catalog();
  const auto expect_latest_only = [&] {
    EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{kCpuName + ":10"}));
  };
  if (listing_first) expect_latest_only();
  auto* older = catalog->GetModelVariant(kCpuName + ":2");
  ASSERT_NE(older, nullptr);
  expect_latest_only();
  ASSERT_EQ(catalog->GetModelVariant(kCpuName + ":2"), older);
  if (!cached_first) package();
  ASSERT_NO_THROW(older->RefreshCachedState());
  int progress = 0;
  ASSERT_NO_THROW(older->Download([&](float percent) {
    EXPECT_EQ(percent, 100);
    ++progress;
    return 0;
  }));
  EXPECT_EQ(progress, 1);
  EXPECT_EQ(catalog->GetCachedModels(), (std::vector<Model*>{older}));
  ASSERT_NO_THROW(older->Load());
  EXPECT_TRUE(older->IsLoaded());
  EXPECT_EQ(catalog->GetLoadedModels(), (std::vector<Model*>{older}));

  Inventory(inventory, false);
  catalog->InvalidateCache();
  expect_latest_only();
  EXPECT_EQ(catalog->GetModelVariant(kCpuName + ":2"), older);
  EXPECT_TRUE(older->IsLoaded());
  auto* latest = catalog->GetLatestVersion(older);
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(latest->Id(), kCpuName + ":10");
  ASSERT_NO_THROW(older->Unload());
  EXPECT_TRUE(catalog->GetLoadedModels().empty());
  EXPECT_NO_THROW(older->Download());
}

INSTANTIATE_TEST_SUITE_P(
    CallOrders, AzureRegistryCatalogExactHandleTest,
    ::testing::Values(std::pair{false, false}, std::pair{false, true},
                      std::pair{true, false}, std::pair{true, true}),
    [](const ::testing::TestParamInfo<std::pair<bool, bool>>& info) {
      return std::string(info.param.first ? "ListingFirst" : "ExactFirst") +
          (info.param.second ? "Cached" : "Registry");
    });

TEST_F(AzureRegistryCatalogTest, ExactLookupOfListedVersionKeepsListingCacheStateCurrentWithoutDuplicateCacheEntries) {
  Inventory({LegacyModel(kCpuName, 10)});
  auto catalog = Catalog();
  ASSERT_EQ(catalog->ListModels().size(), 1);
  auto* exact = catalog->GetModelVariant(kCpuName + ":10");
  ASSERT_NE(exact, nullptr);
  Package(10);
  exact->RefreshCachedState();
  ASSERT_NO_THROW(exact->Download());
  auto* alias = catalog->GetModel("tiny");
  ASSERT_NE(alias, nullptr);
  EXPECT_TRUE(alias->IsCached());
  EXPECT_TRUE(catalog->ListModels().front()->IsCached());
  const auto cached = catalog->GetCachedModels();
  ASSERT_EQ(cached.size(), 1);
  EXPECT_EQ(cached.front()->Id(), kCpuName + ":10");
}

TEST_F(AzureRegistryCatalogTest, MissingExactVersionReturnsNullOnColdCatalog) {
  Discovery();
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, Json{},
                    HttpMethod::Get, HttpStatusCode::NotFound);
  transport_->responses.back().body = "not-json";
  auto catalog = Catalog();
  EXPECT_EQ(catalog->GetModelVariant(kCpuName + ":2"), nullptr);
}

TEST_F(AzureRegistryCatalogTest, MissingExactVersionReturnsNullWithoutChangingListedModels) {
  Inventory({LegacyModel(kCpuName, 10)});
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, Json{},
                    HttpMethod::Get, HttpStatusCode::NotFound);
  auto catalog = Catalog();
  const std::set<std::string> expected{kCpuName + ":10"};
  EXPECT_EQ(VariantIds(catalog->ListModels()), expected);
  EXPECT_EQ(catalog->GetModelVariant(kCpuName + ":2"), nullptr);
  EXPECT_EQ(VariantIds(catalog->ListModels()), expected);
  CatalogCache snapshot(CachePath().string(), services_.logger);
  snapshot.Load();
  ASSERT_TRUE(snapshot.GetCachedModels().has_value());
  ASSERT_EQ(snapshot.GetCachedModels()->size(), 1);
  EXPECT_EQ(snapshot.GetCachedModels()->front().version, 10);
}

TEST_F(AzureRegistryCatalogTest, ExactVersionLookupPropagatesAuthorizationAndAuthenticationFailures) {
  for (const auto status : {HttpStatusCode::Unauthorized, HttpStatusCode::Forbidden}) {
    Discovery();
    transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, Json{}, HttpMethod::Get, status);
    auto catalog = Catalog();
    try {
      catalog->GetModelVariant(kCpuName + ":2");
      FAIL() << "Expected exact-version HTTP failure";
    } catch (const Exception& error) {
      EXPECT_NE(std::string(error.what()).find(std::to_string(static_cast<int>(status))), std::string::npos);
    }
  }
  credential_->fail = true;
  auto catalog = Catalog();
  EXPECT_THROW(catalog->GetModelVariant(kCpuName + ":2"), Exception);
}

TEST_F(AzureRegistryCatalogTest, ExactVersionLookupPropagatesTransportFailures) {
  Discovery();
  auto client = Client();
  client->ResourceId();
  for (int attempt = 0; attempt < 3; ++attempt) {
    transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, Json{});
  }
  transport_->before_response = [](size_t) {
    throw Azure::Core::Http::TransportException("mock connection failure");
  };
  auto catalog = std::make_unique<AzureRegistryCatalog>(kRegistry, config_, devices_, services_.model_load_manager,
                                                    services_.logger, services_.telemetry, std::move(client));
  EXPECT_THROW(catalog->GetModelVariant(kCpuName + ":2"), Exception);
}

TEST_F(AzureRegistryCatalogTest, VersionHistoryPreservesOlderVersionsAndAppliesPerNameLimit) {
  Discovery();
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions" + kApi,
                    {{"value", {LegacyModel(kCpuName, 2), LegacyModel(kCpuName, 1)}},
                     {"nextLink", kBase + "/models/" + kCpuName + "/versions?page=2"}});
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions?page=2",
                    {{"value", {LegacyModel(kCpuName, 10)}}});
  auto catalog = Catalog();
  auto versions = catalog->GetModelVersions("tiny", kCpuName, 2);
  ASSERT_EQ(versions.size(), 2);
  EXPECT_EQ(versions[0]->Info().version, 10);
  EXPECT_EQ(versions[1]->Info().version, 2);
}

TEST_F(AzureRegistryCatalogTest, NamedHistoryReturnsEmptyForMissingModelWithoutScanningInventory) {
  Discovery();
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions" + kApi,
                    Json{}, HttpMethod::Get, HttpStatusCode::NotFound);
  EXPECT_TRUE(Catalog()->GetModelVersions("tiny", kCpuName, 1).empty());
}

TEST_F(AzureRegistryCatalogTest, NamedHistorySkipsInvalidRecordsBeforeApplyingVersionLimit) {
  Discovery();
  auto malformed = TaggedModel(2);
  malformed["properties"]["tags"].erase("alias");
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions" + kApi,
                    {{"value", {TaggedModel(10), malformed}}});

  auto catalog = Catalog();
  const auto versions = catalog->GetModelVersions("tiny", kCpuName, 1);
  ASSERT_EQ(versions.size(), 1);
  EXPECT_EQ(versions.front()->Info().version, 10);
}

TEST_F(AzureRegistryCatalogTest, StrictTagsMapMetadataAndIgnoreUnrelatedAssets) {
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

TEST_F(AzureRegistryCatalogTest, InvalidOptedInRecordsAreSkippedWithoutHidingValidModels) {
  auto missing_alias = TaggedModel(1, "missing-generic-cpu");
  missing_alias["properties"]["tags"].erase("alias");
  auto malformed = TaggedModel(1, "malformed-generic-cpu");
  malformed["properties"]["tags"] = Json::array();
  auto wrong_registry = TaggedModel(1, "wrong-generic-cpu");
  wrong_registry["id"] = "azureml://registries/another-registry/models/wrong-generic-cpu/versions/1";
  Inventory({missing_alias, malformed, wrong_registry, TaggedModel()});
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{kCpuName + ":1"}));

  transport_->Queue(kBase + "/models/missing-generic-cpu/versions/1" + kApi, missing_alias);
  EXPECT_THROW(catalog->GetModelVariant("missing-generic-cpu:1"), Exception);
}

TEST_F(AzureRegistryCatalogTest, RegistryNamesMayStartWithADigit) {
  EXPECT_EQ(AzureMlRegistryClient::ValidateRegistryName("1Registry_a-2"), "1registry_a-2");
  for (const auto* name : {"a", "-registry", "registry-", "reg.istry", "registry_"}) {
    EXPECT_THROW(AzureMlRegistryClient::ValidateRegistryName(name), Exception) << name;
  }
}

TEST_F(AzureRegistryCatalogTest, VariantsOrderByDeviceMetadataWhenNamesDoNotEncodeTheDevice) {
  devices_.devices["GPU"] = {"WebGpuExecutionProvider"};
  auto accelerated = TaggedModel(1, "tiny-accelerated");
  accelerated["properties"]["tags"]["device"] = "GPU";
  accelerated["properties"]["tags"]["execution_provider"] = "WebGpuExecutionProvider";
  Inventory({TaggedModel(5, "tiny-portable"), accelerated});
  auto catalog = Catalog();
  const auto models = catalog->ListModels();
  ASSERT_EQ(models.size(), 1);
  EXPECT_EQ(models.front()->Variants().front()->Id(), "tiny-accelerated:1");
}

TEST_F(AzureRegistryCatalogTest, LoadedListingSelectionSurvivesNewerPublicationUntilUnloaded) {
  Inventory({LegacyModel(kCpuName, 2)});
  auto catalog = Catalog();
  ASSERT_EQ(catalog->ListModels().size(), 1);
  auto* listed = catalog->ListModels().front();
  const auto path = CachePath() / (kCpuName + "-2");
  std::filesystem::copy(test::GetTestDataPath("tiny-paged-attention"), path, std::filesystem::copy_options::recursive);
  std::ofstream(path / "inference_model.json") << "{}";
  ASSERT_NO_THROW(listed->RefreshCachedState());
  ASSERT_NO_THROW(listed->Load());

  const std::vector<Json> published{LegacyModel(kCpuName, 2), LegacyModel(kCpuName, 10)};
  Inventory(published, false);
  catalog->InvalidateCache();
  ASSERT_EQ(catalog->ListModels(), (std::vector<Model*>{listed}));
  EXPECT_EQ(listed->Id(), kCpuName + ":2");
  EXPECT_TRUE(listed->IsLoaded());
  EXPECT_EQ(catalog->GetLoadedModels().size(), 1);

  ASSERT_NO_THROW(listed->Unload());
  EXPECT_TRUE(catalog->GetLoadedModels().empty());
  Inventory(published, false);
  catalog->InvalidateCache();
  catalog->ListModels();
  EXPECT_EQ(listed->Id(), kCpuName + ":10");
}

TEST_F(AzureRegistryCatalogTest, RetainedHandlesObservePackageChangesMadeThroughOtherHandles) {
  Snapshot({CachedInfo(2), CachedInfo(10)});
  Package(2);
  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto catalog = Catalog(false);
  auto* alias = catalog->GetModel("tiny");
  ASSERT_NE(alias, nullptr);
  EXPECT_EQ(alias->Variants().size(), 1);

  Package(10);
  ASSERT_NO_THROW(alias->Download());
  EXPECT_EQ(alias->Variants().size(), 2);
  EXPECT_EQ(alias->Id(), kCpuName + ":10");

  auto* exact = catalog->GetModelVariant(kCpuName + ":10");
  ASSERT_NE(exact, nullptr);
  ASSERT_NO_THROW(exact->RemoveFromCache());
  ASSERT_NO_THROW(alias->Download());
  EXPECT_EQ(alias->Id(), kCpuName + ":2");
  ASSERT_NO_THROW(exact->RefreshCachedState());
  EXPECT_FALSE(exact->IsCached());
  ASSERT_NO_THROW(alias->RefreshCachedState());
  EXPECT_TRUE(alias->IsCached());

  Package(10);
  ASSERT_NO_THROW(alias->Download());
  EXPECT_EQ(alias->Id(), kCpuName + ":10");
  ASSERT_NO_THROW(alias->RefreshCachedState());
  EXPECT_TRUE(alias->IsCached());
}

TEST_F(AzureRegistryCatalogTest, LoadedListingSurvivesAliasDisappearanceUntilUnloaded) {
  Inventory({LegacyModel(kCpuName, 2)});
  auto catalog = Catalog();
  auto* listed = catalog->ListModels().front();
  const auto path = CachePath() / (kCpuName + "-2");
  std::filesystem::copy(test::GetTestDataPath("tiny-paged-attention"), path,
                        std::filesystem::copy_options::recursive);
  std::ofstream(path / "inference_model.json") << "{}";
  listed->RefreshCachedState();
  listed->Load();
  Inventory({}, false);
  catalog->InvalidateCache();
  EXPECT_EQ(catalog->ListModels(), (std::vector<Model*>{listed}));
  EXPECT_TRUE(listed->IsLoaded());
  EXPECT_EQ(catalog->GetLoadedModels().size(), 1);
  listed->Unload();
  Inventory({}, false);
  catalog->InvalidateCache();
  EXPECT_TRUE(catalog->ListModels().empty());
  EXPECT_FALSE(listed->IsLoaded());
  EXPECT_TRUE(catalog->GetLoadedModels().empty());
}

TEST_F(AzureRegistryCatalogTest, DiscoveryBoundsParallelVersionRequests) {
  std::vector<Json> models;
  for (int i = 0; i < 9; ++i) models.push_back(LegacyModel("model-" + std::to_string(i) + "-generic-cpu"));
  Inventory(models);
  std::mutex mutex;
  std::condition_variable ready;
  size_t entered = 0;
  bool released = false;
  transport_->before_response = [&](size_t index) {
    if (index < 2) return;
    std::unique_lock lock(mutex);
    ++entered;
    ready.notify_all();
    EXPECT_TRUE(ready.wait_for(lock, std::chrono::seconds(5), [&] { return released; }));
  };
  auto listing = std::async(std::launch::async, [&] { return Client()->ListModelVersions(); });
  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(ready.wait_for(lock, std::chrono::seconds(5), [&] { return entered >= 4; }));
    EXPECT_FALSE(ready.wait_for(lock, std::chrono::milliseconds(200), [&] { return entered > 4; }));
    released = true;
    ready.notify_all();
  }
  EXPECT_EQ(listing.get().size(), models.size());
}

TEST_F(AzureRegistryCatalogTest, RetainedAliasKeepsExplicitSelectionWhenNewCachedMembersAppear) {
  Snapshot({CachedInfo(2), CachedInfo(10)});
  Package(2);
  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto catalog = Catalog(false);
  auto* alias = catalog->GetModel("tiny");
  ASSERT_NE(alias, nullptr);
  alias->SelectVariant(*alias->Variants().front());
  Package(10);
  ASSERT_NO_THROW(alias->Download());
  EXPECT_EQ(alias->Variants().size(), 2);
  EXPECT_EQ(alias->Id(), kCpuName + ":2");
}

TEST_F(AzureRegistryCatalogTest, ListingDropsSnapshotEntriesFromAnotherRegistryResource) {
  auto stale = CachedInfo(2);
  stale.string_properties["azureml.registryResourceId"] = "/subscriptions/old/registries/mock-registry";
  Snapshot({stale});
  Package(2);
  Inventory({LegacyModel(kCpuName, 10)});
  Catalog()->ListModels();
  CatalogCache snapshot(CachePath().string(), services_.logger);
  snapshot.Load();
  ASSERT_TRUE(snapshot.GetCachedModels().has_value());
  ASSERT_EQ(snapshot.GetCachedModels()->size(), 1);
  EXPECT_EQ(snapshot.GetCachedModels()->front().version, 10);
}

TEST_F(AzureRegistryCatalogTest, IncompatiblePlatformIsExcluded) {
  auto model = TaggedModel();
  model["properties"]["tags"]["architecture"] =
      model["properties"]["tags"]["architecture"] == "arm64" ? "x64" : "arm64";
  Inventory({model});
  auto catalog = Catalog();
  EXPECT_TRUE(catalog->ListModels().empty());
}

TEST_F(AzureRegistryCatalogTest, UnknownBackendAndIncompleteLegacyMetadataAreSkipped) {
  auto missing_alias = LegacyModel("other-generic-cpu");
  missing_alias["properties"]["tags"].erase("alias");
  Inventory({LegacyModel("tiny-mystery-npu"), missing_alias, LegacyModel(kCpuName)});
  auto catalog = Catalog();
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{"tiny-generic-cpu:1"}));
}

TEST_F(AzureRegistryCatalogTest, HardwareFilteringMatchesProviderToDeviceAndRefreshesAfterRegistration) {
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

TEST_F(AzureRegistryCatalogTest, UnregisteredProvidersAreExcludedEvenWhenTheDeviceTypeExists) {
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

class AzureRegistryCatalogBackendTest : public AzureRegistryCatalogTest, public ::testing::WithParamInterface<Backend> {};

TEST_P(AzureRegistryCatalogBackendTest, RecognizesLegacyBackendAndRegisteredDevice) {
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
    AllBackends, AzureRegistryCatalogBackendTest,
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

TEST_F(AzureRegistryCatalogTest, CompletedCacheServesExactAndAliasWithoutCreatingCredentials) {
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

TEST_F(AzureRegistryCatalogTest, IncompleteCacheFallsBackToRegistryForExactId) {
  Snapshot({CachedInfo(2)});
  Package(2, false);
  Discovery();
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, LegacyModel(kCpuName, 2));
  auto catalog = Catalog();
  auto* model = catalog->GetModelVariant("tiny-generic-cpu:2");
  ASSERT_NE(model, nullptr);
  EXPECT_TRUE(model->LocalPathSnapshot().empty());
}

TEST_F(AzureRegistryCatalogTest, AliasCacheMissQueriesRegistryAndGroupsLatestVariant) {
  Inventory({LegacyModel(kCpuName, 1), LegacyModel(kCpuName, 10)});
  auto catalog = Catalog();
  auto* model = catalog->GetModel("tiny");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Id(), "tiny-generic-cpu:10");
  EXPECT_EQ(model->Variants().size(), 1);
}

TEST_F(AzureRegistryCatalogTest, ExactLookupMergesSnapshotAndSurvivesRestartWithoutCredentials) {
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

enum class ConcurrencyMode {
  kExactId,
  kListing,
  kHistory,
};

class AzureRegistryCatalogConcurrencyTest : public AzureRegistryCatalogTest,
                                        public ::testing::WithParamInterface<ConcurrencyMode> {};

TEST_P(AzureRegistryCatalogConcurrencyTest, SnapshotTransactionsPreserveExactIdsAcrossOfflineRestart) {
  Snapshot({CachedInfo(10)});
  Package(10);
  Discovery();
  transport_->Queue(kBase + "/models/" + kCpuName + "/versions/2" + kApi, LegacyModel(kCpuName, 2));
  if (GetParam() == ConcurrencyMode::kExactId) {
    transport_->Queue(kBase + "/models/" + kCpuName + "/versions/3" + kApi, LegacyModel(kCpuName, 3));
  } else {
    if (GetParam() == ConcurrencyMode::kListing) {
      transport_->Queue(kBase + "/models" + kApi, {{"value", {{{"name", kCpuName}}}}});
    }
    transport_->Queue(kBase + "/models/" + kCpuName + "/versions" + kApi,
                      {{"value", {LegacyModel(kCpuName, 10), LegacyModel(kCpuName, 2), LegacyModel(kCpuName, 3)}}});
  }
  auto catalog = Catalog();
  ASSERT_NE(catalog->GetModelVariant("tiny-generic-cpu:10"), nullptr);

  std::promise<void> first_entered;
  auto first_entered_future = first_entered.get_future();
  std::promise<void> release_first;
  auto release_first_future = release_first.get_future().share();
  std::promise<void> second_entered;
  auto second_entered_future = second_entered.get_future();
  transport_->before_response = [&](size_t index) {
    if (index == 1) {
      first_entered.set_value();
      EXPECT_EQ(release_first_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    } else if (index == 2) {
      second_entered.set_value();
    }
  };
  auto first = std::async(std::launch::async, [&] {
    EXPECT_NE(catalog->GetModelVariant("tiny-generic-cpu:2"), nullptr);
  });
  ASSERT_EQ(first_entered_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  std::promise<void> second_started;
  auto second_started_future = second_started.get_future();
  auto second = std::async(std::launch::async, [&] {
    second_started.set_value();
    if (GetParam() == ConcurrencyMode::kExactId) {
      EXPECT_NE(catalog->GetModelVariant("tiny-generic-cpu:3"), nullptr);
    } else if (GetParam() == ConcurrencyMode::kListing) {
      EXPECT_FALSE(catalog->ListModels().empty());
    } else {
      EXPECT_EQ(catalog->GetModelVersions("tiny", kCpuName, 0).size(), 3);
    }
  });
  EXPECT_EQ(second_started_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  const auto second_status = second_entered_future.wait_for(std::chrono::milliseconds(200));
  EXPECT_EQ(second_status, std::future_status::timeout);
  if (second_status == std::future_status::ready) second.get();
  release_first.set_value();
  first.get();
  if (second_status != std::future_status::ready) second.get();

  CatalogCache snapshot(CachePath().string(), services_.logger);
  snapshot.Load();
  const auto saved_models = snapshot.GetCachedModels();
  ASSERT_TRUE(saved_models.has_value());
  std::set<std::string> saved_ids;
  for (const auto& info : *saved_models) saved_ids.insert(info.model_id);
  EXPECT_EQ(saved_ids, (std::set<std::string>{"tiny-generic-cpu:2", "tiny-generic-cpu:3", "tiny-generic-cpu:10"}));

  const auto second_path = Package(2);
  const auto third_path = Package(3);
  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto restarted = Catalog(false);
  ASSERT_FALSE(restarted->ListModels().empty());
  auto* second_model = restarted->GetModelVariant("tiny-generic-cpu:2");
  auto* third_model = restarted->GetModelVariant("tiny-generic-cpu:3");
  ASSERT_NE(second_model, nullptr);
  ASSERT_NE(third_model, nullptr);
  EXPECT_EQ(second_model->LocalPathSnapshot(), second_path.string());
  EXPECT_EQ(third_model->LocalPathSnapshot(), third_path.string());
}

INSTANTIATE_TEST_SUITE_P(
    ConcurrentWriters, AzureRegistryCatalogConcurrencyTest,
    ::testing::Values(ConcurrencyMode::kExactId, ConcurrencyMode::kListing, ConcurrencyMode::kHistory),
    [](const ::testing::TestParamInfo<ConcurrencyMode>& info) {
      switch (info.param) {
        case ConcurrencyMode::kExactId:
          return "ExactId";
        case ConcurrencyMode::kListing:
          return "Listing";
        case ConcurrencyMode::kHistory:
          return "History";
      }
      return "Unknown";
    });

TEST_F(AzureRegistryCatalogTest, CacheOnlyListsLatestDownloadedButAllowsOlderDownloadedExactIds) {
  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  Snapshot({CachedInfo(1), CachedInfo(2), CachedInfo(10)});
  Package(1);
  Package(2);
  auto catalog = Catalog(false);
  EXPECT_EQ(VariantIds(catalog->ListModels()), (std::set<std::string>{"tiny-generic-cpu:2"}));
  ASSERT_NE(catalog->GetModelVariant("tiny-generic-cpu:1"), nullptr);
  EXPECT_EQ(catalog->GetModelVariant("tiny-generic-cpu:10"), nullptr);
}

TEST_F(AzureRegistryCatalogTest, MissingOfflineSnapshotAndCrossRegistrySnapshotFailExplicitly) {
  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto missing = Catalog(false);
  EXPECT_THROW(missing->ListModels(), Exception);
  auto info = CachedInfo(1);
  info.string_properties[AzureRegistryCatalog::kRegistryProperty] = "another-registry";
  Snapshot({info});
  auto mismatched = Catalog(false);
  EXPECT_THROW(mismatched->ListModels(), Exception);
}

TEST_F(AzureRegistryCatalogTest, PaginationUsesBothModelAndVersionNextLinksAndCorrectTokenScopes) {
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

TEST_F(AzureRegistryCatalogTest, PaginationAcceptsMixedCaseHostInNextLink) {
  Discovery("https://Region-EastUS.experiments.azureml.net/");
  const std::string mixed_base =
      "https://Region-EastUS.experiments.azureml.net/mferp/managementfrontend/subscriptions/sub/resourceGroups/group"
      "/providers/Microsoft.MachineLearningServices/registries/mock-registry";
  const std::string lower_base =
      "https://region-eastus.experiments.azureml.net/mferp/managementfrontend/subscriptions/sub/resourceGroups/group"
      "/providers/Microsoft.MachineLearningServices/registries/mock-registry";
  transport_->Queue(mixed_base + "/models" + kApi,
                    {{"value", Json::array()}, {"nextLink", lower_base + "/models?page=2"}});
  transport_->Queue(lower_base + "/models?page=2", {{"value", {{{"name", kCpuName}}}}});
  transport_->Queue(mixed_base + "/models/" + kCpuName + "/versions" + kApi,
                    {{"value", {LegacyModel(kCpuName, 1)}}});
  auto client = Client();
  const auto versions = client->ListModelVersions();
  ASSERT_EQ(versions.size(), 1);
  EXPECT_EQ(versions[0].at("foundryVariantName"), kCpuName);
}

TEST_F(AzureRegistryCatalogTest, DiscoveryEncodesParenthesizedAndUnicodeResourceGroupsAsOneSegment) {
  std::string unicode_limit;
  std::string encoded_limit;
  for (int i = 0; i < 90; ++i) {
    unicode_limit += "\xC3\xA9";
    encoded_limit += "%C3%A9";
  }
  const std::vector<std::pair<std::string, std::string>> groups{
      {"rg(test)", "rg%28test%29"},
      {"rg(\xE6\xB5\x8B\xE8\xAF\x95)", "rg%28%E6%B5%8B%E8%AF%95%29"},
      {unicode_limit, encoded_limit}};
  for (const auto& [group, encoded] : groups) {
    SCOPED_TRACE(group);
    Discovery("https://region.experiments.azureml.net/", group);
    auto base = kBase;
    base.replace(base.find("/resourceGroups/group") + std::string("/resourceGroups/").size(), 5, encoded);
    transport_->Queue(base + "/models/" + kCpuName + "/versions/1" + kApi, LegacyModel(kCpuName));
    auto client = Client();
    const auto model = client->GetModelVersion(kCpuName, "1");
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->at("foundryVariantName"), kCpuName);
    EXPECT_EQ(client->ResourceId(), "/subscriptions/sub/resourceGroups/" + group +
        "/providers/Microsoft.MachineLearningServices/registries/mock-registry");
  }
}

TEST_F(AzureRegistryCatalogTest, DiscoveryRejectsInvalidResourceGroupBoundariesAndAsciiCharacters) {
  for (const auto& group : std::vector<std::string>{
           "", "rg.", "..", "rg/test", "rg\\test", "rg%2Ftest", "rg?test", "rg#test",
           "rg test", std::string("rg\0test", 7), std::string(91, 'a')}) {
    SCOPED_TRACE(group);
    Discovery("https://region.experiments.azureml.net/", group);
    auto client = Client();
    EXPECT_THROW(client->ResourceId(), Exception);
  }
}

TEST_F(AzureRegistryCatalogTest, DiscoveryAndListingNotFoundRemainErrors) {
  Discovery();
  transport_->responses.back().status = HttpStatusCode::NotFound;
  auto undiscovered = Client();
  EXPECT_THROW(undiscovered->ListModelVersions(), Exception);

  Discovery();
  transport_->Queue(kBase + "/models" + kApi, Json{}, HttpMethod::Get, HttpStatusCode::NotFound);
  auto client = Client();
  EXPECT_THROW(client->ListModelVersions(), Exception);
}

TEST_F(AzureRegistryCatalogTest, MissingModelAndArtifactGrantRemainDownloadErrors) {
  for (const bool missing_model : {true, false}) {
    Discovery();
    transport_->Queue(kBase + "/models/" + kCpuName + "/versions/1" + kApi, LegacyModel(kCpuName),
                      HttpMethod::Get, missing_model ? HttpStatusCode::NotFound : HttpStatusCode::Ok);
    if (!missing_model) {
      transport_->Queue(kBase + "/datarefs/" + kCpuName + "/versions/1" + kApi, Json{},
                        HttpMethod::Post, HttpStatusCode::NotFound);
    }
    auto client = Client();
    EXPECT_THROW(client->ResolveArtifacts(kCpuName, 1), Exception);
  }
}

TEST_F(AzureRegistryCatalogTest, RejectsPaginationOutsideRegistryAndCycles) {
  Discovery();
  transport_->Queue(kBase + "/models" + kApi,
                    {{"value", Json::array()}, {"nextLink", "https://unrelated.example/models"}});
  auto client = Client();
  EXPECT_THROW(client->ListModelVersions(), Exception);
  transport_->Queue(kBase + "/models" + kApi,
                    {{"value", Json::array()}, {"nextLink", kBase + "/models" + kApi}});
  EXPECT_THROW(client->ListModelVersions(), Exception);
}

TEST_F(AzureRegistryCatalogTest, RegistryHttpFailurePropagatesStatusAndRequestIdWithoutResponseSecrets) {
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

TEST_F(AzureRegistryCatalogTest, MalformedJsonAndAuthenticationFailureAreReported) {
  Discovery();
  transport_->responses.back().body = "{invalid-json";
  auto client = Client();
  EXPECT_THROW(client->ListModelVersions(), Exception);
  credential_->fail = true;
  auto unauthenticated = Client();
  EXPECT_THROW(unauthenticated->ListModelVersions(), Exception);
}

TEST_F(AzureRegistryCatalogTest, ArtifactAccessUsesReadGrantAndPreservesBlobPrefix) {
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

TEST_F(AzureRegistryCatalogTest, LegacyArtifactsAcceptMatchingTestProxyAndDirectoryPath) {
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

TEST_F(AzureRegistryCatalogTest, ArtifactGrantForAnotherContainerIsRejected) {
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

TEST_F(AzureRegistryCatalogTest, RegionalArtifactProxyStaysBoundToStorageAccountAndContainer) {
  for (const auto& [url, accepted] : std::vector<std::pair<std::string, bool>>{
           {"https://storage-blob.dataproxy.eastus.api.azureml.ms/container?sig=mock", true},
           {"https://storage-blob.dataproxy.westus2.api.azureml.ms/container?sig=mock", true},
           {"https://storage-blob.dataproxy.northeurope.api.azureml.ms/container?sig=mock", true},
           {"https://other-blob.dataproxy.eastus.api.azureml.ms/container?sig=mock", false},
           {"https://storage-blob.dataproxy.eastus.api.azureml.ms/other?sig=mock", false},
           {"https://storage-blob.dataproxy.eastus.api.azureml.ms.example.com/container?sig=mock", false},
           {"https://storage-blob.dataproxy.eastus.evil.api.azureml.ms/container?sig=mock", false},
           {"https://storage-blob.dataproxy..api.azureml.ms/container?sig=mock", false},
           {"https://storage-blob.dataproxy.int.api.azureml-test.ms/container?sig=mock", false},
           {"http://storage-blob.dataproxy.eastus.api.azureml.ms/container?sig=mock", false},
           {"https://storage-blob.dataproxy.eastus.api.azureml.ms:444/container?sig=mock", false},
           {"https://storage-blob.dataproxy.eastus.api.azureml.ms/container", false}}) {
    SCOPED_TRACE(url);
    Discovery();
    transport_->Queue(kBase + "/models/" + kCpuName + "/versions/1" + kApi, LegacyModel(kCpuName));
    transport_->Queue(kBase + "/datarefs/" + kCpuName + "/versions/1" + kApi,
                      {{"blobReferenceForConsumption",
                        {{"credential", {{"credentialType", "SAS"}, {"sasUri", url}}}}}}, HttpMethod::Post);
    auto client = Client();
    if (accepted) {
      EXPECT_EQ(client->ResolveArtifacts(kCpuName, 1).blob_sas_uri, url);
    } else {
      EXPECT_THROW(client->ResolveArtifacts(kCpuName, 1), Exception);
    }
  }
}
TEST_F(AzureRegistryCatalogTest, CacheFirstAliasMaintainsIndependenceFromListingAndLoadedVersion) {
  std::filesystem::create_directories(CachePath());
  const auto package_path = CachePath() / (kCpuName + "-2");
  std::filesystem::copy(test::GetTestDataPath("tiny-paged-attention"), package_path,
                        std::filesystem::copy_options::recursive);
  std::ofstream(package_path / "inference_model.json") << "{}";
  Snapshot({CachedInfo(2)});

  const std::vector<Json> inventory{LegacyModel(kCpuName, 2), LegacyModel(kCpuName, 10)};
  Inventory(inventory);
  auto catalog = Catalog();

  // Test Order A: ListModels first, then GetModel returns cached version 2
  const auto listed = catalog->ListModels();
  ASSERT_EQ(listed.size(), 1);
  EXPECT_EQ(listed.front()->Id(), kCpuName + ":10");

  auto* alias = catalog->GetModel("tiny");
  ASSERT_NE(alias, nullptr);
  EXPECT_EQ(alias->Id(), kCpuName + ":2");

  // Test Order B: Load cached version 2, then ListModels does not replace alias selection with version 10
  ASSERT_NO_THROW(alias->Load());
  EXPECT_TRUE(alias->IsLoaded());
  EXPECT_EQ(catalog->GetLoadedModels(), (std::vector<Model*>{alias->SelectedLeaf()}));

  catalog->ListModels();
  EXPECT_EQ(alias->Id(), kCpuName + ":2");
  ASSERT_NO_THROW(alias->Unload());
  EXPECT_FALSE(alias->IsLoaded());
  EXPECT_TRUE(catalog->GetLoadedModels().empty());
}

TEST_F(AzureRegistryCatalogTest, RetainedUnloadedAliasRefreshesSelectionWhenDeviceMapChanges) {
  devices_.devices["CPU"] = {"CPUExecutionProvider"};
  devices_.devices["GPU"] = {"WebGpuExecutionProvider"};

  ModelInfo gpu_info;
  gpu_info.name = "tiny-generic-gpu";
  gpu_info.model_id = "tiny-generic-gpu:2";
  gpu_info.version = 2;
  gpu_info.alias = "tiny";
  gpu_info.device_type = DeviceType::kGPU;
  gpu_info.execution_provider = "WebGpuExecutionProvider";
  gpu_info.string_properties[AzureRegistryCatalog::kRegistryProperty] = kRegistry;
  Snapshot({CachedInfo(2), gpu_info});

  // Pre-cache both packages on disk
  Package(2);
  const auto gpu_path = CachePath() / "tiny-generic-gpu-2";
  std::filesystem::create_directories(gpu_path);
  std::ofstream(gpu_path / "genai_config.json") << "{}";
  std::ofstream(gpu_path / "inference_model.json") << "{}";

  // Start with CPU only
  auto catalog = Catalog();
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}});

  auto* alias = catalog->GetModel("tiny");
  ASSERT_NE(alias, nullptr);
  EXPECT_EQ(alias->Id(), kCpuName + ":2");

  // Register GPU and invalidate catalog
  catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"WebGpuExecutionProvider"}}});

  // Calling Download or Load on retained unloaded alias reselects GPU variant
  int progress = 0;
  alias->Download([&](float percent) {
    if (percent == 100.0f) ++progress;
    return 0;
  });
  EXPECT_EQ(alias->Id(), "tiny-generic-gpu:2");
}

TEST_F(AzureRegistryCatalogTest, CachedAliasCapacityChangesHideGpuWithoutInvalidatingLeafHandle) {
  auto gpu_info = CachedInfo(2);
  gpu_info.name = "tiny-generic-gpu";
  gpu_info.model_id = "tiny-generic-gpu:2";
  gpu_info.device_type = DeviceType::kGPU;
  gpu_info.execution_provider = "WebGpuExecutionProvider";
  gpu_info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  Snapshot({CachedInfo(2), gpu_info});
  Package(2);
  const auto gpu_path = CachePath() / "tiny-generic-gpu-2";
  std::filesystem::create_directories(gpu_path);
  std::ofstream(gpu_path / "genai_config.json") << "{}";
  std::ofstream(gpu_path / "inference_model.json") << "{}";

  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto catalog = Catalog(false);
  const auto set_capacity = [&](uint64_t bytes) {
    catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"WebGpuExecutionProvider"}}},
                               {{"WebGpuExecutionProvider", 0, "GPU", {bytes}}});
  };

  set_capacity(8);
  auto* alias = catalog->GetModel("tiny");
  ASSERT_NE(alias, nullptr);
  EXPECT_EQ(alias->Id(), kCpuName + ":2");
  EXPECT_EQ(VariantIds({alias}), (std::set<std::string>{kCpuName + ":2"}));
  auto* cpu_leaf = alias->SelectedLeaf();
  const auto retained = alias->Variants(/*include_hidden=*/true);
  ASSERT_EQ(retained.size(), 2);
  const auto gpu = std::find_if(retained.begin(), retained.end(), [&](const Model* variant) {
    return variant->Id() == gpu_info.model_id;
  });
  ASSERT_NE(gpu, retained.end());
  auto* gpu_leaf = *gpu;
  EXPECT_TRUE(gpu_leaf->IsCached());
  EXPECT_EQ(gpu_leaf->LocalPathSnapshot(), gpu_path.string());

  set_capacity(16);
  EXPECT_EQ(catalog->GetModel("tiny"), alias);
  EXPECT_EQ(alias->SelectedLeaf(), gpu_leaf);
  EXPECT_EQ(VariantIds({alias}), (std::set<std::string>{kCpuName + ":2", gpu_info.model_id}));
  EXPECT_EQ(alias->Variants(/*include_hidden=*/true), retained);

  set_capacity(8);
  EXPECT_EQ(catalog->GetModel("tiny"), alias);
  EXPECT_EQ(alias->SelectedLeaf(), cpu_leaf);
  EXPECT_EQ(VariantIds({alias}), (std::set<std::string>{kCpuName + ":2"}));
  EXPECT_EQ(alias->Variants(/*include_hidden=*/true), retained);
  ASSERT_NO_THROW(gpu_leaf->RefreshCachedState());
  EXPECT_TRUE(gpu_leaf->IsCached());
  EXPECT_EQ(gpu_leaf->Id(), gpu_info.model_id);
  EXPECT_EQ(gpu_leaf->LocalPathSnapshot(), gpu_path.string());

  set_capacity(16);
  EXPECT_EQ(catalog->GetModel("tiny"), alias);
  EXPECT_EQ(alias->SelectedLeaf(), gpu_leaf);
  EXPECT_EQ(VariantIds({alias}), (std::set<std::string>{kCpuName + ":2", gpu_info.model_id}));
  EXPECT_EQ(alias->Variants(/*include_hidden=*/true), retained);
}

TEST_F(AzureRegistryCatalogTest, ListingCapacityChangesHideGpuWithoutReplacingRetainedLeafHandle) {
  auto gpu_info = CachedInfo(2);
  gpu_info.name = "tiny-generic-gpu";
  gpu_info.model_id = "tiny-generic-gpu:2";
  gpu_info.device_type = DeviceType::kGPU;
  gpu_info.execution_provider = "WebGpuExecutionProvider";
  gpu_info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  Snapshot({CachedInfo(2), gpu_info});
  Package(2);
  const auto gpu_path = CachePath() / "tiny-generic-gpu-2";
  std::filesystem::create_directories(gpu_path);
  std::ofstream(gpu_path / "genai_config.json") << "{}";
  std::ofstream(gpu_path / "inference_model.json") << "{}";

  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto catalog = Catalog(false);
  const auto set_capacity = [&](uint64_t bytes) {
    catalog->SetRuntimeDevices({{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"WebGpuExecutionProvider"}}},
                               {{"WebGpuExecutionProvider", 0, "GPU", {bytes}}});
  };

  set_capacity(16);
  const auto listed = catalog->ListModels();
  ASSERT_EQ(listed.size(), 1u);
  auto* group = listed.front();
  auto* retained_gpu = group->SelectedLeaf();
  ASSERT_EQ(retained_gpu->Id(), gpu_info.model_id);

  set_capacity(8);
  ASSERT_EQ(catalog->ListModels(), (std::vector<Model*>{group}));
  EXPECT_EQ(group->Id(), kCpuName + ":2");
  EXPECT_EQ(VariantIds({group}), (std::set<std::string>{kCpuName + ":2"}));
  EXPECT_EQ(group->Variants(/*include_hidden=*/true).size(), 2u);
  ASSERT_NO_THROW(retained_gpu->Download());

  set_capacity(16);
  ASSERT_EQ(catalog->ListModels(), (std::vector<Model*>{group}));
  EXPECT_EQ(group->SelectedLeaf(), retained_gpu);
  EXPECT_EQ(VariantIds({group}), (std::set<std::string>{kCpuName + ":2", gpu_info.model_id}));
}

TEST_F(AzureRegistryCatalogTest, MultimodalGenericGpuCapacityDoesNotSelectFittingCudaInsteadOfWebGpu) {
  auto gpu_info = CachedInfo(2);
  gpu_info.name = "tiny-generic-gpu";
  gpu_info.model_id = "tiny-generic-gpu:2";
  gpu_info.device_type = DeviceType::kGPU;
  gpu_info.execution_provider = "WebGpuExecutionProvider";
  gpu_info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT] = 16;
  gpu_info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR] = "text,image";
  Snapshot({CachedInfo(2), gpu_info});
  Package(2);
  const auto gpu_path = CachePath() / "tiny-generic-gpu-2";
  std::filesystem::create_directories(gpu_path);
  std::ofstream(gpu_path / "genai_config.json") << R"({
    "model": {
      "type": "phi3v",
      "decoder": {"session_options": {"provider_options": [{"webgpu": {}}]}}
    }
  })";
  std::ofstream(gpu_path / "inference_model.json") << "{}";
  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto catalog = Catalog(false);
  const auto set_capacity = [&](uint64_t webgpu_bytes) {
    catalog->SetRuntimeDevices(
        {{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"CUDAExecutionProvider", "WebGpuExecutionProvider"}}},
        {{"CUDAExecutionProvider", 0, "GPU", {24}}, {"WebGpuExecutionProvider", 0, "GPU", {webgpu_bytes}}});
  };
  set_capacity(8);
  auto* alias = catalog->GetModel("tiny");
  ASSERT_NE(alias, nullptr);
  EXPECT_EQ(alias->Id(), kCpuName + ":2");
  EXPECT_EQ(alias->Variants().size(), 1u);
  set_capacity(16);
  EXPECT_EQ(catalog->GetModel("tiny"), alias);
  EXPECT_EQ(alias->Id(), gpu_info.model_id);
  EXPECT_EQ(alias->Variants().size(), 2u);
}

TEST_F(AzureRegistryCatalogTest, LiveInventorySavesRetainCompletedLocalPackagesForDeletedRemoteVersions) {
  std::filesystem::create_directories(CachePath());
  const auto package_path = CachePath() / (kCpuName + "-2");
  std::filesystem::copy(test::GetTestDataPath("tiny-paged-attention"), package_path,
                        std::filesystem::copy_options::recursive);
  std::ofstream(package_path / "inference_model.json") << "{}";
  Snapshot({CachedInfo(2)});

  // Remote registry only has version 10 (version 2 was deleted from remote registry)
  Inventory({LegacyModel(kCpuName, 10)});
  auto catalog = Catalog();

  // Online listing must only return the live version 10
  const auto listed = catalog->ListModels();
  ASSERT_EQ(listed.size(), 1);
  EXPECT_EQ(listed.front()->Id(), kCpuName + ":10");

  // Disk snapshot must retain version 2 because its completed package is on disk
  CatalogCache snapshot(CachePath().string(), services_.logger);
  snapshot.Load();
  ASSERT_TRUE(snapshot.GetCachedModels().has_value());
  const auto saved = *snapshot.GetCachedModels();
  std::set<std::string> saved_ids;
  for (const auto& info : saved) saved_ids.insert(info.model_id);
  EXPECT_EQ(saved_ids, (std::set<std::string>{kCpuName + ":2", kCpuName + ":10"}));

  // Offline / cache-only mode recovers the deleted version from retained snapshot
  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto restarted = Catalog(false);
  auto* recovered = restarted->GetModelVariant(kCpuName + ":2");
  ASSERT_NE(recovered, nullptr);
  EXPECT_TRUE(recovered->IsCached());
}

TEST_F(AzureRegistryCatalogTest, GetCachedModelsAndGetLoadedModelsIncludeOlderCompletedAndHistoryModels) {
  std::filesystem::create_directories(CachePath());
  const auto package_path = CachePath() / (kCpuName + "-2");
  std::filesystem::copy(test::GetTestDataPath("tiny-paged-attention"), package_path,
                        std::filesystem::copy_options::recursive);
  std::ofstream(package_path / "inference_model.json") << "{}";
  Snapshot({CachedInfo(2)});

  Inventory({LegacyModel(kCpuName, 10), LegacyModel(kCpuName, 2)});
  auto catalog = Catalog();

  // ListModels only indexes latest version 10
  catalog->ListModels();

  // GetCachedModels includes older completed package 2 without prior exact lookup
  const auto cached = catalog->GetCachedModels();
  ASSERT_EQ(cached.size(), 1);
  EXPECT_EQ(cached.front()->Id(), kCpuName + ":2");

  // Download and load through GetModelVersions
  Inventory({LegacyModel(kCpuName, 10), LegacyModel(kCpuName, 2)}, false);
  auto versions = catalog->GetModelVersions("tiny", "", 0);
  ASSERT_GE(versions.size(), 2);
  auto* v2 = versions[1];
  ASSERT_EQ(v2->Info().version, 2);
  ASSERT_NO_THROW(v2->Load());
  EXPECT_TRUE(v2->IsLoaded());

  // GetLoadedModels includes the version loaded through history
  const auto loaded = catalog->GetLoadedModels();
  ASSERT_EQ(loaded.size(), 1);
  EXPECT_EQ(loaded.front()->Id(), kCpuName + ":2");
  ASSERT_NO_THROW(v2->Unload());
}

TEST_F(AzureRegistryCatalogTest, GetLatestVersionPopulatesLiveListingOnFreshCatalogAndPreservesCacheOnly) {
  std::filesystem::create_directories(CachePath());
  const auto package_path = CachePath() / (kCpuName + "-2");
  std::filesystem::copy(test::GetTestDataPath("tiny-paged-attention"), package_path,
                        std::filesystem::copy_options::recursive);
  std::ofstream(package_path / "inference_model.json") << "{}";
  Snapshot({CachedInfo(2)});

  // Remote registry has version 10
  Inventory({LegacyModel(kCpuName, 10), LegacyModel(kCpuName, 2)});
  auto catalog = Catalog();

  // Fresh lookup of version 2
  auto* exact_v2 = catalog->GetModelVariant(kCpuName + ":2");
  ASSERT_NE(exact_v2, nullptr);

  // GetLatestVersion populates listing and returns remote latest (10), not 2
  auto* latest = catalog->GetLatestVersion(exact_v2);
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(latest->Id(), kCpuName + ":10");

  // In cache-only mode with only version 2 on disk, it preserves cache-only
  config_.additional_options["AzureRegistryModelCacheOnly"] = "true";
  auto cache_only_catalog = Catalog(false);
  auto* cached_exact = cache_only_catalog->GetModelVariant(kCpuName + ":2");
  ASSERT_NE(cached_exact, nullptr);
  auto* cached_latest = cache_only_catalog->GetLatestVersion(cached_exact);
  ASSERT_NE(cached_latest, nullptr);
  EXPECT_EQ(cached_latest->Id(), kCpuName + ":2");
}

}  // namespace
