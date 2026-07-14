// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "internal_api/c_api_test_helpers.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

// All tests go through the vtable obtained from FoundryLocalGetApi().
// Error handling: C API functions return flStatus* where nullptr == success (non-null == error with code + message).

using fl::test::CreateTestConfig;
using fl::test::GetApi;
using fl::test::IsOk;
using fl::test::StatusGuard;

// ========================================================================
// Exports & Version
// ========================================================================

TEST(CApiTest, GetApiReturnsNonNull) {
  const flApi* api = FoundryLocalGetApi(FOUNDRY_LOCAL_API_VERSION);
  ASSERT_NE(api, nullptr);
}

TEST(CApiTest, GetApiReturnsNullForFutureVersion) {
  const flApi* api = FoundryLocalGetApi(FOUNDRY_LOCAL_API_VERSION + 100);
  EXPECT_EQ(api, nullptr);
}

TEST(CApiTest, VersionReturnsNonNull) {
  const char* version = FoundryLocalGetVersionString();
  ASSERT_NE(version, nullptr);
  EXPECT_GT(std::strlen(version), 0u);
}

TEST(CApiTest, VersionContainsSemver) {
  const char* version = FoundryLocalGetVersionString();
  // Expect at least "x.y.z" format
  EXPECT_NE(std::strchr(version, '.'), nullptr);
}

// ========================================================================
// Status API
// ========================================================================

TEST(CApiTest, StatusCreateAndRelease) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);

  flStatus* status = api->Status_Create(FOUNDRY_LOCAL_ERROR_INTERNAL, "test error");
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INTERNAL);
  EXPECT_STREQ(api->Status_GetErrorMessage(status), "test error");

  api->Status_Release(status);
}

TEST(CApiTest, StatusReleaseNullIsNoOp) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  api->Status_Release(nullptr);
}

TEST(CApiTest, StatusCreateNullMessageIsTreatedAsEmpty) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);

  flStatus* status = api->Status_Create(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, nullptr);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);

  const char* msg = api->Status_GetErrorMessage(status);
  ASSERT_NE(msg, nullptr);
  EXPECT_STREQ(msg, "");

  api->Status_Release(status);
}

// ========================================================================
// Sub-API Accessors
// ========================================================================

TEST(CApiTest, SubApiAccessorsReturnNonNull) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);

  EXPECT_NE(api->GetCatalogApi(), nullptr);
  EXPECT_NE(api->GetConfigurationApi(), nullptr);
  EXPECT_NE(api->GetItemApi(), nullptr);
  EXPECT_NE(api->GetInferenceApi(), nullptr);
  EXPECT_NE(api->GetModelApi(), nullptr);
}

// ========================================================================
// Configuration API
// ========================================================================

TEST(CApiTest, ConfigurationCreateAndRelease) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flConfigurationApi* config_api = api->GetConfigurationApi();

  flConfiguration* config = nullptr;
  flStatus* status = config_api->Create("test-app", &config);
  EXPECT_TRUE(IsOk(status));
  ASSERT_NE(config, nullptr);

  config_api->Configuration_Release(config);
}

TEST(CApiTest, ConfigurationCreateNullAppNameFails) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flConfigurationApi* config_api = api->GetConfigurationApi();

  flConfiguration* config = nullptr;
  flStatus* status = config_api->Create(nullptr, &config);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);
}

TEST(CApiTest, ConfigurationSetters) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flConfigurationApi* config_api = api->GetConfigurationApi();

  flConfiguration* config = nullptr;
  ASSERT_TRUE(IsOk(config_api->Create("test-app", &config)));
  ASSERT_NE(config, nullptr);

  EXPECT_TRUE(IsOk(config_api->SetAppDataDir(config, "/tmp/appdata")));
  EXPECT_TRUE(IsOk(config_api->SetLogsDir(config, "/tmp/logs")));
  EXPECT_TRUE(IsOk(config_api->SetModelCacheDir(config, "/tmp/cache")));
  EXPECT_TRUE(IsOk(config_api->SetDefaultLogLevel(config, FOUNDRY_LOCAL_LOG_DEBUG)));
  EXPECT_TRUE(IsOk(config_api->AddCatalogUrl(config, "https://example.com/catalog", nullptr)));
  EXPECT_TRUE(IsOk(config_api->AddWebServiceEndpoint(config, "http://127.0.0.1:0")));

  config_api->Configuration_Release(config);
}

TEST(CApiTest, ConfigurationReleaseNullIsNoOp) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  api->GetConfigurationApi()->Configuration_Release(nullptr);
}

// ========================================================================
// Manager API
// ========================================================================

TEST(CApiTest, ManagerCreateAndRelease) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  ASSERT_FL_OK(api, api->Manager_Create(config, &mgr));
  ASSERT_NE(mgr, nullptr);

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

TEST(CApiTest, ManagerReleaseNullIsNoOp) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  api->Manager_Release(nullptr);
}

TEST(CApiTest, ManagerCreateNullConfigFails) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);

  flManager* mgr = nullptr;
  flStatus* status = api->Manager_Create(nullptr, &mgr);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);
}

// ========================================================================
// Catalog API
// ========================================================================

TEST(CApiTest, GetCatalogFromManager) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  ASSERT_FL_OK(api, api->Manager_Create(config, &mgr));
  ASSERT_NE(mgr, nullptr);

  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));
  EXPECT_NE(cat, nullptr);

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

TEST(CApiTest, GetCatalogNameReturnsNonEmptyString) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  ASSERT_FL_OK(api, api->Manager_Create(config, &mgr));
  ASSERT_NE(mgr, nullptr);

  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));
  ASSERT_NE(cat, nullptr);

  const char* name = nullptr;
  ASSERT_FL_OK(api, catalog_api->GetName(cat, &name));
  EXPECT_NE(name, nullptr);
  EXPECT_GT(std::strlen(name), 0u);

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

TEST(CApiTest, GetCatalogNameNullCatalogFails) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();

  const char* name = nullptr;
  flStatus* status = catalog_api->GetName(nullptr, &name);
  EXPECT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);
}

TEST(CApiTest, GetCatalogNameNullOutputFails) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  ASSERT_FL_OK(api, api->Manager_Create(config, &mgr));

  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));

  flStatus* status = catalog_api->GetName(cat, nullptr);
  EXPECT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

TEST(CApiTest, GetModelsFromCatalog) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  ASSERT_FL_OK(api, api->Manager_Create(config, &mgr));
  ASSERT_NE(mgr, nullptr);

  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));
  ASSERT_NE(cat, nullptr);

  flModelList* models = nullptr;
  ASSERT_FL_OK(api, catalog_api->GetModels(cat, &models));
  EXPECT_NE(models, nullptr);

  if (models) {
    // The catalog is now always fetched live from Azure during Manager_Create.
    // The exact model count depends on network/region availability, so we only
    // verify the C-API plumbing returns a usable list rather than asserting a
    // populated catalog (which would make this unit test network-dependent).
    (void)api->ModelList_Size(models);
    api->ModelList_Release(models);
  }

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

TEST(CApiTest, GetModelVersionsNullCatalogFails) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();

  flModelList* models = nullptr;
  flStatus* status = catalog_api->GetModelVersions(nullptr, "alias", nullptr, 0, &models);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);
}

TEST(CApiTest, GetModelVersionsNullOutputFails) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  ASSERT_FL_OK(api, api->Manager_Create(config, &mgr));
  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));

  flStatus* status = catalog_api->GetModelVersions(cat, "alias", nullptr, 0, nullptr);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

TEST(CApiTest, GetModelVersionsUnknownAliasReturnsEmptyList) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  flStatus* status = api->Manager_Create(config, &mgr);
  if (!IsOk(status)) {
    // Manager creation may fail if catalog is unreachable — skip gracefully.
    api->Status_Release(status);
    api->GetConfigurationApi()->Configuration_Release(config);
    GTEST_SKIP() << "Manager creation failed (catalog may be unreachable)";
  }
  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));

  flModelList* models = nullptr;
  ASSERT_FL_OK(api, catalog_api->GetModelVersions(cat, "definitely-not-a-real-alias", nullptr, 0, &models));
  ASSERT_NE(models, nullptr);
  EXPECT_EQ(api->ModelList_Size(models), 0u);
  api->ModelList_Release(models);

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

TEST(CApiTest, GetModelVersionsEmptyAliasFails) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  flStatus* status = api->Manager_Create(config, &mgr);
  if (!IsOk(status)) {
    api->Status_Release(status);
    api->GetConfigurationApi()->Configuration_Release(config);
    GTEST_SKIP() << "Manager creation failed (catalog may be unreachable)";
  }
  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));

  flModelList* models = nullptr;
  flStatus* get_versions_status = catalog_api->GetModelVersions(cat, "", nullptr, 0, &models);
  ASSERT_NE(get_versions_status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(get_versions_status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(get_versions_status);
  EXPECT_EQ(models, nullptr);

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

TEST(CApiTest, GetModelVersionsForPhi3ReturnsAllVersions) {
  // The canonical phi-3 family alias in the live Azure catalog is "phi-3-mini-4k", which is published
  // with multiple versions (e.g. generic-gpu:2, generic-cpu:3). FetchAllVersionsByAlias drops the
  // `labels=latest` filter, so this end-to-end call should return at least one variant tagged with that
  // alias. The test is network-dependent and skips gracefully if the catalog is unreachable.
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();
  const flModelApi* model_api = api->GetModelApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  flStatus* status = api->Manager_Create(config, &mgr);
  if (!IsOk(status)) {
    api->Status_Release(status);
    api->GetConfigurationApi()->Configuration_Release(config);
    GTEST_SKIP() << "Manager creation failed (catalog may be unreachable)";
  }
  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));

  constexpr const char* kPhi3Alias = "phi-3-mini-4k";
  flModelList* models = nullptr;
  ASSERT_FL_OK(api, catalog_api->GetModelVersions(cat, kPhi3Alias, nullptr, 0, &models));
  ASSERT_NE(models, nullptr);

  const size_t count = api->ModelList_Size(models);
  if (count == 0) {
    api->ModelList_Release(models);
    api->GetConfigurationApi()->Configuration_Release(config);
    api->Manager_Release(mgr);
    GTEST_SKIP() << "Catalog returned no variants for '" << kPhi3Alias
                 << "' (catalog may be unreachable or the alias has been retired)";
  }

  // Every returned model must belong to the requested alias, and across the result set we expect to see
  // more than one distinct version (the whole point of FetchAllVersionsByAlias is to bypass labels=latest).
  // Print every (model_id, version) pair so failures and successes both show the live catalog content.
  std::cout << "[          ] GetModelVersions('" << kPhi3Alias << "') returned " << count
            << " variant(s):\n";
  std::set<int> distinct_versions;
  for (size_t i = 0; i < count; ++i) {
    flModel* model = api->ModelList_GetAt(models, i);
    ASSERT_NE(model, nullptr);
    const flModelInfo* info = nullptr;
    ASSERT_FL_OK(api, model_api->GetInfo(model, &info));
    ASSERT_NE(info, nullptr);

    const char* alias = model_api->Info_GetAlias(info);
    ASSERT_NE(alias, nullptr);
    EXPECT_STREQ(alias, kPhi3Alias) << "Model at index " << i << " has unexpected alias";

    const int version = model_api->Info_GetVersion(info);
    EXPECT_GT(version, 0) << "Model at index " << i << " has non-positive version";
    distinct_versions.insert(version);

    const char* model_id = model_api->Info_GetId(info);

    // GetModelVersions is a metadata-only catalog query — by design it must NOT pull any model bits into
    // the local cache. Verify each returned variant is reported as un-cached.
    int cached = -1;
    ASSERT_FL_OK(api, model_api->IsCached(model, &cached));
    EXPECT_EQ(cached, 0) << "Model at index " << i << " (model_id='"
                         << (model_id ? model_id : "(null)")
                         << "') unexpectedly appears in the local cache after GetModelVersions";

    std::cout << "[" << i << "] model_id='" << (model_id ? model_id : "(null)")
              << "' version=" << version << " cached=" << cached << "\n";
  }

  EXPECT_GE(distinct_versions.size(), 2u)
      << "Expected at least two distinct versions for '" << kPhi3Alias
      << "', got " << distinct_versions.size();

  api->ModelList_Release(models);
  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

// Verify max_versions is applied per variant name by comparing an unbounded
// result against max_versions=1 for the same alias.
TEST(CApiTest, GetModelVersionsMaxVersionsIsPerVariant) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();
  const flModelApi* model_api = api->GetModelApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  flStatus* status = api->Manager_Create(config, &mgr);
  if (!IsOk(status)) {
    api->Status_Release(status);
    api->GetConfigurationApi()->Configuration_Release(config);
    GTEST_SKIP() << "Manager creation failed (catalog may be unreachable)";
  }
  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));

  constexpr const char* kPhi3Alias = "phi-3-mini-4k";

  // Reference: one unbounded call.
  flModelList* baseline = nullptr;
  ASSERT_FL_OK(api, catalog_api->GetModelVersions(cat, kPhi3Alias, nullptr, 0, &baseline));
  ASSERT_NE(baseline, nullptr);
  const size_t baseline_count = api->ModelList_Size(baseline);
  std::cout << "[          ] Baseline GetModelVersions('" << kPhi3Alias
            << "', max_versions=0) returned " << baseline_count << " variant(s):\n";
  if (baseline_count == 0) {
    api->ModelList_Release(baseline);
    api->GetConfigurationApi()->Configuration_Release(config);
    api->Manager_Release(mgr);
    GTEST_SKIP() << "Catalog returned no variants for '" << kPhi3Alias << "'";
  }

  std::map<std::string, std::set<int>> baseline_versions_by_variant;
  for (size_t i = 0; i < baseline_count; ++i) {
    flModel* m = api->ModelList_GetAt(baseline, i);
    ASSERT_NE(m, nullptr);
    const flModelInfo* info = nullptr;
    ASSERT_FL_OK(api, model_api->GetInfo(m, &info));
    ASSERT_NE(info, nullptr);
    const char* name = model_api->Info_GetName(info);
    ASSERT_NE(name, nullptr);
    const int version = model_api->Info_GetVersion(info);
    baseline_versions_by_variant[name].insert(version);

    const char* model_id = model_api->Info_GetId(info);
    std::cout << "[baseline " << i << "] model_id='" << (model_id ? model_id : "(null)")
              << "' name='" << name << "' version=" << version << "\n";
  }
  api->ModelList_Release(baseline);

  flModelList* capped = nullptr;
  ASSERT_FL_OK(api, catalog_api->GetModelVersions(cat, kPhi3Alias, nullptr, /*max_versions=*/1, &capped));
  ASSERT_NE(capped, nullptr);

  std::map<std::string, std::set<int>> capped_versions_by_variant;
  const size_t capped_count = api->ModelList_Size(capped);
  std::cout << "[          ] Capped GetModelVersions('" << kPhi3Alias
            << "', max_versions=1) returned " << capped_count << " variant(s):\n";
  for (size_t i = 0; i < capped_count; ++i) {
    flModel* m = api->ModelList_GetAt(capped, i);
    ASSERT_NE(m, nullptr);
    const flModelInfo* info = nullptr;
    ASSERT_FL_OK(api, model_api->GetInfo(m, &info));
    ASSERT_NE(info, nullptr);
    const char* name = model_api->Info_GetName(info);
    ASSERT_NE(name, nullptr);
    const int version = model_api->Info_GetVersion(info);
    capped_versions_by_variant[name].insert(version);

    const char* model_id = model_api->Info_GetId(info);
    std::cout << "[capped " << i << "] model_id='" << (model_id ? model_id : "(null)")
              << "' name='" << name << "' version=" << version << "\n";
  }
  api->ModelList_Release(capped);

  ASSERT_FALSE(capped_versions_by_variant.empty());
  for (const auto& [variant, versions] : capped_versions_by_variant) {
    ASSERT_EQ(versions.size(), 1u) << "variant='" << variant << "' should have at most one version";
    auto baseline_it = baseline_versions_by_variant.find(variant);
    ASSERT_NE(baseline_it, baseline_versions_by_variant.end())
        << "variant='" << variant << "' missing from baseline";
    const int expected_latest = *baseline_it->second.rbegin();
    EXPECT_EQ(*versions.begin(), expected_latest)
        << "variant='" << variant << "' did not return latest version";
  }

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

TEST(CApiTest, GetModelVersionsDoesNotPersistReturnedVariantsIntoCatalogModelView) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();
  const flModelApi* model_api = api->GetModelApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  flStatus* status = api->Manager_Create(config, &mgr);
  if (!IsOk(status)) {
    api->Status_Release(status);
    api->GetConfigurationApi()->Configuration_Release(config);
    GTEST_SKIP() << "Manager creation failed (catalog may be unreachable)";
  }
  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));

  constexpr const char* kPhi3Alias = "phi-3-mini-4k";

  flModel* baseline_model = nullptr;
  ASSERT_FL_OK(api, catalog_api->GetModel(cat, kPhi3Alias, &baseline_model));
  if (!baseline_model) {
    api->GetConfigurationApi()->Configuration_Release(config);
    api->Manager_Release(mgr);
    GTEST_SKIP() << "Catalog returned no grouped model for '" << kPhi3Alias
                 << "' (catalog may be unreachable or the alias has been retired)";
  }

  flModelList* baseline_variants = nullptr;
  ASSERT_FL_OK(api, model_api->GetVariants(baseline_model, &baseline_variants));
  ASSERT_NE(baseline_variants, nullptr);
  const size_t baseline_count = api->ModelList_Size(baseline_variants);
  api->ModelList_Release(baseline_variants);

  flModelList* versions = nullptr;
  ASSERT_FL_OK(api, catalog_api->GetModelVersions(cat, kPhi3Alias, nullptr, 0, &versions));
  ASSERT_NE(versions, nullptr);
  const size_t fetched_count = api->ModelList_Size(versions);
  if (fetched_count == 0) {
    api->ModelList_Release(versions);
    api->GetConfigurationApi()->Configuration_Release(config);
    api->Manager_Release(mgr);
    GTEST_SKIP() << "Catalog returned no variants for '" << kPhi3Alias << "'";
  }

  flModelList* after_variants = nullptr;
  ASSERT_FL_OK(api, model_api->GetVariants(baseline_model, &after_variants));
  ASSERT_NE(after_variants, nullptr);
  const size_t after_count = api->ModelList_Size(after_variants);
  api->ModelList_Release(after_variants);

  EXPECT_EQ(after_count, baseline_count)
      << "GetModelVersions should not add returned versions to the catalog's grouped model view.";

  api->ModelList_Release(versions);
  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

// Disabled by default: actually downloads a multi-GB model from Azure. Run manually with
// --gtest_also_run_disabled_tests when you need to exercise the Download code path against the live
// catalog. Skipped automatically in CI per the C++ test policy (no model downloads in CI).
TEST(CApiTest, DISABLED_DownloadPhi3MiniCpuV1) {
  // Target the explicit ':1' variant ID: GetModelVariant uses the ID-fallback path that returns a single
  // pinned variant rather than the alias' latest-labeled default.
  constexpr const char* kPhi3Alias = "phi-3-mini-4k";
  constexpr const char* kVariantId = "Phi-3-mini-4k-instruct-generic-cpu:1";
  constexpr int kExpectedVersion = 1;

  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flCatalogApi* catalog_api = api->GetCatalogApi();
  const flModelApi* model_api = api->GetModelApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  flStatus* status = api->Manager_Create(config, &mgr);
  if (!IsOk(status)) {
    api->Status_Release(status);
    api->GetConfigurationApi()->Configuration_Release(config);
    GTEST_SKIP() << "Manager creation failed (catalog may be unreachable)";
  }
  flCatalog* cat = nullptr;
  ASSERT_FL_OK(api, api->Manager_GetCatalog(mgr, &cat));

  flModel* model = nullptr;
  ASSERT_FL_OK(api, catalog_api->GetModelVariant(cat, kVariantId, &model));
  ASSERT_NE(model, nullptr);

  // Sanity-check the variant we got back matches what we asked for.
  const flModelInfo* info = nullptr;
  ASSERT_FL_OK(api, model_api->GetInfo(model, &info));
  ASSERT_NE(info, nullptr);
  EXPECT_STREQ(model_api->Info_GetId(info), kVariantId);
  EXPECT_STREQ(model_api->Info_GetAlias(info), kPhi3Alias);
  EXPECT_EQ(model_api->Info_GetVersion(info), kExpectedVersion);

  std::cout << "[          ] Target variant: model_id='" << kVariantId << "' alias='"
            << kPhi3Alias << "' version=" << kExpectedVersion << "\n";

  // Force a clean download path: if the model is already cached from a previous run, evict it first so
  // we exercise the full network code path and get a meaningful progress stream.
  int cached = -1;
  ASSERT_FL_OK(api, model_api->IsCached(model, &cached));
  if (cached) {
    std::cout << "[          ] Pre-existing cache entry detected; calling RemoveFromCache to force a "
                 "fresh download.\n";
    ASSERT_FL_OK(api, model_api->RemoveFromCache(model));
    ASSERT_FL_OK(api, model_api->IsCached(model, &cached));
    ASSERT_EQ(cached, 0) << "RemoveFromCache did not clear the cache entry";
  }

  // C ABI uses a plain function-pointer callback; route through a file-scope static vector via the
  // user-data pointer so we can capture progress values from inside the C trampoline.
  std::vector<float> progress_values;
  auto progress_cb = +[](float value, void* user_data) -> int {
    static_cast<std::vector<float>*>(user_data)->push_back(value);
    return 0;  // continue
  };

  std::cout << "[          ] Downloading " << kVariantId
            << " (this may take a while; multi-GB transfer)...\n";
  ASSERT_FL_OK(api, model_api->Download(model, progress_cb, &progress_values));

  // Confirm the cache flipped 0 -> 1 across Download.
  ASSERT_FL_OK(api, model_api->IsCached(model, &cached));
  EXPECT_EQ(cached, 1) << "Model should be cached after a successful Download";

  // A real download produces multiple progress callbacks, with the final at 100% and the sequence
  // monotonically non-decreasing.
  ASSERT_FALSE(progress_values.empty()) << "Download produced no progress callbacks";
  EXPECT_FLOAT_EQ(progress_values.back(), 100.0f);
  for (size_t i = 1; i < progress_values.size(); ++i) {
    EXPECT_GE(progress_values[i], progress_values[i - 1])
        << "Progress went backwards at index " << i;
  }
  std::cout << "[          ] Download complete: " << progress_values.size()
            << " progress callbacks, final value=" << progress_values.back() << "\n";

  // Spot-check that GetPath now returns a usable filesystem path under the test cache dir.
  const char* path = nullptr;
  ASSERT_FL_OK(api, model_api->GetPath(model, &path));
  ASSERT_NE(path, nullptr);
  EXPECT_GT(std::strlen(path), 0u);
  EXPECT_TRUE(std::filesystem::exists(path)) << "Reported model path does not exist: " << path;
  std::cout << "[          ] GetPath -> '" << path << "'\n";

  // Clean up so re-running the test from a fresh cache continues to exercise the download path, and so
  // we don't litter the developer's disk with model bits after a manual run.
  ASSERT_FL_OK(api, model_api->RemoveFromCache(model));
  ASSERT_FL_OK(api, model_api->IsCached(model, &cached));
  EXPECT_EQ(cached, 0) << "RemoveFromCache failed to evict the just-downloaded model";

  api->GetConfigurationApi()->Configuration_Release(config);
  api->Manager_Release(mgr);
}

// ========================================================================
// ModelList API
// ========================================================================

TEST(CApiTest, ModelListReleaseNullIsNoOp) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  api->ModelList_Release(nullptr);
}

TEST(CApiTest, ModelListSizeNull) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  EXPECT_EQ(api->ModelList_Size(nullptr), 0u);
}

TEST(CApiTest, ModelListGetAtNull) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  EXPECT_EQ(api->ModelList_GetAt(nullptr, 0), nullptr);
}

// ========================================================================
// Model API — RemoveFromCache
// ========================================================================

TEST(CApiTest, RemoveFromCacheNullModelFails) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flModelApi* model_api = api->GetModelApi();

  flStatus* status = model_api->RemoveFromCache(nullptr);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);
}

TEST(CApiTest, RemoveFromCacheNotCachedModelFails) {
  // Get a model from the catalog — it won't be cached (not downloaded).
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flConfigurationApi* config_api = api->GetConfigurationApi();
  const flCatalogApi* catalog_api = api->GetCatalogApi();
  const flModelApi* model_api = api->GetModelApi();

  flConfiguration* config = CreateTestConfig(api);
  ASSERT_NE(config, nullptr);

  flManager* mgr = nullptr;
  flStatus* status = api->Manager_Create(config, &mgr);
  if (!IsOk(status)) {
    // Manager creation may fail if catalog is unreachable — skip gracefully.
    api->Status_Release(status);
    config_api->Configuration_Release(config);
    GTEST_SKIP() << "Manager creation failed (catalog may be unreachable)";
  }
  ASSERT_NE(mgr, nullptr);

  flCatalog* cat = nullptr;
  ASSERT_TRUE(IsOk(api->Manager_GetCatalog(mgr, &cat)));
  ASSERT_NE(cat, nullptr);

  flModelList* models = nullptr;
  ASSERT_TRUE(IsOk(catalog_api->GetModels(cat, &models)));
  ASSERT_NE(models, nullptr);

  if (api->ModelList_Size(models) == 0) {
    api->ModelList_Release(models);
    config_api->Configuration_Release(config);
    api->Manager_Release(mgr);
    GTEST_SKIP() << "No models in catalog";
  }

  flModel* model = api->ModelList_GetAt(models, 0);
  ASSERT_NE(model, nullptr);

  // Model from catalog is not cached — RemoveFromCache should fail.
  int cached = 0;
  ASSERT_TRUE(IsOk(model_api->IsCached(model, &cached)));
  if (cached) {
    // If it happens to be cached already, skip — we need an uncached model.
    api->ModelList_Release(models);
    config_api->Configuration_Release(config);
    api->Manager_Release(mgr);
    GTEST_SKIP() << "First model is already cached";
  }

  status = model_api->RemoveFromCache(model);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  api->Status_Release(status);

  api->ModelList_Release(models);
  config_api->Configuration_Release(config);
  api->Manager_Release(mgr);
}

// ========================================================================
// KeyValuePairs API
// ========================================================================

TEST(CApiTest, KeyValuePairsCreateAndRelease) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);

  flKeyValuePairs* kvps = nullptr;
  api->CreateKeyValuePairs(&kvps);
  ASSERT_NE(kvps, nullptr);

  api->KeyValuePairs_Release(kvps);
}

TEST(CApiTest, KeyValuePairsAddAndGet) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);

  flKeyValuePairs* kvps = nullptr;
  api->CreateKeyValuePairs(&kvps);
  ASSERT_NE(kvps, nullptr);

  api->AddKeyValuePair(kvps, "key1", "value1");
  api->AddKeyValuePair(kvps, "key2", "value2");

  EXPECT_STREQ(api->GetKeyValue(kvps, "key1"), "value1");
  EXPECT_STREQ(api->GetKeyValue(kvps, "key2"), "value2");
  EXPECT_EQ(api->GetKeyValue(kvps, "nonexistent"), nullptr);

  const char* const* keys = nullptr;
  const char* const* values = nullptr;
  size_t num_entries = 0;
  api->GetKeyValuePairs(kvps, &keys, &values, &num_entries);
  EXPECT_EQ(num_entries, 2u);

  api->RemoveKeyValuePair(kvps, "key1");
  EXPECT_EQ(api->GetKeyValue(kvps, "key1"), nullptr);

  api->KeyValuePairs_Release(kvps);
}

TEST(CApiTest, KeyValuePairsReleaseNullIsNoOp) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  api->KeyValuePairs_Release(nullptr);
}

// ========================================================================
// Item API
// ========================================================================

TEST(CApiTest, ItemCreateTextAndRelease) {
  const flApi* api = GetApi();
  ASSERT_NE(api, nullptr);
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  flStatus* status = item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &item);
  EXPECT_TRUE(IsOk(status));
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item_api->GetType(item), FOUNDRY_LOCAL_ITEM_TEXT);

  // DEFAULT type round-trip.
  {
    flTextData in{};
    in.version = FOUNDRY_LOCAL_API_VERSION;
    in.text = "hello world";
    in.type = FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT;
    EXPECT_TRUE(IsOk(item_api->SetText(item, &in)));

    flTextData out{};
    out.version = FOUNDRY_LOCAL_API_VERSION;
    EXPECT_TRUE(IsOk(item_api->GetText(item, &out)));
    EXPECT_STREQ(out.text, "hello world");
    EXPECT_EQ(out.type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  }

  // REASONING type round-trip.
  {
    flTextData in{};
    in.version = FOUNDRY_LOCAL_API_VERSION;
    in.text = "let me think...";
    in.type = FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING;
    EXPECT_TRUE(IsOk(item_api->SetText(item, &in)));

    flTextData out{};
    out.version = FOUNDRY_LOCAL_API_VERSION;
    EXPECT_TRUE(IsOk(item_api->GetText(item, &out)));
    EXPECT_STREQ(out.text, "let me think...");
    EXPECT_EQ(out.type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  }

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemTextTypeMismatch) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TENSOR, &item)));
  ASSERT_NE(item, nullptr);

  // SetText / GetText on a non-TEXT item must fail.
  flTextData in{};
  in.version = FOUNDRY_LOCAL_API_VERSION;
  in.text = "bad";
  flStatus* status = item_api->SetText(item, &in);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  api->Status_Release(status);

  flTextData out{};
  out.version = FOUNDRY_LOCAL_API_VERSION;
  status = item_api->GetText(item, &out);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  api->Status_Release(status);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemCreateMessage) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_MESSAGE, &item)));
  ASSERT_NE(item, nullptr);

  flItem* text = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &text)));
  flTextData text_in{FOUNDRY_LOCAL_API_VERSION, "Hello!", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
  ASSERT_TRUE(IsOk(item_api->SetText(text, &text_in)));

  const flItem* parts[] = {text};
  flMessageData msg_in{FOUNDRY_LOCAL_API_VERSION, FOUNDRY_LOCAL_ROLE_USER, parts, 1, nullptr};
  EXPECT_TRUE(IsOk(item_api->SetMessage(item, &msg_in)));

  flMessageData msg_out{};
  msg_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetMessage(item, &msg_out)));
  EXPECT_EQ(msg_out.role, FOUNDRY_LOCAL_ROLE_USER);
  ASSERT_EQ(msg_out.content_items_count, 1u);
  ASSERT_NE(msg_out.content_items[0], nullptr);
  flTextData part_out{};
  part_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetText(msg_out.content_items[0], &part_out)));
  EXPECT_STREQ(part_out.text, "Hello!");
  EXPECT_EQ(msg_out.name, nullptr);

  item_api->Item_Release(text);
  item_api->Item_Release(item);
}

TEST(CApiTest, ItemCreateToolCall) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TOOL_CALL, &item)));
  ASSERT_NE(item, nullptr);

  flToolCallData tc_in{FOUNDRY_LOCAL_API_VERSION, "call_1", "get_weather",
                       R"({"city":"Seattle"})"};
  EXPECT_TRUE(IsOk(item_api->SetToolCall(item, &tc_in)));

  flToolCallData tc_out{};
  tc_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetToolCall(item, &tc_out)));
  EXPECT_STREQ(tc_out.call_id, "call_1");
  EXPECT_STREQ(tc_out.name, "get_weather");
  EXPECT_STREQ(tc_out.arguments, R"({"city":"Seattle"})");

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemCreateToolResult) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TOOL_RESULT, &item)));
  ASSERT_NE(item, nullptr);

  flToolResultData tr_in{FOUNDRY_LOCAL_API_VERSION, "call_1", "72°F and sunny"};
  EXPECT_TRUE(IsOk(item_api->SetToolResult(item, &tr_in)));

  flToolResultData tr_out{};
  tr_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetToolResult(item, &tr_out)));
  EXPECT_STREQ(tr_out.call_id, "call_1");
  EXPECT_STREQ(tr_out.result, "72°F and sunny");

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemReleaseNullIsNoOp) {
  const flApi* api = GetApi();
  api->GetItemApi()->Item_Release(nullptr);
}

// ========================================================================
// Speech items (output-only) — Create is rejected; Get works on internally-built items
// ========================================================================

TEST(CApiTest, ItemCreateSpeechSegmentRejected) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  flStatus* status = item_api->Create(FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT, &item);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_EQ(item, nullptr);
  api->Status_Release(status);
}

TEST(CApiTest, ItemCreateSpeechResultRejected) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  flStatus* status = item_api->Create(FOUNDRY_LOCAL_ITEM_SPEECH_RESULT, &item);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_EQ(item, nullptr);
  api->Status_Release(status);
}

// ========================================================================
// Inference API — Request / Response
// ========================================================================

TEST(CApiTest, RequestCreateAddItemAndRelease) {
  const flApi* api = GetApi();
  const flInferenceApi* inf_api = api->GetInferenceApi();
  const flItemApi* item_api = api->GetItemApi();

  flRequest* req = nullptr;
  EXPECT_TRUE(IsOk(inf_api->Request_Create(&req)));
  ASSERT_NE(req, nullptr);
  EXPECT_EQ(inf_api->Request_GetItemCount(req), 0u);

  // Create an item and add it — request borrows the pointer
  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_MESSAGE, &item)));
  flItem* text = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &text)));
  flTextData hi_data{FOUNDRY_LOCAL_API_VERSION, "Hi", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
  ASSERT_TRUE(IsOk(item_api->SetText(text, &hi_data)));
  const flItem* parts[] = {text};
  flMessageData msg{FOUNDRY_LOCAL_API_VERSION, FOUNDRY_LOCAL_ROLE_USER, parts, 1, nullptr};
  ASSERT_TRUE(IsOk(item_api->SetMessage(item, &msg)));
  item_api->Item_Release(text);  // message has internalized a copy of the part
  EXPECT_TRUE(IsOk(inf_api->Request_AddItem(req, item, /*take_ownership*/ false)));

  EXPECT_EQ(inf_api->Request_GetItemCount(req), 1u);

  const flItem* retrieved = nullptr;
  EXPECT_TRUE(IsOk(inf_api->Request_GetItem(req, 0, &retrieved)));
  EXPECT_NE(retrieved, nullptr);

  // Set options via KeyValuePairs
  flKeyValuePairs* opts = nullptr;
  api->CreateKeyValuePairs(&opts);
  api->AddKeyValuePair(opts, "temperature", "0.7");
  EXPECT_TRUE(IsOk(inf_api->Request_SetOptions(req, opts)));
  api->KeyValuePairs_Release(opts);

  inf_api->Request_Release(req);
  item_api->Item_Release(item);  // Request borrows — caller retains ownership
}

TEST(CApiTest, ResponseCreateAndRelease) {
  const flApi* api = GetApi();
  const flInferenceApi* inf_api = api->GetInferenceApi();

  flResponse* resp = nullptr;
  EXPECT_TRUE(IsOk(inf_api->Response_Create(&resp)));
  ASSERT_NE(resp, nullptr);

  EXPECT_EQ(inf_api->Response_GetItemCount(resp), 0u);
  EXPECT_EQ(inf_api->Response_GetFinishReason(resp), FOUNDRY_LOCAL_FINISH_NONE);

  flUsage usage{};
  usage.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(inf_api->Response_GetUsage(resp, &usage)));
  EXPECT_EQ(usage.total_tokens, 0);

  inf_api->Response_Release(resp);
}

TEST(CApiTest, RequestReleaseNullIsNoOp) {
  const flApi* api = GetApi();
  api->GetInferenceApi()->Request_Release(nullptr);
}

TEST(CApiTest, ResponseReleaseNullIsNoOp) {
  const flApi* api = GetApi();
  api->GetInferenceApi()->Response_Release(nullptr);
}

TEST(CApiTest, SessionReleaseNullIsNoOp) {
  const flApi* api = GetApi();
  api->GetInferenceApi()->Session_Release(nullptr);
}

// ========================================================================
// Item API — Versioned struct types (Bytes, Tensor, Image, Audio)
// ========================================================================

TEST(CApiTest, ItemCreateBytesRoundTrip) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_BYTES, &item)));
  ASSERT_NE(item, nullptr);

  uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
  flBytesData bytes_in{};
  bytes_in.version = FOUNDRY_LOCAL_API_VERSION;
  bytes_in.item_type = FOUNDRY_LOCAL_ITEM_AUDIO;
  bytes_in.data = data;
  bytes_in.data_size = sizeof(data);
  EXPECT_TRUE(IsOk(item_api->SetBytes(item, &bytes_in)));

  flBytesData bytes_out{};
  bytes_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetBytes(item, &bytes_out)));
  EXPECT_EQ(bytes_out.item_type, FOUNDRY_LOCAL_ITEM_AUDIO);
  EXPECT_EQ(bytes_out.data_size, sizeof(data));
  EXPECT_EQ(std::memcmp(bytes_out.data, data, sizeof(data)), 0);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemCreateTensorRoundTrip) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TENSOR, &item)));
  ASSERT_NE(item, nullptr);

  float data[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  int64_t shape[] = {2, 3};
  flTensorData tensor_in{};
  tensor_in.version = FOUNDRY_LOCAL_API_VERSION;
  tensor_in.data_type = FOUNDRY_LOCAL_TENSOR_FLOAT;
  tensor_in.data = data;
  tensor_in.shape = shape;
  tensor_in.rank = 2;
  EXPECT_TRUE(IsOk(item_api->SetTensor(item, &tensor_in)));

  flTensorData tensor_out{};
  tensor_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetTensor(item, &tensor_out)));
  EXPECT_EQ(tensor_out.data_type, FOUNDRY_LOCAL_TENSOR_FLOAT);
  EXPECT_EQ(tensor_out.rank, 2u);
  EXPECT_EQ(tensor_out.shape[0], 2);
  EXPECT_EQ(tensor_out.shape[1], 3);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemCreateImageBytesRoundTrip) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_IMAGE, &item)));
  ASSERT_NE(item, nullptr);

  uint8_t pixels[] = {0xFF, 0x00, 0xFF};
  flImageData image_in{};
  image_in.version = FOUNDRY_LOCAL_API_VERSION;
  image_in.data = pixels;
  image_in.data_size = sizeof(pixels);
  image_in.format = "png";
  EXPECT_TRUE(IsOk(item_api->SetImage(item, &image_in)));

  flImageData image_out{};
  image_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetImage(item, &image_out)));
  EXPECT_EQ(image_out.data_size, sizeof(pixels));
  EXPECT_STREQ(image_out.format, "png");
  EXPECT_EQ(image_out.uri, nullptr);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemCreateImageUriRoundTrip) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_IMAGE, &item)));
  ASSERT_NE(item, nullptr);

  flImageData image_in{};
  image_in.version = FOUNDRY_LOCAL_API_VERSION;
  image_in.format = "jpeg";
  image_in.uri = "https://example.com/img.jpg";
  EXPECT_TRUE(IsOk(item_api->SetImage(item, &image_in)));

  flImageData image_out{};
  image_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetImage(item, &image_out)));
  EXPECT_STREQ(image_out.uri, "https://example.com/img.jpg");
  EXPECT_STREQ(image_out.format, "jpeg");
  EXPECT_EQ(image_out.data, nullptr);
  EXPECT_EQ(image_out.data_size, 0u);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemCreateAudioRoundTrip) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_AUDIO, &item)));
  ASSERT_NE(item, nullptr);

  uint8_t data[] = {0x01, 0x02, 0x03};
  flAudioData audio_in{};
  audio_in.version = FOUNDRY_LOCAL_API_VERSION;
  audio_in.data = data;
  audio_in.data_size = sizeof(data);
  audio_in.format = "mp3";
  EXPECT_TRUE(IsOk(item_api->SetAudio(item, &audio_in)));

  flAudioData audio_out{};
  audio_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetAudio(item, &audio_out)));
  EXPECT_EQ(audio_out.data_size, sizeof(data));
  EXPECT_STREQ(audio_out.format, "mp3");
  EXPECT_EQ(audio_out.uri, nullptr);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemSetBytesNullDataWithNonZeroSizeFails) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_BYTES, &item)));

  flBytesData bytes_in{};
  bytes_in.version = FOUNDRY_LOCAL_API_VERSION;
  bytes_in.item_type = FOUNDRY_LOCAL_ITEM_AUDIO;
  bytes_in.data = nullptr;
  bytes_in.mutable_data = nullptr;
  bytes_in.data_size = 16;

  flStatus* status = item_api->SetBytes(item, &bytes_in);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemSetImageNullDataWithNonZeroSizeFails) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_IMAGE, &item)));

  flImageData image_in{};
  image_in.version = FOUNDRY_LOCAL_API_VERSION;
  image_in.data = nullptr;
  image_in.mutable_data = nullptr;
  image_in.data_size = 32;
  image_in.format = "png";

  flStatus* status = item_api->SetImage(item, &image_in);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemSetAudioNullDataWithNonZeroSizeFails) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_AUDIO, &item)));

  flAudioData audio_in{};
  audio_in.version = FOUNDRY_LOCAL_API_VERSION;
  audio_in.data = nullptr;
  audio_in.mutable_data = nullptr;
  audio_in.data_size = 64;
  audio_in.format = "pcm";

  flStatus* status = item_api->SetAudio(item, &audio_in);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemCreateMessageWithName) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_MESSAGE, &item)));
  ASSERT_NE(item, nullptr);

  flItem* text = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &text)));
  flTextData hello_in{FOUNDRY_LOCAL_API_VERSION, "Hello!", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
  ASSERT_TRUE(IsOk(item_api->SetText(text, &hello_in)));
  const flItem* parts[] = {text};
  flMessageData msg_in{FOUNDRY_LOCAL_API_VERSION, FOUNDRY_LOCAL_ROLE_ASSISTANT, parts, 1, "Alice"};
  EXPECT_TRUE(IsOk(item_api->SetMessage(item, &msg_in)));

  flMessageData msg_out{};
  msg_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetMessage(item, &msg_out)));
  EXPECT_EQ(msg_out.role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  ASSERT_EQ(msg_out.content_items_count, 1u);
  flTextData part_out{};
  part_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetText(msg_out.content_items[0], &part_out)));
  EXPECT_STREQ(part_out.text, "Hello!");
  EXPECT_STREQ(msg_out.name, "Alice");

  item_api->Item_Release(text);
  item_api->Item_Release(item);
}

// ========================================================================
// ItemQueue API
// ========================================================================

TEST(CApiTest, ItemQueueCreateAndRelease) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItemQueue* queue = nullptr;
  EXPECT_TRUE(IsOk(item_api->ItemQueue_Create(&queue)));
  ASSERT_NE(queue, nullptr);

  EXPECT_EQ(item_api->ItemQueue_Size(queue), 0u);
  EXPECT_FALSE(item_api->ItemQueue_IsFinished(queue));
  item_api->ItemQueue_Release(queue);
}

TEST(CApiTest, ItemQueuePushPopAndFinish) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItemQueue* queue = nullptr;
  ASSERT_TRUE(IsOk(item_api->ItemQueue_Create(&queue)));
  ASSERT_NE(queue, nullptr);

  // Push two text items
  flItem* item1 = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &item1)));
  flTextData first_in{FOUNDRY_LOCAL_API_VERSION, "first", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
  ASSERT_TRUE(IsOk(item_api->SetText(item1, &first_in)));
  EXPECT_TRUE(IsOk(item_api->ItemQueue_Push(queue, item1)));

  flItem* item2 = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &item2)));
  flTextData second_in{FOUNDRY_LOCAL_API_VERSION, "second", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
  ASSERT_TRUE(IsOk(item_api->SetText(item2, &second_in)));
  EXPECT_TRUE(IsOk(item_api->ItemQueue_Push(queue, item2)));

  EXPECT_EQ(item_api->ItemQueue_Size(queue), 2u);

  // Pop first
  flItem* popped = nullptr;
  EXPECT_TRUE(item_api->ItemQueue_TryPop(queue, &popped));
  ASSERT_NE(popped, nullptr);
  flTextData popped_out{};
  popped_out.version = FOUNDRY_LOCAL_API_VERSION;
  ASSERT_TRUE(IsOk(item_api->GetText(popped, &popped_out)));
  EXPECT_STREQ(popped_out.text, "first");
  item_api->Item_Release(popped);

  EXPECT_EQ(item_api->ItemQueue_Size(queue), 1u);

  // Mark finished
  item_api->ItemQueue_MarkFinished(queue);
  EXPECT_TRUE(item_api->ItemQueue_IsFinished(queue));
  // Pop second
  popped = nullptr;
  EXPECT_TRUE(item_api->ItemQueue_TryPop(queue, &popped));
  ASSERT_NE(popped, nullptr);
  popped_out = {};
  popped_out.version = FOUNDRY_LOCAL_API_VERSION;
  ASSERT_TRUE(IsOk(item_api->GetText(popped, &popped_out)));
  EXPECT_STREQ(popped_out.text, "second");
  item_api->Item_Release(popped);

  // Empty — TryPop returns false
  popped = nullptr;
  EXPECT_FALSE(item_api->ItemQueue_TryPop(queue, &popped));

  item_api->ItemQueue_Release(queue);
}

// ========================================================================
// Inference API — Request_Cancel
// ========================================================================

TEST(CApiTest, RequestCancelOnIdleRequest) {
  const flApi* api = GetApi();
  const flInferenceApi* inf_api = api->GetInferenceApi();

  flRequest* req = nullptr;
  ASSERT_TRUE(IsOk(inf_api->Request_Create(&req)));
  ASSERT_NE(req, nullptr);

  // Cancel on an idle request should succeed (no-op)
  EXPECT_TRUE(IsOk(inf_api->Request_Cancel(req)));

  inf_api->Request_Release(req);
}

// ========================================================================
// Item API — Metadata
// ========================================================================

TEST(CApiTest, ItemMetadataInitiallyNull) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &item)));
  ASSERT_NE(item, nullptr);

  const flKeyValuePairs* meta = nullptr;
  EXPECT_TRUE(IsOk(item_api->GetMetadata(item, &meta)));
  EXPECT_EQ(meta, nullptr);

  item_api->Item_Release(item);
}

TEST(CApiTest, ItemMutableMetadataCreatesOnDemand) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &item)));
  ASSERT_NE(item, nullptr);

  flKeyValuePairs* meta = nullptr;
  EXPECT_TRUE(IsOk(item_api->GetMutableMetadata(item, &meta)));
  ASSERT_NE(meta, nullptr);

  // Add metadata and read it back
  api->AddKeyValuePair(meta, "key1", "value1");
  EXPECT_STREQ(api->GetKeyValue(meta, "key1"), "value1");

  // Read-only accessor should now see the same data
  const flKeyValuePairs* meta_ro = nullptr;
  EXPECT_TRUE(IsOk(item_api->GetMetadata(item, &meta_ro)));
  ASSERT_NE(meta_ro, nullptr);
  EXPECT_STREQ(api->GetKeyValue(meta_ro, "key1"), "value1");

  // Metadata is owned by item — do NOT release it separately
  item_api->Item_Release(item);
}

// ========================================================================
// Item API — Per-type deleter callback via embedded deleter in data struct
// ========================================================================

TEST(CApiTest, TensorDeleterFiredOnRelease) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  static bool deleter_called = false;
  static void* captured_user_data = nullptr;
  deleter_called = false;
  captured_user_data = nullptr;

  int marker = 99;
  float buf[] = {1.0f, 2.0f};
  int64_t shape[] = {2};

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TENSOR, &item)));
  ASSERT_NE(item, nullptr);

  auto deleter = [](const flTensorData* /*td*/, void* user_data) {
    deleter_called = true;
    captured_user_data = user_data;
  };

  flTensorData td{};
  td.version = FOUNDRY_LOCAL_API_VERSION;
  td.data_type = FOUNDRY_LOCAL_TENSOR_FLOAT;
  td.data = buf;
  td.mutable_data = buf;
  td.shape = shape;
  td.rank = 1;
  td.deleter = deleter;
  td.deleter_user_data = &marker;
  EXPECT_TRUE(IsOk(item_api->SetTensor(item, &td)));

  item_api->Item_Release(item);
  EXPECT_TRUE(deleter_called);
  EXPECT_EQ(captured_user_data, &marker);
}

// ========================================================================
// Item API — GetQueue (QUEUE item contains sub-queue)
// ========================================================================

TEST(CApiTest, ItemGetQueueFromQueueItem) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  // Create a QUEUE item
  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_QUEUE, &item)));
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item_api->GetType(item), FOUNDRY_LOCAL_ITEM_QUEUE);

  // Get the embedded queue
  flItemQueue* queue = nullptr;
  EXPECT_TRUE(IsOk(item_api->GetQueue(item, &queue)));
  ASSERT_NE(queue, nullptr);

  // Push an item through the embedded queue
  flItem* text_item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &text_item)));
  flTextData via_in{FOUNDRY_LOCAL_API_VERSION, "via-queue-item", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
  ASSERT_TRUE(IsOk(item_api->SetText(text_item, &via_in)));
  EXPECT_TRUE(IsOk(item_api->ItemQueue_Push(queue, text_item)));

  EXPECT_EQ(item_api->ItemQueue_Size(queue), 1u);

  flItem* popped = nullptr;
  EXPECT_TRUE(item_api->ItemQueue_TryPop(queue, &popped));
  flTextData via_out{};
  via_out.version = FOUNDRY_LOCAL_API_VERSION;
  ASSERT_TRUE(IsOk(item_api->GetText(popped, &via_out)));
  EXPECT_STREQ(via_out.text, "via-queue-item");
  item_api->Item_Release(popped);

  // Do NOT release the queue — it's owned by the item
  item_api->Item_Release(item);
}

TEST(CApiTest, ItemGetQueueFromNonQueueFails) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &item)));
  ASSERT_NE(item, nullptr);

  flItemQueue* queue = nullptr;
  flStatus* status = item_api->GetQueue(item, &queue);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  api->Status_Release(status);

  item_api->Item_Release(item);
}

// ========================================================================
// Inference API — Request_AddItem with ownership transfer
// ========================================================================

TEST(CApiTest, RequestAddOwnedItem) {
  const flApi* api = GetApi();
  const flInferenceApi* inf_api = api->GetInferenceApi();
  const flItemApi* item_api = api->GetItemApi();

  flRequest* req = nullptr;
  ASSERT_TRUE(IsOk(inf_api->Request_Create(&req)));
  ASSERT_NE(req, nullptr);

  // Create an item and add it with ownership transfer
  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &item)));
  flTextData owned_in{FOUNDRY_LOCAL_API_VERSION, "owned by request", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
  ASSERT_TRUE(IsOk(item_api->SetText(item, &owned_in)));
  EXPECT_TRUE(IsOk(inf_api->Request_AddItem(req, item, /*take_ownership*/ true)));

  EXPECT_EQ(inf_api->Request_GetItemCount(req), 1u);

  // Verify we can read it back
  const flItem* retrieved = nullptr;
  EXPECT_TRUE(IsOk(inf_api->Request_GetItem(req, 0, &retrieved)));
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(item_api->GetType(retrieved), FOUNDRY_LOCAL_ITEM_TEXT);

  // Request owns the item — releasing request should free it. No Item_Release needed.
  inf_api->Request_Release(req);
}

TEST(CApiTest, RequestMixedBorrowedAndOwned) {
  const flApi* api = GetApi();
  const flInferenceApi* inf_api = api->GetInferenceApi();
  const flItemApi* item_api = api->GetItemApi();

  flRequest* req = nullptr;
  ASSERT_TRUE(IsOk(inf_api->Request_Create(&req)));

  // Borrowed item — caller retains ownership
  flItem* borrowed = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_MESSAGE, &borrowed)));
  flItem* text_part = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &text_part)));
  flTextData borrowed_in{FOUNDRY_LOCAL_API_VERSION, "borrowed", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
  ASSERT_TRUE(IsOk(item_api->SetText(text_part, &borrowed_in)));
  const flItem* parts[] = {text_part};
  flMessageData msg{FOUNDRY_LOCAL_API_VERSION, FOUNDRY_LOCAL_ROLE_USER, parts, 1, nullptr};
  ASSERT_TRUE(IsOk(item_api->SetMessage(borrowed, &msg)));
  item_api->Item_Release(text_part);
  EXPECT_TRUE(IsOk(inf_api->Request_AddItem(req, borrowed, /*take_ownership*/ false)));

  // Owned item — request takes ownership
  flItem* owned = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_TEXT, &owned)));
  flTextData owned2_in{FOUNDRY_LOCAL_API_VERSION, "owned", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
  ASSERT_TRUE(IsOk(item_api->SetText(owned, &owned2_in)));
  EXPECT_TRUE(IsOk(inf_api->Request_AddItem(req, owned, /*take_ownership*/ true)));

  EXPECT_EQ(inf_api->Request_GetItemCount(req), 2u);

  inf_api->Request_Release(req);
  item_api->Item_Release(borrowed);  // only the borrowed one needs manual cleanup
}

// ========================================================================
// Item API — Audio URI round-trip
// ========================================================================

TEST(CApiTest, ItemCreateAudioUriRoundTrip) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  ASSERT_TRUE(IsOk(item_api->Create(FOUNDRY_LOCAL_ITEM_AUDIO, &item)));
  ASSERT_NE(item, nullptr);

  flAudioData audio_in{};
  audio_in.version = FOUNDRY_LOCAL_API_VERSION;
  audio_in.format = "wav";
  audio_in.uri = "https://example.com/audio.wav";
  EXPECT_TRUE(IsOk(item_api->SetAudio(item, &audio_in)));

  flAudioData audio_out{};
  audio_out.version = FOUNDRY_LOCAL_API_VERSION;
  EXPECT_TRUE(IsOk(item_api->GetAudio(item, &audio_out)));
  EXPECT_STREQ(audio_out.uri, "https://example.com/audio.wav");
  EXPECT_STREQ(audio_out.format, "wav");
  EXPECT_EQ(audio_out.data, nullptr);
  EXPECT_EQ(audio_out.data_size, 0u);

  item_api->Item_Release(item);
}

// ========================================================================
// Item API — All type discriminators via Create+GetType
// ========================================================================

TEST(CApiTest, AllItemTypesHaveCorrectDiscriminator) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  struct TypeCase {
    flItemType type;
    const char* label;
  };

  TypeCase cases[] = {
      {FOUNDRY_LOCAL_ITEM_TEXT, "TEXT"},
      {FOUNDRY_LOCAL_ITEM_MESSAGE, "MESSAGE"},
      {FOUNDRY_LOCAL_ITEM_TOOL_CALL, "TOOL_CALL"},
      {FOUNDRY_LOCAL_ITEM_TOOL_RESULT, "TOOL_RESULT"},
      {FOUNDRY_LOCAL_ITEM_IMAGE, "IMAGE"},
      {FOUNDRY_LOCAL_ITEM_AUDIO, "AUDIO"},
      {FOUNDRY_LOCAL_ITEM_TENSOR, "TENSOR"},
      {FOUNDRY_LOCAL_ITEM_BYTES, "BYTES"},
      {FOUNDRY_LOCAL_ITEM_QUEUE, "QUEUE"},
  };

  for (const auto& tc : cases) {
    flItem* item = nullptr;
    flStatus* status = item_api->Create(tc.type, &item);
    EXPECT_TRUE(IsOk(status)) << "Create failed for " << tc.label;
    ASSERT_NE(item, nullptr) << "Item null for " << tc.label;
    EXPECT_EQ(item_api->GetType(item), tc.type) << "Type mismatch for " << tc.label;
    item_api->Item_Release(item);
  }
}

TEST(CApiTest, ItemCreateUnknownTypeFails) {
  const flApi* api = GetApi();
  const flItemApi* item_api = api->GetItemApi();

  flItem* item = nullptr;
  flStatus* status = item_api->Create(FOUNDRY_LOCAL_ITEM_UNKNOWN, &item);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api->Status_Release(status);
}
