// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for ScanLocalModels — discovering locally cached model directories.
//
#include "catalog/local_model_scanner.h"
#include "logger.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

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

class LocalModelScannerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = (fs::temp_directory_path() / ("fl_scan_test_" + std::to_string(GetPid()))).string();
    fs::create_directories(test_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  /// Create a valid model directory with genai_config.json and inference_model.json.
  void CreateModelDir(const std::string& relative_path,
                      const std::string& model_name) {
    auto dir = fs::path(test_dir_) / relative_path;
    fs::create_directories(dir);

    // genai_config.json — only existence matters
    {
      std::ofstream f(dir / "genai_config.json");
      f << "{}";
    }

    // inference_model.json — Name field is read
    {
      std::ofstream f(dir / "inference_model.json");
      f << R"({"Name": ")" << model_name << R"(", "PromptTemplate": null})";
    }
  }

  /// Create a file with the given content at a relative path under test_dir_.
  void CreateFile(const std::string& relative_path, const std::string& content = "{}") {
    auto path = fs::path(test_dir_) / relative_path;
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
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

TEST_F(LocalModelScannerTest, ValidModelDirectory) {
  CreateModelDir("publisher/phi-4-mini", "phi-4-mini:3");

  auto results = ScanLocalModels(test_dir_, logger_);

  ASSERT_EQ(results.size(), 1u);
  ASSERT_TRUE(results.count("phi-4-mini:3"));

  // Verify the path points to the correct directory.
  auto expected_path = (fs::path(test_dir_) / "publisher" / "phi-4-mini").string();
  EXPECT_EQ(results["phi-4-mini:3"], expected_path);
}

TEST_F(LocalModelScannerTest, VersionlessModelNameGetsZeroAppended) {
  CreateModelDir("publisher/my-model", "my-model");

  auto results = ScanLocalModels(test_dir_, logger_);

  ASSERT_EQ(results.size(), 1u);
  ASSERT_TRUE(results.count("my-model:0"));
}

TEST_F(LocalModelScannerTest, IncompleteDownloadExcluded) {
  CreateModelDir("publisher/downloading-model", "downloading-model:1");

  // Add the download signal file — marks this as incomplete.
  CreateFile("publisher/downloading-model/download.tmp", "");

  auto results = ScanLocalModels(test_dir_, logger_);
  EXPECT_TRUE(results.empty());
}

TEST_F(LocalModelScannerTest, IncompleteParentExcludesRevisionDirectories) {
  CreateModelDir("publisher/downloading-model/v3", "downloading-model:3");
  CreateModelDir("publisher/completed-model/v1", "completed-model:1");
  CreateFile("publisher/downloading-model/download.tmp", "");

  const auto results = ScanLocalModels(test_dir_, logger_);
  EXPECT_EQ(results.count("downloading-model:3"), 0u);
  EXPECT_EQ(results.count("completed-model:1"), 1u);
}

TEST_F(LocalModelScannerTest, MissingGenaiConfigExcluded) {
  auto dir = fs::path(test_dir_) / "publisher" / "no-genai";
  fs::create_directories(dir);

  // Only inference_model.json, no genai_config.json
  {
    std::ofstream f(dir / "inference_model.json");
    f << R"({"Name": "no-genai:1", "PromptTemplate": null})";
  }

  auto results = ScanLocalModels(test_dir_, logger_);
  EXPECT_TRUE(results.empty());
}

TEST_F(LocalModelScannerTest, MissingInferenceModelExcluded) {
  auto dir = fs::path(test_dir_) / "publisher" / "no-inference";
  fs::create_directories(dir);

  // Only genai_config.json, no inference_model.json
  {
    std::ofstream f(dir / "genai_config.json");
    f << "{}";
  }

  auto results = ScanLocalModels(test_dir_, logger_);
  EXPECT_TRUE(results.empty());
}

TEST_F(LocalModelScannerTest, NestedPublisherModelStructure) {
  CreateModelDir("microsoft/phi-4-mini-instruct", "phi-4-mini-instruct:2");

  auto results = ScanLocalModels(test_dir_, logger_);

  ASSERT_EQ(results.size(), 1u);
  ASSERT_TRUE(results.count("phi-4-mini-instruct:2"));
}

TEST_F(LocalModelScannerTest, MultipleModelsAllReturned) {
  CreateModelDir("microsoft/phi-4-mini", "phi-4-mini:3");
  CreateModelDir("meta/llama-3", "llama-3:1");
  CreateModelDir("google/gemma", "gemma:0");

  auto results = ScanLocalModels(test_dir_, logger_);

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results.count("phi-4-mini:3"));
  EXPECT_TRUE(results.count("llama-3:1"));
  EXPECT_TRUE(results.count("gemma:0"));
}

TEST_F(LocalModelScannerTest, EmptyDirectoryReturnsEmptyMap) {
  // test_dir_ exists but has no subdirectories with models.
  auto results = ScanLocalModels(test_dir_, logger_);
  EXPECT_TRUE(results.empty());
}

TEST_F(LocalModelScannerTest, NonExistentDirectoryReturnsEmptyMap) {
  auto results = ScanLocalModels(test_dir_ + "/does_not_exist", logger_);
  EXPECT_TRUE(results.empty());
}

TEST_F(LocalModelScannerTest, EmptyStringReturnsEmptyMap) {
  auto results = ScanLocalModels("", logger_);
  EXPECT_TRUE(results.empty());
}
