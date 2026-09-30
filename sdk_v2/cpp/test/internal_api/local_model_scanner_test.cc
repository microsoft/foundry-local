// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for ScanLocalModels — discovering locally cached model directories.
//
#include "catalog/local_model_scanner.h"
#include "logger.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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

  void CreateBundleDir(const std::string& relative_path,
                       const std::string& model_name,
                       const std::string& task = "text-ranking") {
    const auto root = fs::path(test_dir_) / relative_path;
    CreateFile(relative_path + "/tokenizer.json");
    CreateFile(relative_path + "/tokenizer_config.json");
    nlohmann::json components = nlohmann::json::object();
    const auto add_component = [&](const std::string& name, const std::string& role) {
      const auto relative = "components/" + name + ".onnx";
      CreateFile(relative_path + "/" + relative, "synthetic");
      components[name] = {{"role", role}, {"filename", relative}};
    };
    if (task == "text-ranking") {
      add_component("encoder", "backbone");
      add_component("state_head", "head");
      add_component("action_head", "head");
      add_component("scorer", "scorer");
    } else {
      add_component("backbone", "backbone");
      add_component("pointer_head", "head");
    }
    std::ofstream(root / "component_manifest.json")
        << nlohmann::json{{"schema_version", 1},
                          {"model_type", "synthetic"},
                          {"components", components}};
    nlohmann::json metadata = {
        {"Name", model_name},
        {"Alias", "synthetic"},
        {"Task", task},
        {"ComponentManifest", "component_manifest.json"},
        {"License", "MIT"},
        {"Provenance",
         {{"source", "synthetic"},
          {"artifact_revision", "test-artifact"},
          {"base_model", "synthetic/base"},
          {"base_revision", "test-base"}}},
        {"Provider", {{"execution_provider", "cpu"}, {"variant", "fp32"}}},
        {"Capabilities", nlohmann::json::array({"structured-input"})},
    };
    std::ofstream(root / "inference_model.json") << metadata;
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

TEST_F(LocalModelScannerTest, MultiComponentRootIsOneModelAndChildrenAreNotScanned) {
  CreateBundleDir("microsoft/clm", "clm-generic-cpu:1");
  CreateModelDir("microsoft/clm/components/accidental-leaf", "child:1");

  const auto results = ScanLocalModels(test_dir_, logger_);

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results.at("clm-generic-cpu:1"),
            (fs::path(test_dir_) / "microsoft" / "clm").string());
}

TEST_F(LocalModelScannerTest, MalformedMultiComponentPackageIsExcluded) {
  CreateBundleDir("microsoft/clm", "clm-generic-cpu:1");
  CreateFile("microsoft/clm/genai_config.json");
  fs::remove(fs::path(test_dir_) / "microsoft/clm/tokenizer.json");
  EXPECT_TRUE(ScanLocalModels(test_dir_, logger_).empty());
}

TEST_F(LocalModelScannerTest, IncompleteBundleDownloadIsExcluded) {
  CreateBundleDir("microsoft/clm", "clm-generic-cpu:1");
  CreateFile("microsoft/clm/download.tmp", "");
  EXPECT_TRUE(ScanLocalModels(test_dir_, logger_).empty());
}

TEST_F(LocalModelScannerTest, AlternateManifestNameIsExcluded) {
  CreateBundleDir("microsoft/clm", "clm-generic-cpu:1");
  auto metadata_path = fs::path(test_dir_) / "microsoft/clm/inference_model.json";
  nlohmann::json metadata;
  {
    std::ifstream input(metadata_path);
    input >> metadata;
  }
  metadata["ComponentManifest"] = "other.json";
  std::ofstream(metadata_path) << metadata;
  EXPECT_TRUE(ScanLocalModels(test_dir_, logger_).empty());
}

TEST_F(LocalModelScannerTest, ManifestFileWithoutMetadataKeyCannotFallBackToGenerative) {
  CreateBundleDir("microsoft/clm", "clm-generic-cpu:1");
  CreateFile("microsoft/clm/genai_config.json");
  auto metadata_path = fs::path(test_dir_) / "microsoft/clm/inference_model.json";
  nlohmann::json metadata;
  {
    std::ifstream input(metadata_path);
    input >> metadata;
  }
  metadata.erase("ComponentManifest");
  std::ofstream(metadata_path) << metadata;

  EXPECT_TRUE(ScanLocalModels(test_dir_, logger_).empty());
}

TEST_F(LocalModelScannerTest, NonGenerativeTaskWithoutManifestCannotFallBackToGenerative) {
  CreateBundleDir("microsoft/clm", "clm-generic-cpu:1");
  CreateFile("microsoft/clm/genai_config.json");
  auto root = fs::path(test_dir_) / "microsoft/clm";
  nlohmann::json metadata;
  {
    std::ifstream input(root / "inference_model.json");
    input >> metadata;
  }
  metadata.erase("ComponentManifest");
  fs::remove(root / "component_manifest.json");
  std::ofstream(root / "inference_model.json") << metadata;

  EXPECT_TRUE(ScanLocalModels(test_dir_, logger_).empty());
}

TEST_F(LocalModelScannerTest, TaskSpecificIncompleteLayoutIsExcluded) {
  CreateBundleDir("microsoft/clm", "clm-generic-cpu:1");
  auto manifest_path = fs::path(test_dir_) / "microsoft/clm/component_manifest.json";
  nlohmann::json manifest;
  {
    std::ifstream input(manifest_path);
    input >> manifest;
  }
  manifest["components"].erase("scorer");
  std::ofstream(manifest_path) << manifest;
  EXPECT_TRUE(ScanLocalModels(test_dir_, logger_).empty());
}

#ifndef _WIN32
TEST_F(LocalModelScannerTest, SymlinkedPackageAssetEscapeIsExcluded) {
  CreateBundleDir("microsoft/clm", "clm-generic-cpu:1");
  CreateFile("outside-tokenizer.json");
  const auto tokenizer = fs::path(test_dir_) / "microsoft/clm/tokenizer.json";
  fs::remove(tokenizer);
  fs::create_symlink(fs::path(test_dir_) / "outside-tokenizer.json", tokenizer);
  EXPECT_TRUE(ScanLocalModels(test_dir_, logger_).empty());
}
#endif

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
