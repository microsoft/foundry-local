// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/model_load_manager.h"

#include "ep_detection/ep_detector.h"
#include "exception.h"
#include "inferencing/generative/genai_model_instance.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "logger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>

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
};

class BlockingGpuEpDetector : public fl::IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      availability_queried_ = true;
    }
    cv_.notify_all();

    return {
        {"CPU", {"CPUExecutionProvider"}},
        {"GPU", {"CUDAExecutionProvider"}},
    };
  }

  bool PrepareForModelLoad(std::string_view /*ep_name*/) override {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return released_; });
    return false;
  }

  bool WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return entered_; });
  }

  bool WaitUntilAvailabilityQueried() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return availability_queried_; });
  }

  bool PreparationEnteredWithin(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return entered_; });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }

    cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  mutable bool availability_queried_ = false;
  bool entered_ = false;
  bool released_ = false;
};

/// Creates a minimal model directory with a dummy genai_config.json.
/// Cleans up on destruction.
class TempModelDir {
 public:
  TempModelDir(const std::string& model_name) {
    path_ = (std::filesystem::temp_directory_path() / ("fl_test_" + model_name)).string();
    std::filesystem::create_directories(path_);

    // Write a minimal genai_config.json
    std::ofstream config(std::filesystem::path(path_) / "genai_config.json");
    config << R"({"model": {"type": "phi3"}})";
  }

  ~TempModelDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempModelDir(const TempModelDir&) = delete;
  TempModelDir& operator=(const TempModelDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace

// ---------------------------------------------------------------------------
// EP Guard Tests
// ---------------------------------------------------------------------------

TEST(ModelLoadManagerTest, LoadWithCudaOverride_CudaNotAvailable_Throws) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  fl::ModelLoadManager mgr(ep, logger);

  TempModelDir dir("test-cpu-model");

  EXPECT_THROW({ mgr.LoadModel(dir.path(), "test-cpu-model", fl::ExecutionProvider::kCUDA); }, fl::Exception);
}

TEST(ModelLoadManagerTest, LoadWithCudaOverride_CudaNotAvailable_ErrorMessage) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  fl::ModelLoadManager mgr(ep, logger);

  TempModelDir dir("test-cpu-model");

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
  fl::ModelLoadManager mgr(ep, logger);

  TempModelDir dir("phi-4-mini-cuda-gpu");

  EXPECT_THROW({ mgr.LoadModel(dir.path(), "phi-4-mini-cuda-gpu"); }, fl::Exception);
}

TEST(ModelLoadManagerTest, LoadCudaGpuModel_CudaNotAvailable_ErrorMessage) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  fl::ModelLoadManager mgr(ep, logger);

  TempModelDir dir("phi-4-mini-cuda-gpu");

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

TEST(ModelLoadManagerTest, LoadOpenVinoNpuModel_NotAvailable_Throws) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  fl::ModelLoadManager mgr(ep, logger);

  TempModelDir dir("phi-4-mini-openvino-npu");

  EXPECT_THROW({ mgr.LoadModel(dir.path(), "phi-4-mini-openvino-npu"); }, fl::Exception);
}

TEST(ModelLoadManagerTest, LoadCpuModel_AlwaysSucceeds_NoEpGuard) {
  CpuOnlyDetector ep;
  fl::StderrLogger logger;
  fl::ModelLoadManager mgr(ep, logger);

  TempModelDir dir("phi-4-mini-cpu");

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
  fl::ModelLoadManager mgr(ep, logger);

  TempModelDir dir("phi-4-mini-generic-gpu");

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

TEST(ModelLoadManagerTest, CloseLoadAdmissionReturnsBeforeAdmittedLoadDrains) {
  BlockingGpuEpDetector ep;
  fl::StderrLogger logger;
  fl::ModelLoadManager manager(ep, logger);
  TempModelDir dir("nonblocking-load-closure");

  std::exception_ptr load_error;
  std::thread loader([&] {
    try {
      static_cast<void>(manager.LoadModel(dir.path(), "nonblocking-load-closure",
                                          fl::ExecutionProvider::kCUDA));
    } catch (...) {
      load_error = std::current_exception();
    }
  });

  const bool load_entered = ep.WaitUntilEntered();
  if (!load_entered) {
    ep.Release();
    loader.join();
    FAIL() << "admitted load did not reach EP preparation";
    return;
  }

  std::promise<void> close_returned;
  auto close_future = close_returned.get_future();
  std::thread closer([&] {
    manager.CloseLoadAdmission();
    close_returned.set_value();
  });

  const bool returned_without_drain =
      close_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready;

  ep.Release();
  closer.join();
  loader.join();
  manager.DrainModelLifecycleWork();

  EXPECT_TRUE(returned_without_drain);
  EXPECT_TRUE(load_error != nullptr);
  EXPECT_THROW(
      static_cast<void>(manager.LoadModel(dir.path(), "rejected-after-closure",
                                          fl::ExecutionProvider::kCUDA)),
      fl::Exception);
}

#ifdef _WIN32
TEST(ModelLoadManagerTest, EpPreparationAndModelConstructionAreProcessSerializedOnWindows) {
  BlockingGpuEpDetector first_ep;
  BlockingGpuEpDetector second_ep;
  fl::StderrLogger logger;
  fl::ModelLoadManager first_manager(first_ep, logger);
  fl::ModelLoadManager second_manager(second_ep, logger);
  TempModelDir first_dir("serialized-ep-first");
  TempModelDir second_dir("serialized-ep-second");

  std::thread first_loader([&] {
    try {
      static_cast<void>(first_manager.LoadModel(first_dir.path(), "serialized-ep-first",
                                                fl::ExecutionProvider::kCUDA));
    } catch (...) {
    }
  });

  const bool first_entered = first_ep.WaitUntilEntered();
  if (!first_entered) {
    first_ep.Release();
    first_loader.join();
    FAIL() << "first load did not reach EP preparation";
    return;
  }

  std::thread second_loader([&] {
    try {
      static_cast<void>(second_manager.LoadModel(second_dir.path(), "serialized-ep-second",
                                                 fl::ExecutionProvider::kCUDA));
    } catch (...) {
    }
  });

  const bool second_reached_preparation_boundary = second_ep.WaitUntilAvailabilityQueried();
  const bool second_entered_while_first_held_lock =
      second_ep.PreparationEnteredWithin(std::chrono::milliseconds(250));

  first_ep.Release();
  first_loader.join();

  const bool second_eventually_entered = second_ep.WaitUntilEntered();
  second_ep.Release();
  second_loader.join();

  ASSERT_TRUE(second_reached_preparation_boundary);
  EXPECT_FALSE(second_entered_while_first_held_lock);
  EXPECT_TRUE(second_eventually_entered);
}
#endif

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
