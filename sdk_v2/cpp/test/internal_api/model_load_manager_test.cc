// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/model_load_manager.h"

#include "ep_detection/ep_detector.h"
#include "exception.h"
#include "inferencing/generative/genai_model_instance.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "logger.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

/// EP detector that reports GPU EPs as available.
class GpuEpDetector : public fl::IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {
        {"CPU", {"CPUExecutionProvider"}},
        {"GPU", {"CUDAExecutionProvider"}},
    };
  }

  bool PrepareForModelLoad(std::string_view ep_name) override {
    prepared_ep = ep_name;
    return true;
  }

  std::string prepared_ep;
};

/// EP detector that reports CPU only.
class CpuOnlyDetector : public fl::IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {{"CPU", {"CPUExecutionProvider"}}};
  }

  bool PrepareForModelLoad(std::string_view ep_name) override {
    prepared_ep = ep_name;
    return true;
  }

  std::string prepared_ep;
};

/// Creates a minimal model directory with a dummy genai_config.json.
/// Cleans up on destruction.
class TempModelDir {
 public:
  explicit TempModelDir(const std::string& model_name, const std::string& provider = "")
      : root_(fl::test::TempPath::CreateTempDir("fl_model_load_" + model_name + "_")), path_(root_.string()) {
    // Write a minimal genai_config.json
    std::ofstream config(std::filesystem::path(path_) / "genai_config.json");
    if (provider.empty()) {
      config << R"({"model": {"type": "phi3"}})";
    } else {
      config << R"({"model":{"type":"phi3","decoder":{"session_options":{"provider_options":[{")"
             << provider << R"(":{}}]}}}})";
    }
  }

  TempModelDir(const TempModelDir&) = delete;
  TempModelDir& operator=(const TempModelDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  fl::test::TempPath root_;
  std::string path_;
};

}  // namespace

// ---------------------------------------------------------------------------
// EP Guard Tests
// ---------------------------------------------------------------------------

TEST(ModelLoadManagerTest, LoadWithCudaOverride_CudaNotAvailable_Throws) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("test-cpu-model");
  fl::ModelLoadManager mgr(ep, logger);

  EXPECT_THROW({ mgr.LoadModel(dir.path(), "test-cpu-model", fl::ExecutionProvider::kCUDA); }, fl::Exception);
}

TEST(ModelLoadManagerTest, LoadWithCudaOverride_CudaNotAvailable_ErrorMessage) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("test-cpu-model");
  fl::ModelLoadManager mgr(ep, logger);

  try {
    mgr.LoadModel(dir.path(), "test-cpu-model", fl::ExecutionProvider::kCUDA);
    FAIL() << "Expected exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    std::string msg = e.what();
    EXPECT_NE(msg.find("CUDAExecutionProvider"), std::string::npos);
    EXPECT_NE(msg.find("DownloadAndRegisterEps"), std::string::npos);
  }
}

TEST(ModelLoadManagerTest, LoadCudaGpuModel_CudaNotAvailable_Throws) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("phi-4-mini-cuda-gpu");
  fl::ModelLoadManager mgr(ep, logger);

  EXPECT_THROW({ mgr.LoadModel(dir.path(), "phi-4-mini-cuda-gpu"); }, fl::Exception);
}

TEST(ModelLoadManagerTest, LoadCudaGpuModel_CudaNotAvailable_ErrorMessage) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("phi-4-mini-cuda-gpu");
  fl::ModelLoadManager mgr(ep, logger);

  try {
    mgr.LoadModel(dir.path(), "phi-4-mini-cuda-gpu");
    FAIL() << "Expected exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    std::string msg = e.what();
    EXPECT_NE(msg.find("CUDAExecutionProvider"), std::string::npos);
    EXPECT_NE(msg.find("DownloadAndRegisterEps"), std::string::npos);
  }
}

TEST(ModelLoadManagerTest, LoadRuntimeIdWithCudaConfig_CudaNotAvailable_Throws) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("alias-cuda-config", "cuda");
  fl::ModelLoadManager mgr(ep, logger);

  try {
    mgr.LoadModel(dir.path(), "local/test-registration");
    FAIL() << "Expected exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_NE(std::string(e.what()).find("CUDAExecutionProvider"), std::string::npos);
  }
}

TEST(ModelLoadManagerTest, LoadRuntimeIdWithWebGpuConfig_WebGpuNotAvailable_Throws) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("alias-webgpu-config", "WebGPU");
  fl::ModelLoadManager mgr(ep, logger);

  try {
    mgr.LoadModel(dir.path(), "local/test-registration");
    FAIL() << "Expected exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_NE(std::string(e.what()).find("WebGpuExecutionProvider"), std::string::npos);
  }
}

TEST(ModelLoadManagerTest, LoadRuntimeIdWithCudaConfig_CudaAvailable_PreparesCuda) {
  GpuEpDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("alias-cuda-available", "cuda");
  fl::ModelLoadManager mgr(ep, logger);

  try {
    mgr.LoadModel(dir.path(), "local/test-registration");
  } catch (const fl::Exception& e) {
    EXPECT_NE(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  }

  EXPECT_EQ(ep.prepared_ep, "CUDAExecutionProvider");
}

TEST(ModelLoadManagerTest, LoadRuntimeIdWithCanonicalWinMlProvider_NotAvailable_Throws) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("alias-migraphx-config", "MIGraphXExecutionProvider");
  fl::ModelLoadManager mgr(ep, logger);

  try {
    mgr.LoadModel(dir.path(), "local/test-registration");
    FAIL() << "Expected exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_NE(std::string(e.what()).find("MIGraphXExecutionProvider"), std::string::npos);
  }
}

TEST(ModelLoadManagerTest, LoadRuntimeIdWithFullDmlProviderName_DoesNotRequireDownloadableEp) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("alias-dml-config", "DmlExecutionProvider");
  fl::ModelLoadManager mgr(ep, logger);

  try {
    mgr.LoadModel(dir.path(), "local/test-registration");
  } catch (const fl::Exception& e) {
    EXPECT_NE(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  }

  EXPECT_TRUE(ep.prepared_ep.empty());
}

TEST(ModelLoadManagerTest, LoadWithUnknownOverride_ThrowsInvalidArgument) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("unknown-override");
  fl::ModelLoadManager mgr(ep, logger);

  try {
    mgr.LoadModel(dir.path(), "local/test-registration", fl::ExecutionProvider::kUnknown);
    FAIL() << "Expected exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

TEST(ModelLoadManagerTest, LoadWithCpuOverride_IgnoresCudaConfigAndModelId) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("explicit-cpu", "cuda");
  fl::ModelLoadManager mgr(ep, logger);

  try {
    mgr.LoadModel(dir.path(), "local/alias-cuda-gpu:0", fl::ExecutionProvider::kCPU);
  } catch (const fl::Exception& e) {
    EXPECT_NE(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  }

  EXPECT_TRUE(ep.prepared_ep.empty());
}

TEST(ModelLoadManagerTest, LoadOpenVinoNpuModel_NotAvailable_Throws) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("phi-4-mini-openvino-npu");
  fl::ModelLoadManager mgr(ep, logger);

  EXPECT_THROW({ mgr.LoadModel(dir.path(), "phi-4-mini-openvino-npu"); }, fl::Exception);
}

TEST(ModelLoadManagerTest, LoadCpuModel_AlwaysSucceeds_NoEpGuard) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("phi-4-mini-cpu");
  fl::ModelLoadManager mgr(ep, logger);

  // This will fail at GenAIModelInstance construction (no real model), not at EP guard.
  // If it threw INVALID_USAGE about EP, the guard is wrong for CPU models.
  try {
    mgr.LoadModel(dir.path(), "phi-4-mini-cpu");
    // If it somehow succeeds (unlikely without real model), that's fine
  } catch (const fl::Exception& e) {
    // Should NOT be an EP guard error
    EXPECT_NE(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  }
}

TEST(ModelLoadManagerTest, LoadGenericGpuModel_CudaAvailable_AutoSelectsCuda) {
  GpuEpDetector ep;
  fl::StderrLogger logger;
  TempModelDir dir("phi-4-mini-generic-gpu");
  fl::ModelLoadManager mgr(ep, logger);

  // Will fail at GenAIModelInstance construction, but should NOT fail at EP guard.
  // The auto-select logic should pick CUDA, and CUDA IS available.
  try {
    mgr.LoadModel(dir.path(), "phi-4-mini-generic-gpu");
  } catch (const fl::Exception& e) {
    // Should NOT be an EP guard error
    EXPECT_NE(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  }

  EXPECT_EQ(ep.prepared_ep, "CUDAExecutionProvider");
}

// ---------------------------------------------------------------------------
// Unload-with-live-sessions tests (real model load)
// ---------------------------------------------------------------------------

class ModelLoadManagerUnloadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    model_path_ = fl::test::GetTestModelPath(fl::test::kTestChatModelAlias).string();
    ep_ = std::make_unique<fl::test::CpuOnlyEpDetector>();
    logger_ = std::make_unique<fl::StderrLogger>();
    mgr_ = std::make_unique<fl::ModelLoadManager>(*ep_, *logger_);

    auto result = mgr_->LoadModel(model_path_, fl::test::kTestChatModelAlias);
    ASSERT_EQ(result.status, fl::ModelLoadManager::LoadStatus::kSuccess)
        << "Failed to load test model from: " << model_path_;
    instance_ = result.model;
  }

  std::string model_path_;
  std::unique_ptr<fl::test::CpuOnlyEpDetector> ep_;
  std::unique_ptr<fl::StderrLogger> logger_;
  std::unique_ptr<fl::ModelLoadManager> mgr_;
  fl::GenAIModelInstance* instance_ = nullptr;
};

TEST_F(ModelLoadManagerUnloadTest, UnloadSucceedsWhenNoSessions) {
  ASSERT_EQ(instance_->SessionRefCount(), 0);
  EXPECT_TRUE(mgr_->UnloadModel(fl::test::kTestChatModelAlias));
  EXPECT_EQ(mgr_->GetLoadedModel(fl::test::kTestChatModelAlias), nullptr);
}

TEST_F(ModelLoadManagerUnloadTest, UnloadThrowsWhenSessionsLive) {
  instance_->AcquireSession();
  instance_->AcquireSession();

  EXPECT_THROW(mgr_->UnloadModel(fl::test::kTestChatModelAlias), fl::Exception);
  // Model is still loaded.
  EXPECT_EQ(mgr_->GetLoadedModel(fl::test::kTestChatModelAlias), instance_);

  instance_->ReleaseSession();
  instance_->ReleaseSession();
}

TEST_F(ModelLoadManagerUnloadTest, UnloadFailsWhenInUseAndSucceedsAfterSessionsReleased) {
  instance_->AcquireSession();
  EXPECT_THROW(mgr_->UnloadModel(fl::test::kTestChatModelAlias), fl::Exception);

  instance_->ReleaseSession();
  ASSERT_EQ(instance_->SessionRefCount(), 0);

  EXPECT_TRUE(mgr_->UnloadModel(fl::test::kTestChatModelAlias));
  EXPECT_EQ(mgr_->GetLoadedModel(fl::test::kTestChatModelAlias), nullptr);
}

TEST_F(ModelLoadManagerUnloadTest, SameIdRequiresSameCanonicalPathForLoadLookupAndUnload) {
  const auto equivalent_path = std::filesystem::path(model_path_) / ".";
  const auto different_path = std::filesystem::path(model_path_).parent_path();

  const auto idempotent = mgr_->LoadModel(equivalent_path.string(), fl::test::kTestChatModelAlias);
  EXPECT_EQ(idempotent.status, fl::ModelLoadManager::LoadStatus::kModelAlreadyLoaded);
  EXPECT_EQ(idempotent.model, instance_);

  try {
    mgr_->LoadModel(different_path.string(), fl::test::kTestChatModelAlias);
    FAIL() << "Expected a same-ID path collision";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_NE(std::string(e.what()).find("different path"), std::string::npos);
  }

  EXPECT_EQ(mgr_->GetLoadedModel(fl::test::kTestChatModelAlias, different_path.string()), nullptr);
  EXPECT_FALSE(mgr_->UnloadModel(fl::test::kTestChatModelAlias, different_path.string()));
  EXPECT_EQ(mgr_->GetLoadedModel(fl::test::kTestChatModelAlias), instance_);

  EXPECT_TRUE(mgr_->UnloadModel(fl::test::kTestChatModelAlias, equivalent_path.string()));
  EXPECT_EQ(mgr_->GetLoadedModel(fl::test::kTestChatModelAlias), nullptr);
}
