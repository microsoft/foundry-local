// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "configuration.h"
#include "logger.h"
#include "manager.h"
#include "telemetry/one_ds_telemetry.h"
#include "telemetry/telemetry_environment.h"
#include "util/process_shutdown.h"

#include <LogManagerProvider.hpp>
#include <NullObjects.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace {
namespace sdk = Microsoft::Applications::Events;

[[noreturn]] void Fail(const char* message) {
  std::fprintf(stderr, "FAIL: %s\n", message);
  std::_Exit(32);
}

class MockLogManager final : public sdk::NullLogManager {
 public:
  explicit MockLogManager(bool pending) : callback_(CreateEventW(nullptr, TRUE, !pending, nullptr)) {
    if (callback_ == nullptr) Fail("cannot create pending uploader callback");
  }

  ~MockLogManager() override { CloseHandle(callback_); }

  sdk::status_t Flush() override {
    ++flushes;
    return sdk::STATUS_SUCCESS;
  }

  void FlushAndTeardown() override {
    if (flushes != 1) Fail("teardown did not follow exactly one Flush");
    ++teardowns;
    if (WaitForSingleObject(callback_, 2000) != WAIT_OBJECT_0) {
      Fail("teardown waited for a callback that cannot arrive during process termination");
    }
  }

  int flushes = 0;
  int teardowns = 0;

 private:
  HANDLE callback_;
};

struct State {
  HANDLE verified = nullptr;
  bool process_exit = false;
  int creations = 0;
  int releases = 0;
  sdk::ILogConfiguration* config = nullptr;
  std::unique_ptr<MockLogManager> uploader;
  fl::StderrLogger logger;
};

State* state = nullptr;

void CheckCleanup() {
  const int expected = state->process_exit ? 0 : 1;
  if (state->creations != 1 || state->uploader->flushes != expected || state->uploader->teardowns != expected ||
      state->releases != expected || (state->config != nullptr) != state->process_exit) {
    Fail("production uploader cleanup did not match the DLL lifecycle");
  }
}

struct Verification {
  ~Verification() {
    if (state == nullptr) return;
    if (fl::IsDllShutdownInProgress() != state->process_exit) {
      Fail("test did not reach the expected Windows DLL shutdown state");
    }
    CheckCleanup();
    if (!SetEvent(state->verified)) Fail("cannot signal verified DLL teardown");
    if (!state->process_exit) {
      delete state;
      state = nullptr;
    }
  }
} verification;

}  // namespace

namespace Microsoft::Applications::Events {
// Real SDK worker threads pin their DLL, so use the production bridge with a mock provider to test FreeLibrary itself.
ILogManager* LogManagerProvider::Get(ILogConfiguration& config, status_t& status) {
  if (++state->creations != 1) Fail("unexpected extra production uploader initialization");
  state->config = &config;
  status = STATUS_SUCCESS;
  return state->uploader.get();
}

ILogManager* LogManagerProvider::Get(const char*, status_t&) {
  Fail("unexpected module-name provider lookup");
}

status_t LogManagerProvider::Release(ILogConfiguration& config) {
  if (&config != state->config || state->uploader->flushes != 1 || state->uploader->teardowns != 1) {
    Fail("provider release did not follow complete uploader teardown");
  }
  ++state->releases;
  state->config = nullptr;
  return STATUS_SUCCESS;
}

status_t LogManagerProvider::Release(const char*) {
  Fail("unexpected module-name provider release");
}
}  // namespace Microsoft::Applications::Events

extern "C" __declspec(dllexport) void WindowsTelemetryShutdownTest(const char* component, const char* mode,
                                                                   HANDLE verified) {
  if (state != nullptr) Fail("fixture was initialized twice");
  state = new State;
  state->verified = verified;
  state->process_exit = std::string_view{mode} == "process-exit";
  state->uploader = std::make_unique<MockLogManager>(state->process_exit);

  const auto home = fl::TelemetryEnvironment::GetEnv("TELEMETRY_TEST_HOME");
  if (home.empty()) Fail("expected an isolated test home");
  const bool manager = std::string_view{component} == "manager";
  if (manager) {
    fl::Configuration config;
    config.app_name = "windows-telemetry-shutdown";
    config.app_data_dir = home;
    config.model_cache_dir = (std::filesystem::path(home) / "models").string();
    config.logs_dir = (std::filesystem::path(home) / "logs").string();
    fl::Manager::Create(config);
  } else {
    // Register the owner after construction initializes the SDK's static dependencies, just like Manager.
    static auto owned = std::make_unique<fl::OneDsTelemetry>("windows-telemetry-shutdown", state->logger);
    auto* telemetry = owned.get();
    if (!telemetry->IsUploadEnabled()) Fail("standalone production telemetry was not enabled");
    telemetry->RecordAction(fl::Action::kCoreInitialize, fl::ActionStatus::kSuccess, {}, 0);
    if (std::string_view{mode} == "normal-destruction") owned.reset();
  }

  if (std::string_view{mode} == "normal-destruction") {
    if (manager) fl::Manager::Destroy();
    CheckCleanup();
    if (!SetEvent(verified)) Fail("cannot signal verified ordinary destruction");
  }
}
