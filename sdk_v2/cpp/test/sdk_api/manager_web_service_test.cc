// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for Manager's web service lifecycle (Start/Stop/GetEndpoints).
// Each test creates its own Manager instance because Manager is a singleton —
// the previous one must be destroyed before creating a new one.
//

#include <foundry_local/foundry_local_cpp.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// Windows headers define StartService as a macro (StartServiceA/StartServiceW).
// Undefine it so we can call foundry_local::Manager::StartService() directly.
#undef StartService
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ========================================================================
// Test fixture — creates a unique temp directory used as the model cache dir.
// ========================================================================

class ManagerWebServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = (fs::temp_directory_path() / ("fl_manager_ws_test_" + std::to_string(GetPid()))).string();
    fs::create_directories(test_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  // Cache-only config — convenient when the test never intends to start the web service.
  foundry_local::Configuration MakeCacheOnlyConfig() {
    foundry_local::Configuration config("manager_ws_test");
    config.SetModelCacheDir(test_dir_)
        .SetExternalServiceUrl("http://127.0.0.1:12345");
    return config;
  }

  std::string test_dir_;

 private:
#ifdef _WIN32
  static DWORD GetPid() { return ::GetCurrentProcessId(); }
#else
  static pid_t GetPid() { return ::getpid(); }
#endif
};

// ========================================================================
// Tests
// ========================================================================

// GetWebServiceEndpoints() must return empty (not throw) when the service has never been
// started. Callers rely on the empty vector as the documented "is running" probe.
TEST_F(ManagerWebServiceTest, GetWebServiceEndpointsReturnsEmptyWhenNotStarted) {
  foundry_local::Manager manager(MakeCacheOnlyConfig());

  std::vector<std::string> endpoints;
  EXPECT_NO_THROW(endpoints = manager.GetWebServiceEndpoints());
  EXPECT_TRUE(endpoints.empty());
}

// StopWebService() must be a no-op (not throw) when the service has never been started, so
// callers can shut down unconditionally.
TEST_F(ManagerWebServiceTest, StopWebServiceIsNoOpWhenNotStarted) {
  foundry_local::Manager manager(MakeCacheOnlyConfig());

  EXPECT_NO_THROW(manager.StopWebService());

  // Repeat calls must also be no-ops.
  EXPECT_NO_THROW(manager.StopWebService());

  // And GetWebServiceEndpoints() still returns empty after the no-op stops.
  EXPECT_TRUE(manager.GetWebServiceEndpoints().empty());
}

TEST_F(ManagerWebServiceTest, GetCatalogRejectsInvalidType) {
  foundry_local::Manager manager(MakeCacheOnlyConfig());

  try {
    manager.GetCatalog(static_cast<flCatalogType>(999));
    FAIL() << "Expected invalid catalog type to throw";
  } catch (const foundry_local::Error& error) {
    EXPECT_EQ(error.Code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

TEST_F(ManagerWebServiceTest, GetCatalogReturnsStableWrapperPerType) {
  foundry_local::Manager manager(MakeCacheOnlyConfig());

  auto& default_catalog = manager.GetCatalog();
  auto& public_catalog = manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_PUBLIC);
  auto& local_catalog = manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_LOCAL);

  EXPECT_EQ(&default_catalog, &public_catalog);
  EXPECT_EQ(&public_catalog, &manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_PUBLIC));
  EXPECT_EQ(&local_catalog, &manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_LOCAL));
  EXPECT_NE(&public_catalog, &local_catalog);
}

TEST_F(ManagerWebServiceTest, PublicCatalogRejectsMutation) {
  foundry_local::Manager manager(MakeCacheOnlyConfig());
  auto& public_catalog = manager.GetCatalog();
  foundry_local::ModelInfo metadata;
  metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion");

  try {
    public_catalog.RegisterModel(test_dir_, "test-model:1", metadata);
    FAIL() << "Expected public catalog registration to throw";
  } catch (const foundry_local::Error& error) {
    EXPECT_EQ(error.Code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }

  try {
    public_catalog.UnregisterModel("test-model:1");
    FAIL() << "Expected public catalog unregistration to throw";
  } catch (const foundry_local::Error& error) {
    EXPECT_EQ(error.Code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

// Start → stop → stop sequence: the second stop must be a no-op and the endpoint list must
// return to empty so callers can use it as an "is running" probe again.
TEST_F(ManagerWebServiceTest, StopWebServiceIsIdempotentAfterSuccessfulStart) {
  // Non-cache-only config so StartWebService() is allowed. Point at a known cache dir to
  // avoid touching the user's real cache, and bind to an ephemeral loopback port.
  foundry_local::Configuration config("stop_idempotent_test");
  config.SetModelCacheDir(test_dir_)
      .AddWebServiceEndpoint("http://127.0.0.1:0");

  foundry_local::Manager manager(std::move(config));

  try {
    manager.StartWebService();
  } catch (const std::exception& ex) {
    // Web service support is a compile-time option (FOUNDRY_LOCAL_BUILD_SERVICE). Only
    // skip on the build-disabled sentinel from manager.cc; rethrow anything else so real
    // failures surface instead of being silently swallowed.
    std::string what = ex.what();
    if (what.find("requires oatpp") == std::string::npos) {
      throw;
    }

    GTEST_SKIP() << "StartWebService unavailable in this build: " << what;
  }

  EXPECT_FALSE(manager.GetWebServiceEndpoints().empty());

  EXPECT_NO_THROW(manager.StopWebService());
  EXPECT_TRUE(manager.GetWebServiceEndpoints().empty());

  // Second stop after a successful start/stop must also be a no-op.
  EXPECT_NO_THROW(manager.StopWebService());
  EXPECT_TRUE(manager.GetWebServiceEndpoints().empty());
}
