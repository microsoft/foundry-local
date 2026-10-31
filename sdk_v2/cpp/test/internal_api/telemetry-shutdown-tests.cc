// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "configuration.h"
#include "manager.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_environment.h"
#include "DebugEvents.hpp"
#include "ITaskDispatcher.hpp"
#include "mat/config.h"
#include "http/HttpClientFactory.hpp"
#include "pal/PAL.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace {
namespace sdk = Microsoft::Applications::Events;
namespace pal = Microsoft::Applications::Events::PlatformAbstraction;
using namespace std::chrono_literals;

[[noreturn]] void Fail(int code, const char* message) {
  std::fprintf(stderr, "FAIL: %s\n", message);
  std::_Exit(code);
}

struct State {
  std::mutex mutex;
  std::condition_variable changed;
  sdk::DebugEventSource source;
  std::weak_ptr<sdk::ITaskDispatcher> worker;
  std::thread::id construction_thread = std::this_thread::get_id();
  void* worker_address = nullptr;
  bool owner_ready = false;
  bool no_worker_expected = false;
  bool mutex_before_owner = false;
  bool mutex_retired = false;
  bool callback_started = false;
  bool callback_completed = false;
  bool release_callback = false;
  bool worker_joined = false;
  bool essential_only = false;
  bool late_create_expected = false;
  bool late_create_rejected = false;
  bool upload_at_exit = false;
  bool encode_started = false;
  bool version_destroyed = false;
  int version_reads = 0;
  int uploads_completed = 0;
  int workers_started = 0;
  int collector_events = 0;
  int process_info_events = 0;

  ~State() {
    if (no_worker_expected) {
      if (workers_started != 0 || collector_events != 0) {
        Fail(32, "priming or suppression started a worker or emitted a collector event");
      }
    } else if (!mutex_before_owner || !mutex_retired || !worker_joined || !worker.expired() ||
               (!upload_at_exit && (!callback_started || !callback_completed))) {
      Fail(32, "owner, native SDK worker, and mutex teardown were not fully exercised");
    }
    if (upload_at_exit && (!encode_started || uploads_completed == 0 || version_reads < 2)) {
      Fail(32, "shutdown did not exercise production decoration, encoding, and upload completion");
    }
    if (essential_only && (process_info_events == 0 || process_info_events != collector_events)) {
      Fail(32, "runtime opt-out did not preserve ProcessInfo-only telemetry");
    }
    if (late_create_expected && !late_create_rejected) {
      Fail(32, "final owner destruction did not reject late creation");
    }
    std::fputs("PASS: actual Manager shutdown preserved SDK dependencies\n", stderr);
  }
};

State& GetState() {
  // This is constructed by the first SDK lock hook, before the SDK mutex, including before main.
  static State state;
  return state;
}

struct MutexLifetime {
  MutexLifetime() {
    auto& state = GetState();
    std::lock_guard lock(state.mutex);
    state.mutex_before_owner = !state.owner_ready;
  }

  ~MutexLifetime() {
    auto& state = GetState();
    std::lock_guard lock(state.mutex);
    // Registered immediately after the real mutex; fail before libc++ destroys it.
    if (!state.no_worker_expected &&
        ((!state.upload_at_exit && !state.callback_completed) || !state.worker_joined || !state.worker.expired())) {
      Fail(21, "SDK mutex retired before Manager joined the worker");
    }
    state.mutex_retired = true;
  }
};

class DiagnosticTask final : public sdk::Task {
 public:
  DiagnosticTask() {
    Type = Call;
    TargetTime = 0;
    TypeName = "TelemetryShutdownRegression";
  }

  void operator()() override {
    auto& state = GetState();
    if (std::this_thread::get_id() == state.construction_thread) {
      Fail(32, "diagnostic task did not run on an SDK worker");
    }
    state.source.DispatchEvent({});
    {
      std::unique_lock lock(state.mutex);
      state.callback_started = true;
      state.changed.notify_all();
      if (!state.changed.wait_for(lock, 10s, [&] { return state.release_callback; })) {
        Fail(32, "process exit did not release the SDK diagnostic task");
      }
    }
    state.source.DispatchEvent({});
    std::lock_guard lock(state.mutex);
    state.callback_completed = true;
  }
};

void ReleaseCallback() {
  auto& state = GetState();
  std::lock_guard lock(state.mutex);
  state.release_callback = true;
  state.changed.notify_all();
}

void CreateManager() {
  auto& state = GetState();
  {
    std::lock_guard lock(state.mutex);
    state.construction_thread = std::this_thread::get_id();
  }
  fl::Configuration config;
  config.app_name = "telemetry-shutdown-regression";
  const auto home = fl::TelemetryEnvironment::GetEnv("TELEMETRY_TEST_HOME");
  config.app_data_dir = home;
  config.model_cache_dir = (std::filesystem::path(home) / "models").string();
  config.logs_dir = (std::filesystem::path(home) / "logs").string();
  fl::Manager::Create(config);
  {
    std::lock_guard lock(state.mutex);
    state.owner_ready = true;
    state.changed.notify_all();
  }
}

void AttemptLateCreate() {
  try {
    CreateManager();
  } catch (const std::exception& error) {
    if (std::string_view{error.what()}.find("Manager cannot be created during process shutdown.") ==
        std::string_view::npos) {
      Fail(32, "late creation failed for a reason other than final owner destruction");
    }
    GetState().late_create_rejected = true;
    return;
  }
  Fail(32, "late creation succeeded after final owner destruction");
}

class LocalHttpClient final : public sdk::IHttpClient {
 public:
  sdk::IHttpRequest* CreateRequest() override { return new sdk::SimpleHttpRequest(pal::generateUuidString()); }

  void SendRequestAsync(sdk::IHttpRequest* request, sdk::IHttpResponseCallback* callback) override {
    if (request->GetHeaders().get("SDK-Version").empty() || request->GetBody().empty()) {
      Fail(32, "upload did not contain an encoded event and SDK version");
    }
    auto* response = new sdk::SimpleHttpResponse(request->GetId());
    response->m_result = sdk::HttpResult_OK;
    response->m_statusCode = 200;
    {
      auto& state = GetState();
      std::lock_guard lock(state.mutex);
      ++state.uploads_completed;
    }
    callback->OnHttpResponse(response);
  }

  void CancelRequestAsync(const std::string&) override {}
};
}  // namespace

namespace Microsoft::Applications::Events {
// Replace only transport: production Foundry events still traverse the pinned SDK's whole upload pipeline.
std::shared_ptr<IHttpClient> HttpClientFactory::Create() { return std::make_shared<LocalHttpClient>(); }
}  // namespace Microsoft::Applications::Events

extern "C" void TelemetryTestVersionDestroyed() {
  auto& state = GetState();
  std::lock_guard lock(state.mutex);
  state.version_destroyed = true;
}

extern "C" void TelemetryTestVersionAccess() {
  auto& state = GetState();
  std::lock_guard lock(state.mutex);
  if (state.version_destroyed) Fail(21, "SDK read its version after string destruction");
  ++state.version_reads;
}

extern "C" void TelemetryTestBeforeEncode() {
  auto& state = GetState();
  std::unique_lock lock(state.mutex);
  if (!state.upload_at_exit) return;
  state.encode_started = true;
  state.changed.notify_all();
  if (!state.changed.wait_for(lock, 30s, [&] { return state.release_callback; })) {
    Fail(32, "process exit did not release production upload encoding");
  }
}

extern "C" void TelemetryTestBeforeDebugLock() {
  auto& state = GetState();
  std::unique_lock lock(state.mutex);
  if (std::this_thread::get_id() != state.construction_thread &&
      !state.changed.wait_for(lock, 10s, [&] { return state.owner_ready; })) {
    Fail(32, "initialization depended on a background diagnostic");
  }
  if (state.mutex_retired) {
    Fail(21, "SDK accessed its diagnostic mutex after retirement");
  }
}

extern "C" void TelemetryTestDebugLockInitialized() {
  static MutexLifetime lifetime;
}

extern "C" void TelemetryTestWorkerStarted(void*) {
  auto& state = GetState();
  std::lock_guard lock(state.mutex);
  ++state.workers_started;
}

extern "C" void TelemetryTestWorkerJoined(void* address) {
  auto& state = GetState();
  std::lock_guard lock(state.mutex);
  if (address == state.worker_address) state.worker_joined = true;
}

extern "C" void TelemetryTestCollectorEvent(const char* name) {
  // Observe without intercepting normal processing; LocalHttpClient prevents external transmission.
  auto& state = GetState();
  std::lock_guard lock(state.mutex);
  ++state.collector_events;
  if (std::string_view{name}.find("ProcessInfo") != std::string_view::npos) ++state.process_info_events;
}

int main(int argc, char* argv[]) {
  if (argc != 2 || fl::TelemetryEnvironment::GetEnv("TELEMETRY_TEST_HOME").empty()) {
    Fail(32, "expected a mode and isolated test home");
  }
  const std::string_view mode{argv[1]};
  if (mode != "retained" && mode != "threaded" && mode != "recreated" && mode != "explicit" &&
      mode != "prime-only" && mode != "optout" && mode != "ci-disabled" &&
      mode != "enabled-after-ci" && mode != "late-create" && mode != "upload-after-ci") {
    Fail(32, "unknown shutdown regression mode");
  }
  auto& state = GetState();
  state.no_worker_expected = mode == "prime-only" || mode == "ci-disabled";
  state.essential_only = mode == "optout";
  state.late_create_expected = mode == "late-create";
  state.upload_at_exit = mode == "upload-after-ci";
  if (mode == "prime-only") return 0;
  if (state.late_create_expected && std::atexit(AttemptLateCreate) != 0) {
    Fail(32, "cannot register late creation check");
  }
  if (mode == "threaded") {
    std::thread initializer(CreateManager);
    initializer.join();
  } else {
    CreateManager();
  }
  if (state.no_worker_expected) return 0;

  if (mode == "enabled-after-ci" || state.upload_at_exit) {
    if (state.workers_started != 0 || state.collector_events != 0) {
      Fail(32, "CI-suppressed first creation started a worker or emitted a collector event");
    }
    fl::Manager::Destroy();
    // Register before the first real event initializes the SDK version, but after the first Manager owner.
    // The unpatched SDK destroys its version before this callback releases the blocked upload.
    if (state.upload_at_exit && std::atexit(ReleaseCallback) != 0) {
      Fail(32, "cannot register production upload release");
    }
#ifdef _WIN32
    if (_putenv_s("CI", "0") != 0) Fail(32, "cannot clear CI suppression");
#else
    if (setenv("CI", "0", 1) != 0) Fail(32, "cannot clear CI suppression");
#endif
    CreateManager();
    if (state.workers_started == 0 || state.collector_events == 0) {
      Fail(32, "recreation did not enable telemetry after CI suppression");
    }
  }

  if (state.upload_at_exit) {
    auto dispatcher = pal::getDefaultTaskDispatcher();
    if (!dispatcher) Fail(32, "actual SDK upload dispatcher unavailable");
    {
      std::lock_guard lock(state.mutex);
      state.worker = dispatcher;
      state.worker_address = dispatcher.get();
    }
    dispatcher.reset();
    std::unique_lock lock(state.mutex);
    if (!state.changed.wait_for(lock, 30s, [&] { return state.encode_started; })) {
      Fail(32, "production event never reached upload encoding");
    }
    return 0;
  }

  if (mode == "recreated") {
    auto previous_worker = pal::getDefaultTaskDispatcher();
    std::weak_ptr<sdk::ITaskDispatcher> previous = previous_worker;
    previous_worker.reset();
    fl::Manager::Destroy();
    if (!previous.expired()) Fail(32, "ordinary Destroy retained the SDK worker");
    CreateManager();
  }

  {
    std::lock_guard lock(state.mutex);
    if (state.workers_started == 0 || state.collector_events == 0) {
      Fail(32, "enabled case did not initialize telemetry (check suppression)");
    }
  }

  if (std::atexit(ReleaseCallback) != 0) Fail(32, "cannot register task release");
  {
    auto dispatcher = pal::getDefaultTaskDispatcher();
    if (!dispatcher) Fail(32, "actual SDK dispatcher unavailable");
    {
      std::lock_guard lock(state.mutex);
      state.worker = dispatcher;
      state.worker_address = dispatcher.get();
    }
    dispatcher->Queue(new DiagnosticTask());
  }
  {
    std::unique_lock lock(state.mutex);
    if (!state.changed.wait_for(lock, 10s, [&] { return state.callback_started; })) {
      Fail(32, "SDK task never started");
    }
  }
  if (mode == "explicit") {
    ReleaseCallback();
    fl::Manager::Destroy();
    if (!state.worker.expired()) Fail(32, "explicit Destroy retained the SDK worker");
  }
  return 0;
}
