// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/model_load_manager.h"

#include "exception.h"
#include "internal_api/test_model_cache.h"

#include <gtest/gtest.h>
#include <ort_genai_c.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace fl {

class ModelLoadManagerTestAccess {
 public:
  static std::shared_ptr<void> Acquire(ModelLoadManager& manager, const std::function<void()>& on_admitted = {}) {
    return manager.AcquireManagedRuntime(
        "typed", "typed", "/typed", 0, "cpu", [] { return std::make_shared<int>(42); }, on_admitted);
  }
};

}  // namespace fl

namespace {

thread_local bool fail_allocations = false;
thread_local int failed_allocations = 0;
thread_local void* watched_model = nullptr;
thread_local bool watched_model_destroyed = false;

}  // namespace

// These replacements are confined to this executable and armed only after model destruction.
void* operator new(std::size_t size) {
  if (fail_allocations) {
    ++failed_allocations;
    throw std::bad_alloc();
  }

  if (void* allocation = std::malloc(size ? size : 1)) {
    return allocation;
  }

  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* allocation) noexcept {
  if (allocation == watched_model) {
    watched_model_destroyed = true;
  }

  std::free(allocation);
}

void operator delete[](void* allocation) noexcept {
  ::operator delete(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept {
  ::operator delete(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
  ::operator delete(allocation);
}

namespace {

enum class ReleaseBehavior { kSuccess,
                             kErrorResult,
                             kException,
                             kUnknownException,
                             kMissingSymbol,
                             kLookupException };

struct ReleaseState {
  ReleaseBehavior behavior = ReleaseBehavior::kSuccess;
  int lookups = 0;
  int releases = 0;
  bool cuda_requested = false;
  bool destroyed_before_release = false;
  bool fail_warning_allocation = false;
  OgaResult* error = nullptr;
};

thread_local ReleaseState* release_state = nullptr;

OgaResult* OGA_API_CALL ReleaseDeviceResources(const char* device_type) {
  auto& state = *release_state;
  ++state.releases;
  state.cuda_requested = std::strcmp(device_type, "CUDA") == 0;
  state.destroyed_before_release = watched_model_destroyed;
  fail_allocations = state.fail_warning_allocation;
  switch (state.behavior) {
    case ReleaseBehavior::kErrorResult:
      return std::exchange(state.error, nullptr);
    case ReleaseBehavior::kException:
      throw std::exception();
    case ReleaseBehavior::kUnknownException:
      throw 42;
    default:
      return nullptr;
  }
}

void* ResolveReleaseSymbol(const char* library_name, const char* symbol_name) {
  auto& state = *release_state;
  ++state.lookups;
  EXPECT_STREQ(library_name, "onnxruntime-genai.dll");
  EXPECT_STREQ(symbol_name, "OgaReleaseDeviceResources");
  if (state.behavior == ReleaseBehavior::kMissingSymbol) {
    return nullptr;
  }
  if (state.behavior == ReleaseBehavior::kLookupException) {
    throw std::runtime_error("symbol lookup failed");
  }

  return reinterpret_cast<void*>(&ReleaseDeviceResources);
}

class CpuOnlyDetector : public fl::IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {{"CPU", {"CPUExecutionProvider"}}};
  }
};

class WarningLogger : public fl::ILogger {
 public:
  void Log(fl::LogLevel level, std::string_view message) override {
    if (level != fl::LogLevel::Warning) {
      return;
    }

    ++warnings;
    if (throw_on_warning) {
      throw std::runtime_error("logger failed");
    }

    fail_allocations = fail_on_warning_allocation;
    warning.assign(message);
  }

  int warnings = 0;
  bool throw_on_warning = false;
  bool fail_on_warning_allocation = false;
  std::string warning;
};

class DeviceResourceReleaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    release_state = &state_;
    failed_allocations = 0;
    watched_model = nullptr;
    watched_model_destroyed = false;
  }

  void TearDown() override {
    fail_allocations = false;
    manager_.reset();
    OgaDestroyResult(state_.error);
    release_state = nullptr;
    watched_model = nullptr;
  }

  void CreateManager(std::optional<std::string> ownership = "true") {
    fl::Configuration config;
    if (ownership) {
      config.additional_options["ExclusiveGenAICudaOwnership"] = *ownership;
    }

    manager_ = std::make_unique<fl::ModelLoadManager>(
        ep_, logger_, (std::numeric_limits<uint64_t>::max)(), 1, config, ResolveReleaseSymbol);
  }

  void LoadModel(const char* id = "cuda-model", const char* device_type = "CUDA") {
    const auto path = fl::test::GetTestDataPath("tiny-paged-attention");
    auto result = manager_->LoadModel(path.string(), id, fl::ExecutionProvider::kCPU);
    ASSERT_EQ(result.status, fl::ModelLoadManager::LoadStatus::kSuccess);
    // Keep real CPU OGA objects, but simulate the reported device to exercise CUDA unload without a GPU.
    const_cast<std::string&>(result.model->DeviceType()) = device_type;
    if (std::strcmp(device_type, "CUDA") == 0) {
      watched_model = result.model;
      watched_model_destroyed = false;
    }
  }

  void PrepareErrorResult() {
    OgaConfig* config = nullptr;
    const auto missing_path = fl::test::GetTestDataPath("nonexistent-release-test-model").string();
    state_.error = OgaCreateConfig(missing_path.c_str(), &config);
    OgaDestroyConfig(config);
    ASSERT_NE(state_.error, nullptr);
  }

  void ExpectSuccessfulUnload() {
    bool unloaded = false;
    bool threw = false;
    try {
      unloaded = manager_->UnloadModel("cuda-model");
    } catch (...) {
      threw = true;
    }

    fail_allocations = false;
    EXPECT_FALSE(threw);
    EXPECT_TRUE(unloaded);
    EXPECT_EQ(manager_->GetLoadedModel("cuda-model"), nullptr);
  }

  ReleaseState state_;
  CpuOnlyDetector ep_;
  WarningLogger logger_;
  std::unique_ptr<fl::ModelLoadManager> manager_;
};

TEST_F(DeviceResourceReleaseTest, DisabledByDefaultAndWithoutExplicitTruthyOptIn) {
  for (const auto& value : {std::optional<std::string>{}, std::optional<std::string>{"false"},
                            std::optional<std::string>{"0"}, std::optional<std::string>{"no"},
                            std::optional<std::string>{"invalid"}}) {
    CreateManager(value);
    LoadModel();
    ExpectSuccessfulUnload();
    EXPECT_EQ(state_.lookups, 0);
    EXPECT_EQ(state_.releases, 0);
  }
}

TEST_F(DeviceResourceReleaseTest, OptInReleasesOnlyAfterLastCudaModelIsDestroyed) {
  CreateManager("YES");
  LoadModel("first");
  LoadModel("second");
  LoadModel("cpu", "CPU");
  EXPECT_TRUE(manager_->UnloadModel("first"));
  EXPECT_EQ(state_.lookups, 0);
  EXPECT_TRUE(manager_->UnloadModel("second"));
  EXPECT_EQ(state_.releases, 1);
  EXPECT_TRUE(state_.cuda_requested);
  EXPECT_TRUE(state_.destroyed_before_release);
  EXPECT_NE(manager_->GetLoadedModel("cpu"), nullptr);
  EXPECT_TRUE(manager_->UnloadModel("cpu"));
  EXPECT_EQ(state_.releases, 1);
  EXPECT_FALSE(manager_->UnloadModel("second"));
  EXPECT_EQ(state_.releases, 1);
}

TEST_F(DeviceResourceReleaseTest, LiveSessionPreventsUnloadAndRelease) {
  CreateManager();
  LoadModel();
  auto reference = manager_->AcquireSessionModel(
      "cuda-model", fl::test::GetTestDataPath("tiny-paged-attention").string());
  ASSERT_NE(reference, nullptr);
  EXPECT_THROW(manager_->UnloadModel("cuda-model"), fl::Exception);
  EXPECT_EQ(state_.releases, 0);
  reference.reset();
  ExpectSuccessfulUnload();
  EXPECT_EQ(state_.releases, 1);
}

TEST_F(DeviceResourceReleaseTest, ShutdownUnloadAlsoHonorsOptInAndLastCudaModel) {
  CreateManager("1");
  LoadModel("first");
  LoadModel("second");
  manager_->UnloadAll();
  EXPECT_EQ(state_.releases, 1);
  EXPECT_TRUE(state_.destroyed_before_release);
  EXPECT_EQ(manager_->GetLoadedModel("first"), nullptr);
  EXPECT_EQ(manager_->GetLoadedModel("second"), nullptr);
}

TEST_F(DeviceResourceReleaseTest, TypedRuntimePreventsReleaseUntilDestroyed) {
  CreateManager();
  auto lease = fl::ModelLoadManagerTestAccess::Acquire(*manager_);
  LoadModel();
  ExpectSuccessfulUnload();
  EXPECT_EQ(state_.releases, 0);

  lease.reset();
  EXPECT_TRUE(manager_->UnloadModelArtifact("typed", "/typed"));
  LoadModel();
  ExpectSuccessfulUnload();
  EXPECT_EQ(state_.releases, 1);
}

TEST_F(DeviceResourceReleaseTest, LoadingTypedRuntimePreventsReleaseBeforeAdmissionReservation) {
  CreateManager();
  LoadModel();
  auto lease = fl::ModelLoadManagerTestAccess::Acquire(*manager_, [&] {
    ExpectSuccessfulUnload();
    EXPECT_EQ(state_.releases, 0);
  });
  EXPECT_NE(lease, nullptr);
  EXPECT_EQ(state_.releases, 0);
}

TEST_F(DeviceResourceReleaseTest, TypedLeaseRetainedAcrossShutdownPreventsRelease) {
  CreateManager();
  auto lease = fl::ModelLoadManagerTestAccess::Acquire(*manager_);
  LoadModel();
  manager_->UnloadAll();
  EXPECT_EQ(manager_->GetLoadedModel("cuda-model"), nullptr);
  EXPECT_EQ(*static_cast<int*>(lease.get()), 42);
  EXPECT_EQ(state_.releases, 0);
}

TEST_F(DeviceResourceReleaseTest, MissingOptionalSymbolDoesNotFailUnload) {
  state_.behavior = ReleaseBehavior::kMissingSymbol;
  CreateManager();
  LoadModel();
  ExpectSuccessfulUnload();
  EXPECT_EQ(state_.lookups, 1);
  EXPECT_EQ(state_.releases, 0);
  EXPECT_EQ(logger_.warnings, 0);
}

TEST_F(DeviceResourceReleaseTest, SymbolLookupFailureDoesNotFailUnload) {
  state_.behavior = ReleaseBehavior::kLookupException;
  CreateManager();
  LoadModel();
  ExpectSuccessfulUnload();
  EXPECT_EQ(state_.releases, 0);
  EXPECT_NE(logger_.warning.find("symbol lookup failed"), std::string::npos);
}

class DeviceResourceReleaseFailureTest : public DeviceResourceReleaseTest,
                                         public ::testing::WithParamInterface<ReleaseBehavior> {};

TEST_P(DeviceResourceReleaseFailureTest, ReleaseFailureDoesNotFailUnload) {
  state_.behavior = GetParam();
  CreateManager();
  LoadModel();
  if (GetParam() == ReleaseBehavior::kErrorResult) {
    PrepareErrorResult();
  }

  ExpectSuccessfulUnload();
  EXPECT_EQ(state_.releases, 1);
  EXPECT_EQ(logger_.warnings, 1);
  EXPECT_NE(logger_.warning.find("GenAI device resource release failed for CUDA:"), std::string::npos);
}

TEST_P(DeviceResourceReleaseFailureTest, WarningConstructionAllocationFailureDoesNotTerminateOrFailUnload) {
  state_.behavior = GetParam();
  CreateManager();
  LoadModel();
  if (GetParam() == ReleaseBehavior::kErrorResult) {
    PrepareErrorResult();
  }

  state_.fail_warning_allocation = true;
  ExpectSuccessfulUnload();
  EXPECT_EQ(state_.releases, 1);
  EXPECT_GT(failed_allocations, 0);
  EXPECT_EQ(logger_.warnings, 0);
}

INSTANTIATE_TEST_SUITE_P(ReleaseFailures, DeviceResourceReleaseFailureTest,
                         ::testing::Values(ReleaseBehavior::kErrorResult, ReleaseBehavior::kException,
                                           ReleaseBehavior::kUnknownException));

TEST_F(DeviceResourceReleaseTest, WarningLoggerAllocationFailureDoesNotFailUnload) {
  state_.behavior = ReleaseBehavior::kException;
  CreateManager();
  LoadModel();
  logger_.fail_on_warning_allocation = true;
  ExpectSuccessfulUnload();
  EXPECT_GT(failed_allocations, 0);
  EXPECT_EQ(logger_.warnings, 1);
}

TEST_F(DeviceResourceReleaseTest, WarningLoggerExceptionDoesNotFailUnload) {
  state_.behavior = ReleaseBehavior::kUnknownException;
  CreateManager();
  LoadModel();
  logger_.throw_on_warning = true;
  ExpectSuccessfulUnload();
  EXPECT_EQ(logger_.warnings, 1);
}

}  // namespace
