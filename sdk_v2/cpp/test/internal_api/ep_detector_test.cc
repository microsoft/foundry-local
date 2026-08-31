// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Unit tests for EpDetector download orchestration logic.
// Discovery and query paths are covered by sdk_api/ep_detection_test.cc
// (integration tests through the full Manager → EpDetector → real bootstrappers stack).
// These tests focus on behavior that can't be exercised via integration tests:
//   - Download orchestration (success, failure, partial failure)
//   - Name filtering
//   - Progress callback forwarding
//   - State mutation after registration
//   - Cancellation via callback
//
#include "ep_detection/ep_bootstrapper.h"
#include "ep_detection/ep_detector.h"
#include "ep_detection/ep_types.h"
#include "logger.h"

#include <onnxruntime_c_api.h>

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace fl;

// ========================================================================
// MockEpBootstrapper — tracks calls and optionally fails registration
// ========================================================================

class MockEpBootstrapper : public IEpBootstrapper {
 public:
  MockEpBootstrapper(std::string name, bool succeed_on_register = true)
      : name_(std::move(name)), succeed_(succeed_on_register) {}

  const std::string& Name() const override { return name_; }
  bool IsRegistered() const override { return registered_; }

  bool DownloadAndRegister(
      bool /*force*/,
      const ProgressCallback& progress_cb,
      ILogger& /*logger*/) override {
    download_called_ = true;
    if (progress_cb) {
      progress_cb(name_, 50.0f);
      progress_cb(name_, 100.0f);
    }
    if (succeed_) {
      registered_ = true;
    }
    return succeed_;
  }

  bool PrepareForModelLoad(ILogger&) override {
    prepare_called_ = true;
    return prepare_succeeds_;
  }

  bool download_called_ = false;
  bool prepare_called_ = false;
  bool prepare_succeeds_ = true;

 private:
  std::string name_;
  bool succeed_;
  bool registered_ = false;
};

// ========================================================================
// Fixture
// ========================================================================

class EpDetectorTest : public ::testing::Test {
 protected:
  StderrLogger logger_;
  const OrtApi* ort_api_ = OrtGetApiBase()->GetApi(ORT_API_VERSION);
  OrtEnv* ort_env_ = nullptr;

  void SetUp() override {
    ASSERT_NE(ort_api_, nullptr);
    OrtStatus* status = ort_api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ep_detector_test", &ort_env_);
    ASSERT_EQ(status, nullptr) << "Failed to create OrtEnv";
  }

  /// Build an EpDetector from a list of mock bootstrappers, retaining raw pointers.
  std::unique_ptr<EpDetector> MakeDetector(std::vector<MockEpBootstrapper*>& raw_ptrs,
                                           std::vector<std::pair<std::string, bool>> specs) {
    std::vector<std::unique_ptr<IEpBootstrapper>> bootstrappers;
    for (auto& [name, succeed] : specs) {
      auto mock = std::make_unique<MockEpBootstrapper>(name, succeed);
      raw_ptrs.push_back(mock.get());
      bootstrappers.push_back(std::move(mock));
    }
    return std::make_unique<EpDetector>(*ort_api_, *ort_env_, std::move(bootstrappers), logger_);
  }
};

// ========================================================================
// Download orchestration — these exercise DownloadAndRegisterEps which
// can't be called safely in integration tests (would download real binaries).
// ========================================================================

TEST_F(EpDetectorTest, GetAvailableDevices_AlwaysIncludesCpu) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true}});

  const auto& devices = detector->GetAvailableDevicesToEPs();
  ASSERT_TRUE(devices.count("CPU"));
  EXPECT_FALSE(devices.at("CPU").empty());
}

TEST_F(EpDetectorTest, PrepareForModelLoad_DelegatesToMatchingBootstrapper) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true}, {"WebGpuExecutionProvider", true}});

  EXPECT_TRUE(detector->PrepareForModelLoad("WebGpuExecutionProvider"));
  EXPECT_FALSE(mocks[0]->prepare_called_);
  EXPECT_TRUE(mocks[1]->prepare_called_);
}

TEST_F(EpDetectorTest, DownloadAll_CallsAllBootstrappers) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true},
                                       {"QNNExecutionProvider", true}});

  auto result = detector->DownloadAndRegisterEps(nullptr, nullptr);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(result.registered_eps.size(), 2u);
  EXPECT_EQ(result.registered_eps[0], "CUDAExecutionProvider");
  EXPECT_EQ(result.registered_eps[1], "QNNExecutionProvider");
  EXPECT_TRUE(result.failed_eps.empty());

  EXPECT_TRUE(mocks[0]->download_called_);
  EXPECT_TRUE(mocks[1]->download_called_);
}

TEST_F(EpDetectorTest, DownloadFiltered_CallsOnlyMatchingBootstrapper) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true},
                                       {"QNNExecutionProvider", true}});

  std::vector<std::string> names = {"CUDAExecutionProvider"};
  auto result = detector->DownloadAndRegisterEps(&names, nullptr);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(result.registered_eps.size(), 1u);
  EXPECT_EQ(result.registered_eps[0], "CUDAExecutionProvider");

  EXPECT_TRUE(mocks[0]->download_called_);
  EXPECT_FALSE(mocks[1]->download_called_);
}

TEST_F(EpDetectorTest, DownloadAll_WhenOneFailsResultHasFailedEps) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true},
                                       {"QNNExecutionProvider", false}});

  auto result = detector->DownloadAndRegisterEps(nullptr, nullptr);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.status, "Some EPs failed to register");
  ASSERT_EQ(result.registered_eps.size(), 1u);
  EXPECT_EQ(result.registered_eps[0], "CUDAExecutionProvider");
  ASSERT_EQ(result.failed_eps.size(), 1u);
  EXPECT_EQ(result.failed_eps[0], "QNNExecutionProvider");
}

TEST_F(EpDetectorTest, DownloadAll_ProgressCallbackForwarded) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true},
                                       {"QNNExecutionProvider", true}});

  std::vector<std::pair<std::string, float>> progress_events;
  auto cb = [&](const std::string& ep_name, float percent) -> bool {
    progress_events.emplace_back(ep_name, percent);
    return true;
  };

  detector->DownloadAndRegisterEps(nullptr, cb);

  // MockEpBootstrapper fires 50% and 100% for each EP.
  ASSERT_EQ(progress_events.size(), 4u);
  EXPECT_EQ(progress_events[0].first, "CUDAExecutionProvider");
  EXPECT_FLOAT_EQ(progress_events[0].second, 50.0f);
  EXPECT_EQ(progress_events[1].first, "CUDAExecutionProvider");
  EXPECT_FLOAT_EQ(progress_events[1].second, 100.0f);
  EXPECT_EQ(progress_events[2].first, "QNNExecutionProvider");
  EXPECT_FLOAT_EQ(progress_events[2].second, 50.0f);
  EXPECT_EQ(progress_events[3].first, "QNNExecutionProvider");
  EXPECT_FLOAT_EQ(progress_events[3].second, 100.0f);
}

TEST_F(EpDetectorTest, DownloadAll_DiscoverableEpsReflectRegistrationState) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true},
                                       {"QNNExecutionProvider", false}});

  detector->DownloadAndRegisterEps(nullptr, nullptr);

  const auto& eps = detector->GetDiscoverableEps();
  ASSERT_EQ(eps.size(), 2u);
  EXPECT_TRUE(eps[0].is_registered);   // CUDA succeeded
  EXPECT_FALSE(eps[1].is_registered);  // QNN failed
}

TEST_F(EpDetectorTest, DownloadFiltered_UnknownNamesSkipped) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true}});

  std::vector<std::string> names = {"CUDAExecutionProvider", "NonExistentProvider"};
  auto result = detector->DownloadAndRegisterEps(&names, nullptr);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(result.registered_eps.size(), 1u);
  EXPECT_EQ(result.registered_eps[0], "CUDAExecutionProvider");
  EXPECT_TRUE(result.failed_eps.empty());

  EXPECT_TRUE(mocks[0]->download_called_);
}

TEST_F(EpDetectorTest, DownloadFiltered_AllNamesUnknown_SucceedsWithNothing) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true}});

  std::vector<std::string> names = {"FakeProvider"};
  auto result = detector->DownloadAndRegisterEps(&names, nullptr);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.registered_eps.empty());
  EXPECT_TRUE(result.failed_eps.empty());

  EXPECT_FALSE(mocks[0]->download_called_);
}

// NvTensorRTRTX reuses the GenAI CUDA bridge shipped in the CUDA EP bundle, so requesting it by
// name must also auto-register the CUDA EP (when a CUDA bootstrapper is present).
TEST_F(EpDetectorTest, DownloadFiltered_TrtRtxAlsoRegistersCuda) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"NvTensorRTRTXExecutionProvider", true},
                                       {"CUDAExecutionProvider", true}});

  std::vector<std::string> names = {"NvTensorRTRTXExecutionProvider"};
  auto result = detector->DownloadAndRegisterEps(&names, nullptr);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(mocks[0]->download_called_);
  EXPECT_TRUE(mocks[1]->download_called_);
  EXPECT_NE(std::find(result.registered_eps.begin(), result.registered_eps.end(), "CUDAExecutionProvider"),
            result.registered_eps.end());
}

TEST_F(EpDetectorTest, DownloadFiltered_TrtRtxWithoutCudaReportsFailure) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"NvTensorRTRTXExecutionProvider", true}});

  std::vector<std::string> names = {"NvTensorRTRTXExecutionProvider"};
  auto result = detector->DownloadAndRegisterEps(&names, nullptr);

  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.registered_eps.empty());
  ASSERT_EQ(result.failed_eps.size(), 1u);
  EXPECT_EQ(result.failed_eps[0], "NvTensorRTRTXExecutionProvider");
  EXPECT_FALSE(mocks[0]->download_called_);
}

// A host with a CUDA bootstrapper but no NvTensorRTRTX bootstrapper (e.g. Linux + NVIDIA GPU) must
// preserve unknown-name behavior: requesting the unknown NvTensorRTRTX name must NOT pull in CUDA.
TEST_F(EpDetectorTest, DownloadFiltered_TrtRtxUnknownWithCudaPresent_NoInjection) {
  std::vector<MockEpBootstrapper*> mocks;
  auto detector = MakeDetector(mocks, {{"CUDAExecutionProvider", true},
                                       {"WebGpuExecutionProvider", true}});

  std::vector<std::string> names = {"NvTensorRTRTXExecutionProvider"};
  auto result = detector->DownloadAndRegisterEps(&names, nullptr);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.registered_eps.empty());
  EXPECT_FALSE(mocks[0]->download_called_);  // CUDA must not be injected
  EXPECT_FALSE(mocks[1]->download_called_);
}