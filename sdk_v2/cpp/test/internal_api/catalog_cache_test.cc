// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for CatalogCache — disk-based model info caching with freshness logic.
//
#include "catalog/catalog_cache.h"
#include "logger.h"
#include "model_info.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace fl;
namespace fs = std::filesystem;

// ========================================================================
// Test fixture — creates a unique temp directory per test
// ========================================================================

class CatalogCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = (fs::temp_directory_path() / ("fl_cache_test_" + std::to_string(GetPid()))).string();
    fs::create_directories(test_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  std::string CacheFilePath() const {
    return (fs::path(test_dir_) / "foundry.modelinfo.json").string();
  }

  /// Helper: build a simple ModelInfo for test purposes.
  static ModelInfo MakeTestModel(const std::string& id, int version) {
    ModelInfo info;
    info.model_id = id;
    info.name = id.substr(0, id.find(':'));
    info.version = version;
    info.alias = info.name;
    info.uri = "azureml://test/" + id;
    return info;
  }

  /// Helper: write raw JSON content to the cache file path.
  void WriteCacheFile(const std::string& content) {
    std::ofstream f(CacheFilePath(), std::ios::trunc);
    ASSERT_TRUE(f.is_open());
    f << content;
  }

#ifdef _WIN32
  static DWORD GetPid() { return ::GetCurrentProcessId(); }
#else
  static pid_t GetPid() { return ::getpid(); }
#endif

  std::string test_dir_;
  StderrLogger logger_;
};

// ========================================================================
// Tests
// ========================================================================

TEST_F(CatalogCacheTest, GetCachedModelsBeforeLoadReturnsNullopt) {
  CatalogCache cache(test_dir_, logger_);
  EXPECT_FALSE(cache.GetCachedModels().has_value());
}

TEST_F(CatalogCacheTest, LoadFromMissingFileReturnsNullopt) {
  // No cache file exists — Load should silently succeed, GetCachedModels returns nullopt.
  CatalogCache cache(test_dir_, logger_);
  cache.Load();
  EXPECT_FALSE(cache.GetCachedModels().has_value());
}

TEST_F(CatalogCacheTest, SaveAndLoadRoundTrip) {
  std::vector<ModelInfo> models = {
      MakeTestModel("phi-4-mini:3", 3),
      MakeTestModel("llama-3:1", 1),
  };

  // Save
  {
    CatalogCache cache(test_dir_, logger_);
    cache.Save(models);
  }

  // Load in a new instance
  {
    CatalogCache cache(test_dir_, logger_);
    cache.Load();
    auto loaded = cache.GetCachedModels();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->size(), 2u);
    EXPECT_EQ((*loaded)[0].model_id, "phi-4-mini:3");
    EXPECT_EQ((*loaded)[0].version, 3);
    EXPECT_EQ((*loaded)[0].uri, "azureml://test/phi-4-mini:3");
    EXPECT_EQ((*loaded)[1].model_id, "llama-3:1");
    EXPECT_EQ((*loaded)[1].version, 1);
  }
}

TEST_F(CatalogCacheTest, DetectedRegionRoundTrip) {
  ModelInfo model = MakeTestModel("phi-4-mini:3", 3);
  model.detected_region = "westus2";

  {
    CatalogCache cache(test_dir_, logger_);
    cache.Save({model});
  }

  {
    CatalogCache cache(test_dir_, logger_);
    cache.Load();
    auto loaded = cache.GetCachedModels();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->size(), 1u);
    EXPECT_EQ((*loaded)[0].detected_region, "westus2");
  }
}

TEST_F(CatalogCacheTest, EmptyModelListRoundTrip) {
  std::vector<ModelInfo> empty_models;

  {
    CatalogCache cache(test_dir_, logger_);
    cache.Save(empty_models);
  }

  {
    CatalogCache cache(test_dir_, logger_);
    cache.Load();
    auto loaded = cache.GetCachedModels();

    // Should return an empty vector, NOT nullopt.
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->empty());
  }
}

TEST_F(CatalogCacheTest, LoadWithCorruptJsonReturnsNullopt) {
  WriteCacheFile("this is not valid json {{{");

  CatalogCache cache(test_dir_, logger_);
  cache.Load();
  EXPECT_FALSE(cache.GetCachedModels().has_value());
}

TEST_F(CatalogCacheTest, LoadWithWrongVersionReturnsNullopt) {
  nlohmann::json j;
  j["version"] = 99;
  j["models"] = nlohmann::json::array();
  WriteCacheFile(j.dump());

  CatalogCache cache(test_dir_, logger_);
  cache.Load();
  EXPECT_FALSE(cache.GetCachedModels().has_value());
}

TEST_F(CatalogCacheTest, LoadWithMissingModelsArrayReturnsNullopt) {
  nlohmann::json j;
  j["version"] = 1;
  // No "models" key
  WriteCacheFile(j.dump());

  CatalogCache cache(test_dir_, logger_);
  cache.Load();
  EXPECT_FALSE(cache.GetCachedModels().has_value());
}

TEST_F(CatalogCacheTest, SaveFreshnessCheckSkipsRecentSave) {
  std::vector<ModelInfo> models = {MakeTestModel("test-model:1", 1)};

  CatalogCache cache(test_dir_, logger_);
  cache.Save(models);

  // Record the file's last write time after the first save.
  auto time_after_first_save = fs::last_write_time(CacheFilePath());

  // Save again immediately — should be skipped because the file is fresh.
  std::vector<ModelInfo> models2 = {MakeTestModel("different-model:2", 2)};
  cache.Save(models2);

  auto time_after_second_save = fs::last_write_time(CacheFilePath());
  EXPECT_EQ(time_after_first_save, time_after_second_save);

  // Verify the file still has the original data (second save was skipped).
  CatalogCache verify_cache(test_dir_, logger_);
  verify_cache.Load();
  auto loaded = verify_cache.GetCachedModels();
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->size(), 1u);
  EXPECT_EQ((*loaded)[0].model_id, "test-model:1");
}

TEST_F(CatalogCacheTest, SaveSucceedsWhenExistingTimestampIsOld) {
  // Write a cache file with an old savedAtUnix timestamp.
  auto old_time = std::chrono::system_clock::now() - std::chrono::hours(12);
  auto old_unix = std::chrono::duration_cast<std::chrono::seconds>(
                      old_time.time_since_epoch())
                      .count();

  nlohmann::json old_cache;
  old_cache["version"] = 1;
  old_cache["savedAtUnix"] = old_unix;
  old_cache["models"] = nlohmann::json::array();
  WriteCacheFile(old_cache.dump());

  // Save new data — should succeed because existing cache is older than 6 hours.
  std::vector<ModelInfo> models = {MakeTestModel("new-model:5", 5)};
  CatalogCache cache(test_dir_, logger_);
  cache.Save(models);

  // Verify the file was updated.
  CatalogCache verify_cache(test_dir_, logger_);
  verify_cache.Load();
  auto loaded = verify_cache.GetCachedModels();
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->size(), 1u);
  EXPECT_EQ((*loaded)[0].model_id, "new-model:5");
}

TEST_F(CatalogCacheTest, SaveIsAtomic_StaleTempFileDoesNotCorruptDestination) {
  // Simulate a previous crash mid-write: a stale .tmp file sits beside the cache.
  // The next Save must still produce a valid destination (no truncation/corruption)
  // and must not be defeated by the stale temp file.
  std::vector<ModelInfo> first = {MakeTestModel("good-model:1", 1)};

  CatalogCache cache(test_dir_, logger_);
  cache.Save(first);

  // Drop a stale partial temp file in the cache directory. A crashed previous run
  // would leave one of these behind; the next Save must not inherit its contents.
  auto stale_temp = CacheFilePath() + ".tmp.999999";
  {
    std::ofstream f(stale_temp);
    f << "{ this is not valid json -- partial write";
  }

  // Force the freshness check to allow a re-save by aging the existing cache.
  auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(12);
  fs::last_write_time(CacheFilePath(), old_time);

  // Also rewrite savedAtUnix to be old so the save proceeds.
  {
    nlohmann::json j;
    j["version"] = 1;
    j["savedAtUnix"] = std::chrono::duration_cast<std::chrono::seconds>(
                           (std::chrono::system_clock::now() - std::chrono::hours(12))
                               .time_since_epoch())
                           .count();
    j["models"] = nlohmann::json::array();
    WriteCacheFile(j.dump());
  }

  std::vector<ModelInfo> second = {MakeTestModel("replacement:7", 7)};
  cache.Save(second);

  // The destination must parse cleanly and contain the new data — never the
  // stale-temp contents.
  CatalogCache verify(test_dir_, logger_);
  verify.Load();
  auto loaded = verify.GetCachedModels();
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->size(), 1u);
  EXPECT_EQ((*loaded)[0].model_id, "replacement:7");

  // Cleanup the stale temp file we planted.
  std::error_code ec;
  fs::remove(stale_temp, ec);
}
