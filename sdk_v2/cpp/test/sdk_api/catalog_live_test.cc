// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// End-to-end tests against the LIVE Azure model catalog using only the public C++ API.
//
// Lifetime / singleton note: GoogleTest runs the tests in a binary sequentially in a single
// process. Manager is a process-wide singleton ("at most one alive at a time"), so each test
// constructs its own Manager as a local - its destructor runs Manager::Destroy() at scope exit,
// before the next test starts, so no two Managers ever overlap. This binary (cache_only_tests)
// has no SharedTestEnv, so nothing else holds the singleton. This mirrors cache_only_test.cc.

#include <foundry_local/foundry_local_cpp.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

// The live catalog endpoint and an explicit region. Setting the region exercises the public
// Configuration::SetCatalogRegion() override path; it matches the URL template so routing stays
// consistent.
constexpr const char* kLiveCatalogUrl = "https://ai.azure.com/api/centralus/ux/v1.0";
constexpr const char* kLiveCatalogRegion = "centralus";

// A small model the repo already standardizes on (see SharedTestEnv). Used by the download test.
constexpr const char* kSmallModelAlias = "qwen2.5-0.5b";

#ifdef _WIN32
unsigned long CurrentPid() { return ::GetCurrentProcessId(); }
#else
pid_t CurrentPid() { return ::getpid(); }
#endif

// Removes its directory on destruction so live tests stay hermetic and never pollute the user's
// default model cache. Declared before the Manager in each test so the Manager (which may hold the
// cache open) is destroyed first.
struct TempDirGuard {
  fs::path path;

  explicit TempDirGuard(std::string tag)
      : path(fs::temp_directory_path() / ("fl_live_" + tag + "_" + std::to_string(CurrentPid()))) {
    fs::create_directories(path);
  }

  ~TempDirGuard() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }

  TempDirGuard(const TempDirGuard&) = delete;
  TempDirGuard& operator=(const TempDirGuard&) = delete;
};

foundry_local::Configuration MakeLiveConfig(const std::string& app_name, const fs::path& cache_dir) {
  foundry_local::Configuration config(app_name);
  config.SetDefaultLogLevel(FOUNDRY_LOCAL_LOG_WARNING)
      .AddCatalogUrl(kLiveCatalogUrl)
      .SetCatalogRegion(kLiveCatalogRegion)
      .SetModelCacheDir(cache_dir.string());
  return config;
}

}  // namespace

// Fetches the entire live catalog and prints every model so the catalog is visible in CI logs.
TEST(CatalogLiveTest, DumpLiveCatalog) {
  TempDirGuard cache("dump");

  foundry_local::Manager manager(MakeLiveConfig("catalog_live_dump", cache.path));
  foundry_local::ModelList models = manager.GetCatalog().GetModels();
  auto list = models.Models();

  ASSERT_FALSE(list.empty()) << "Live catalog returned no models.";

  std::cout << "\n=== Live model catalog (" << list.size() << " models, region=" << kLiveCatalogRegion
            << ") ===\n";
  for (const auto& model : list) {
    auto info = model->GetInfo();
    std::cout << "  - alias=" << info.Alias() << "  id=" << info.Id() << "  name=" << info.Name() << "\n";
    EXPECT_FALSE(info.Id().empty()) << "every catalog model must expose a non-empty id";
  }
  std::cout << "=== end catalog ===\n";
}

// Downloads a real model end-to-end through the public API. Disabled by default because it performs large network I/O.
TEST(CatalogLiveTest, DISABLED_DownloadRealModel) {
  TempDirGuard cache("download");

  try {
    foundry_local::Manager manager(MakeLiveConfig("catalog_live_download", cache.path));
    auto& catalog = manager.GetCatalog();

    auto model = catalog.GetModel(kSmallModelAlias);
    if (!model) {
      GTEST_SKIP() << "Model '" << kSmallModelAlias << "' is not in the live catalog.";
    }

    // Pick the smallest CPU variant to keep the download fast.
    std::string smallest_id;
    int64_t smallest_mb = std::numeric_limits<int64_t>::max();
    foundry_local::ModelList variants = model->GetVariants();
    for (const auto& variant : variants.Models()) {
      auto info = variant->GetInfo();
      if (info.DeviceType() != FOUNDRY_LOCAL_DEVICE_CPU) {
        continue;
      }
      int64_t mb = info.FilesizeMb().value_or(std::numeric_limits<int64_t>::max());
      if (smallest_id.empty() || mb < smallest_mb) {
        smallest_mb = mb;
        smallest_id = std::string(info.Id());
      }
    }

    if (smallest_id.empty()) {
      GTEST_SKIP() << "No CPU variant available for '" << kSmallModelAlias << "' in the live catalog.";
    }

    auto download = catalog.GetModelVariant(smallest_id);
    ASSERT_NE(download, nullptr) << "GetModelVariant should resolve the selected variant id";
    ASSERT_FALSE(download->IsCached()) << "the temp cache dir should start empty";

    std::cout << "\nDownloading " << smallest_id << " ("
              << (smallest_mb == std::numeric_limits<int64_t>::max() ? std::string("size unknown")
                                                                     : std::to_string(smallest_mb) + " MB")
              << ") to " << cache.path.string() << " ...\n";

    download->Download();

    // Re-query the catalog to confirm the variant is now reported as cached.
    auto verify = catalog.GetModelVariant(smallest_id);
    ASSERT_NE(verify, nullptr);
    EXPECT_TRUE(verify->IsCached()) << "the model should be cached after Download()";
  } catch (const foundry_local::Error& e) {
    GTEST_SKIP() << "Live download could not complete (offline or catalog/registry unreachable): "
                 << e.what();
  }
}
