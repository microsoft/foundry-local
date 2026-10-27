// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "configuration.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session.h"
#include "inferencing/session/session_manager.h"
#include "items/text_item.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "internal_api/web_service_test_helpers.h"
#include "manager.h"
#include "model.h"
#include "service/web_service.h"
#include "util/scope_guard.h"
#include "utils.h"
#include "utils/safe_getenv.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

int CudaPrimaryContextActive() {
#ifdef _WIN32
  auto driver = LoadLibraryW(L"nvcuda.dll");
  if (!driver) {
    throw std::runtime_error("Unable to load the CUDA driver for context measurements");
  }
  auto cleanup = fl::ScopeGuard([&]() noexcept { FreeLibrary(driver); });
  const auto symbol = [&](const char* name) { return GetProcAddress(driver, name); };
  using Initialize = int(WINAPI*)(unsigned int);
  using GetDevice = int(WINAPI*)(int*, int);
  using GetState = int(WINAPI*)(int, unsigned int*, int*);
#else
  auto driver = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!driver) {
    throw std::runtime_error("Unable to load the CUDA driver for context measurements");
  }
  auto cleanup = fl::ScopeGuard([&]() noexcept { dlclose(driver); });
  const auto symbol = [&](const char* name) { return dlsym(driver, name); };
  using Initialize = int (*)(unsigned int);
  using GetDevice = int (*)(int*, int);
  using GetState = int (*)(int, unsigned int*, int*);
#endif
  const auto initialize = reinterpret_cast<Initialize>(symbol("cuInit"));
  const auto get_device = reinterpret_cast<GetDevice>(symbol("cuDeviceGet"));
  const auto get_state = reinterpret_cast<GetState>(symbol("cuDevicePrimaryCtxGetState"));
  int device = 0;
  int active = 0;
  unsigned int flags = 0;
  if (!initialize || !get_device || !get_state || initialize(0) != 0 || get_device(&device, 0) != 0 ||
      get_state(device, &flags, &active) != 0) {
    throw std::runtime_error("Unable to query the CUDA primary context state");
  }
  return active;
}

class GpuIdleUnloadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = fl::test::SafeGetEnv("FOUNDRY_TEST_GPU_MODEL_PATH");
    if (path_.empty()) {
      GTEST_SKIP() << "Set FOUNDRY_TEST_GPU_MODEL_PATH to a cached CUDA chat model directory";
    }

#ifdef _WIN32
    const auto cuda_runtime_dir = fl::test::SafeGetEnv("FOUNDRY_TEST_CUDA_RUNTIME_DIR");
    if (!cuda_runtime_dir.empty()) {
      cuda_directory_ = AddDllDirectory(std::filesystem::path(cuda_runtime_dir).c_str());
      ASSERT_NE(cuda_directory_, nullptr);
    }
#endif

    fl::Configuration config;
    config.app_name = "foundry-gpu-idle-test";
    config.app_data_dir = fl::Utils::GetDefaultAppDataDir("foundry-gpu-idle-test");
    config.model_cache_dir = temp_.string();
    config.logs_dir = (temp_.path() / "logs").string();
    config.disable_nonessential_telemetry = true;
    config.additional_options["GpuIdleTimeoutSeconds"] = "5";
    manager_ = &fl::Manager::Create(config);

    const std::vector<std::string> eps{"CUDAExecutionProvider"};
    const auto registration = manager_->DownloadAndRegisterEps(&eps, {});
    ASSERT_TRUE(registration.success);

    auto info = fl::test::MakeTestModelInfo("gpu-idle-test", "test");
    info.model_id = "gpu-idle-test";
    info.device_type = fl::DeviceType::kGPU;
    info.execution_provider = "CUDAExecutionProvider";
    info.task = "chat-completion";
    catalog_.AddModel(fl::Model::FromModelInfo(std::move(info), path_, manager_->GetDownloadManager(),
                                               manager_->GetModelLoadManager()));
    model_ = catalog_.GetModelVariant("gpu-idle-test");
    ASSERT_NE(model_, nullptr);
  }

  void TearDown() override {
    if (manager_) {
      fl::Manager::Destroy();
      manager_ = nullptr;
    }
#ifdef _WIN32
    if (cuda_directory_) {
      RemoveDllDirectory(cuda_directory_);
      cuda_directory_ = nullptr;
    }
#endif
  }

  static void ProbePhase(const char* phase) {
    if (fl::test::SafeGetEnv("FOUNDRY_TEST_GPU_PROBE") == "1") {
      std::cout << "GPU_IDLE_CONTEXT " << phase << " " << CudaPrimaryContextActive() << std::endl;
      std::cout << "GPU_IDLE_PHASE " << phase << std::endl;
      std::string acknowledgement;
      if (!std::getline(std::cin, acknowledgement)) {
        throw std::runtime_error("GPU probe controller disconnected");
      }
    }
  }

  fl::test::TempPath temp_ = fl::test::TempPath::CreateTempDir("fl_gpu_idle_");
  fl::Manager* manager_ = nullptr;
  fl::test::MockCatalog catalog_;
  std::string path_;
  fl::Model* model_ = nullptr;
#ifdef _WIN32
  DLL_DIRECTORY_COOKIE cuda_directory_ = nullptr;
#endif
};

TEST_F(GpuIdleUnloadTest, DisabledTimeoutAndRecentActivityDoNotUnload) {
  model_->Load(fl::ExecutionProvider::kCUDA);
  auto& models = manager_->GetModelLoadManager();
  auto& sessions = manager_->GetSessionManager();
  const auto future = std::chrono::steady_clock::now() + std::chrono::hours(3);
  EXPECT_EQ(models.UnloadIdleModels(std::chrono::seconds(0), sessions, future), 0u);
  EXPECT_EQ(models.UnloadIdleModels(std::chrono::hours(2), sessions), 0u);
  EXPECT_TRUE(model_->IsLoaded());
}

TEST_F(GpuIdleUnloadTest, SessionConstructionLeaseAndLiveSdkSessionPreventUnload) {
  model_->Load(fl::ExecutionProvider::kCUDA);
  auto& models = manager_->GetModelLoadManager();
  auto& sessions = manager_->GetSessionManager();
  const auto future = std::chrono::steady_clock::now() + std::chrono::hours(3);
  auto lease = model_->AcquireForSession();
  EXPECT_EQ(models.UnloadIdleModels(std::chrono::seconds(1), sessions, future), 0u);
  lease.reset();

  auto session = fl::Session::Create(*model_);
  EXPECT_EQ(models.UnloadIdleModels(std::chrono::seconds(1), sessions, future), 0u);
  session.reset();
  EXPECT_EQ(models.UnloadIdleModels(std::chrono::seconds(1), sessions, future), 1u);
  EXPECT_FALSE(model_->IsLoaded());
}

TEST_F(GpuIdleUnloadTest, IdleEvictionReloadsWithOriginalProviderAndExplicitUnloadCancelsReload) {
  model_->Load(fl::ExecutionProvider::kCUDA);
  auto& models = manager_->GetModelLoadManager();
  EXPECT_EQ(models.UnloadIdleModels(std::chrono::seconds(1), manager_->GetSessionManager(),
                                    std::chrono::steady_clock::now() + std::chrono::hours(3)),
            1u);
  EXPECT_FALSE(model_->IsLoaded());

  auto lease = model_->AcquireForSession();
  ASSERT_NE(lease, nullptr);
  EXPECT_EQ(lease->EP(), fl::ExecutionProvider::kCUDA);
  lease.reset();
  EXPECT_TRUE(model_->IsLoaded());

  EXPECT_EQ(models.UnloadIdleModels(std::chrono::seconds(1), manager_->GetSessionManager(),
                                    std::chrono::steady_clock::now() + std::chrono::hours(3)),
            1u);
  model_->Unload();
  EXPECT_EQ(model_->AcquireForSession(), nullptr);
}

TEST_F(GpuIdleUnloadTest, ActiveRequestSurvivesTimeoutAndCompletionRestartsIdleWindow) {
  model_->Load(fl::ExecutionProvider::kCUDA);
  auto session = fl::Session::Create(*model_);
  auto* loaded = manager_->GetModelLoadManager().GetLoadedModel(model_->Id());
  ASSERT_NE(loaded, nullptr);
  const auto before = loaded->LastActivity();

  std::promise<void> started;
  auto started_future = started.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::atomic<bool> announced{false};
  session->SetStreamingCallback([&](flStreamingCallbackData, void*) {
    if (!announced.exchange(true)) {
      started.set_value();
      release_future.wait();
    }
    return 0;
  });

  fl::Request request;
  request.AddOwnedItem(std::make_unique<fl::TextItem>(
      R"({"messages":[{"role":"user","content":"Say hello."}],"max_tokens":2})",
      FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  auto processing = std::async(std::launch::async, [&] {
    fl::Response response;
    session->ProcessRequest(request, response);
  });
  auto unblock = fl::ScopeGuard([&]() noexcept { release.set_value(); });
  ASSERT_EQ(started_future.wait_for(std::chrono::seconds(30)), std::future_status::ready);
  std::this_thread::sleep_for(std::chrono::seconds(6));
  EXPECT_TRUE(model_->IsLoaded());
  release.set_value();
  unblock.Dismiss();
  processing.get();

  const auto completed = loaded->LastActivity();
  EXPECT_GT(completed, before);
  session.reset();
  auto& models = manager_->GetModelLoadManager();
  auto& sessions = manager_->GetSessionManager();
  EXPECT_EQ(models.UnloadIdleModels(std::chrono::seconds(5), sessions, completed + std::chrono::seconds(4)), 0u);
  EXPECT_EQ(models.UnloadIdleModels(std::chrono::seconds(5), sessions, completed + std::chrono::seconds(5)), 1u);
}

TEST_F(GpuIdleUnloadTest, AutomaticUnloadPreservesHttpAndResponseHistoryAndReloadsOnInference) {
  fl::test::MockCatalog local_catalog;
  auto& models = manager_->GetModelLoadManager();
  const auto cpu_path = fl::test::GetTestDataPath("tiny-random-gpt2-fp32-1").string();
  ASSERT_EQ(models.LoadModel(cpu_path, "idle-cpu", fl::ExecutionProvider::kCPU).status,
            fl::ModelLoadManager::LoadStatus::kSuccess);

  fl::WebService web(catalog_, local_catalog, manager_->GetLogger(), temp_.string(), models,
                     manager_->GetSessionManager(), manager_->GetTelemetry(), [] {});
  const auto urls = web.Start({"http://127.0.0.1:0"});
  auto stop = fl::ScopeGuard([&]() noexcept { web.Stop(); });
  ASSERT_FALSE(urls.empty());
  httplib::Client client(urls.front());
  client.set_read_timeout(60);

  ProbePhase("baseline");
  model_->Load(fl::ExecutionProvider::kCUDA);
  auto first = client.Post("/v1/responses",
                           R"({"model":"gpu-idle-test","input":"Say hello.","max_output_tokens":8,"store":true})",
                           "application/json");
  ASSERT_TRUE(first);
  ASSERT_EQ(first->status, 200) << first->body;
  const auto id = nlohmann::json::parse(first->body).at("id").get<std::string>();
  ASSERT_GT(manager_->GetSessionManager().CacheSize(), 0u);
  {
    auto sample = model_->AcquireForSession();
    ProbePhase("loaded");
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (model_->IsLoaded() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ASSERT_FALSE(model_->IsLoaded());
  EXPECT_EQ(manager_->GetSessionManager().CacheSize(), 0u);
  EXPECT_NE(models.GetLoadedModel("idle-cpu"), nullptr);
  ProbePhase("idle");

  const auto listing = client.Get("/v1/models");
  ASSERT_TRUE(listing);
  EXPECT_EQ(listing->status, 200);
  const auto stored = client.Get("/v1/responses/" + id);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->status, 200);

  const nlohmann::json continuation = {
      {"model", "gpu-idle-test"}, {"input", "Say goodbye."}, {"previous_response_id", id}, {"max_output_tokens", 8}, {"store", false}};
  const auto second = client.Post("/v1/responses", continuation.dump(), "application/json");
  ASSERT_TRUE(second);
  ASSERT_EQ(second->status, 200) << second->body;
  EXPECT_TRUE(model_->IsLoaded());
  {
    auto sample = model_->AcquireForSession();
    ProbePhase("reloaded");
  }
}

}  // namespace

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
