// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "manager.h"

#include <fmt/format.h>
#include <onnxruntime_c_api.h>
#include <ort_genai_c.h>

#include <atomic>
#include <set>
#include <string_view>

#include "catalog.h"
#include "catalog/azure_model_catalog.h"
#include "download/download_manager.h"
#include "ep_detection/cuda_ep_bootstrapper.h"
#include "ep_detection/ep_detector.h"
#include "ep_detection/ep_types.h"
#include "ep_detection/runtime_version_info.h"
#include "ep_detection/webgpu_ep_bootstrapper.h"
#include "exception.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session_manager.h"
#include "spdlog_logger.h"
#include "telemetry/telemetry_action_tracker.h"
#include "telemetry/telemetry_logger.h"
#include "util/string_utils.h"
#include "utils.h"

#if FOUNDRY_LOCAL_HAS_EP_CATALOG
#include "ep_detection/winml_ep_bootstrapper.h"
#endif

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include "service/web_service.h"
#endif

#if defined(__ANDROID__) && !defined(NDEBUG)
#include "platform/android/ssl_cert_checker.h"
#endif

namespace fl {

namespace {

std::atomic<ILogger*> s_ort_logger{nullptr};
std::atomic<ILogger*> s_oga_logger{nullptr};

bool IsTruthyConfigValue(const std::string& value) {
  const auto lowered = ToLower(value);
  return lowered == "true" || lowered == "1" || lowered == "yes";
}

bool IsGenAIVerboseLoggingEnabled() {
  auto env = Utils::GetEnv("ORTGENAI_ORT_VERBOSE_LOGGING");
  if (!env.has_value()) {
    return false;
  }

  return IsTruthyConfigValue(*env);
}

bool IsAdditionalOptionEnabled(const Configuration& config, const std::string& option_name) {
  const auto it = config.additional_options.find(option_name);
  return it != config.additional_options.cend() && IsTruthyConfigValue(it->second);
}

OrtLoggingLevel GetDefaultOrtLoggingLevel(bool genai_verbose_logging_enabled) {
  // If someone explicitly enables ORTGENAI_ORT_VERBOSE_LOGGING, treat this as
  // a debug scenario and default ORT logging to verbose as well.
  return genai_verbose_logging_enabled ? ORT_LOGGING_LEVEL_VERBOSE : ORT_LOGGING_LEVEL_ERROR;
}

LogLevel MapOrtLogLevel(OrtLoggingLevel severity) {
  switch (severity) {
    case ORT_LOGGING_LEVEL_VERBOSE:
      return LogLevel::Verbose;
    case ORT_LOGGING_LEVEL_INFO:
      return LogLevel::Information;
    case ORT_LOGGING_LEVEL_WARNING:
      return LogLevel::Warning;
    case ORT_LOGGING_LEVEL_ERROR:
      return LogLevel::Error;
    case ORT_LOGGING_LEVEL_FATAL:
      return LogLevel::Fatal;
    default:
      return LogLevel::Information;
  }
}

void ORT_API_CALL OrtLogCallback(void* /*logger_param*/,
                                 OrtLoggingLevel severity,
                                 const char* category,
                                 const char* logid,
                                 const char* code_location,
                                 const char* message) {
  auto* logger = s_ort_logger.load(std::memory_order_acquire);
  if (logger == nullptr) {
    return;
  }

  try {
    std::string payload = "ORT";
    if (category && category[0] != '\0') {
      payload += " [";
      payload += category;
      payload += "]";
    }

    if (logid && logid[0] != '\0') {
      payload += " [";
      payload += logid;
      payload += "]";
    }

    if (code_location && code_location[0] != '\0') {
      payload += " [";
      payload += code_location;
      payload += "]";
    }

    payload += " ";
    payload += (message && message[0] != '\0') ? message : "(no message)";

    logger->Log(MapOrtLogLevel(severity), payload);
  } catch (...) {
    // Logging callbacks must not throw across the C boundary.
  }
}

void OGA_API_CALL OgaLogCallback(const char* string, size_t length) {
  auto* logger = s_oga_logger.load(std::memory_order_acquire);
  if (logger == nullptr) {
    return;
  }

  try {
    std::string_view message;
    if (string != nullptr) {
      message = std::string_view(string, length);
    }

    std::string payload = "GenAI ";
    payload += message.empty() ? "(no message)" : std::string(message);
    logger->Log(LogLevel::Information, payload);
  } catch (...) {
    // Logging callbacks must not throw across the C boundary.
  }
}

void SetOgaLogCallback(ILogger* logger) {
  if (logger != nullptr) {
    s_oga_logger.store(logger, std::memory_order_release);
  }

  OgaResult* result = OgaSetLogCallback(logger != nullptr ? OgaLogCallback : nullptr);
  if (result == nullptr) {
    if (logger == nullptr) {
      s_oga_logger.store(nullptr, std::memory_order_release);
    }
    return;
  }

  const char* err = OgaResultGetError(result);
  std::string err_msg = err ? err : "unknown";
  OgaDestroyResult(result);

  if (logger != nullptr) {
    logger->Log(LogLevel::Warning, "Failed to set GenAI log callback: " + err_msg);
    s_oga_logger.store(nullptr, std::memory_order_release);
  }
}

}  // namespace

std::mutex Manager::s_mutex_;
std::unique_ptr<Manager> Manager::s_instance_;

Manager::Manager(const Configuration& config)
    : config_(config) {
  config_.Validate();

  const bool genai_verbose_logging = IsGenAIVerboseLoggingEnabled();
  const auto logger_level = genai_verbose_logging ? LogLevel::Verbose : config_.log_level;

  logger_ = std::make_unique<SpdlogLogger>(logger_level, config_.logs_dir.value_or(""));
  s_ort_logger.store(logger_.get(), std::memory_order_release);

  if (genai_verbose_logging) {
    SetOgaLogCallback(logger_.get());
  }

#if defined(__ANDROID__) && !defined(NDEBUG)
  CheckSslCertSetup(*logger_);
#endif

  // Build the EP registration callback. When a bootstrapper successfully
  // prepares an EP, this callback registers it with ORT via the C API.
  // OrtEnv is a singleton — CreateEnv returns the existing instance if GenAI
  // (or any other ORT consumer) already created one, with a bumped refcount.
  // We own one refcount and release it (plus unregister each EP we registered)
  // in ~Manager().
  ort_api_ = OrtGetApiBase()->GetApi(ORT_API_VERSION);
  if (!ort_api_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "ORT API not available");
  }

  {
    OrtStatus* status = ort_api_->CreateEnvWithCustomLogger(
        OrtLogCallback, nullptr, GetDefaultOrtLoggingLevel(genai_verbose_logging), "foundry_local", &ort_env_);
    if (status != nullptr) {
      const char* msg = ort_api_->GetErrorMessage(status);
      std::string err = std::string("Failed to create OrtEnv: ") + (msg ? msg : "unknown");
      ort_api_->ReleaseStatus(status);
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, err);
    }
  }

  LogRuntimeVersions(*logger_);

  EpRegistrationCallback register_ep = [this, &log = *logger_](
                                           const std::string& registration_name,
                                           const std::filesystem::path& library_path) -> bool {
    OrtStatus* status = ort_api_->RegisterExecutionProviderLibrary(
        ort_env_, registration_name.c_str(), library_path.c_str());
    if (status != nullptr) {
      const char* msg = ort_api_->GetErrorMessage(status);
      log.Log(LogLevel::Warning,
              std::string("EP registration: RegisterExecutionProviderLibrary failed for '") +
                  registration_name + "': " + (msg ? msg : "unknown"));
      ort_api_->ReleaseStatus(status);
      return false;
    }

    registered_ep_libraries_.push_back(registration_name);

    auto version = GetEpVersion(*ort_api_, *ort_env_, registration_name);
    log.Log(LogLevel::Information,
            std::string("EP registration: '") + registration_name +
                "' registered successfully (library=" + library_path.string() +
                ", version=" + version + ")");
    return true;
  };

  // Discover bootstrappers from available EP sources
  std::vector<std::unique_ptr<IEpBootstrapper>> bootstrappers;

  // Detected once and reused below for both the WinML-catalog skip-list and the
  // Foundry CUDA bootstrapper. HasNvidiaGpu() shells out to nvidia-smi, so caching
  // the result here avoids a second subprocess spawn.
  const bool has_nvidia_gpu = CudaEpBootstrapper::HasNvidiaGpu();

#if FOUNDRY_LOCAL_HAS_EP_CATALOG
  // WinML EPs — enumerate from the OS EP catalog (Windows 10 19H1+ reg-free runtime).
  // Only present when the WinML EP catalog NuGet package was successfully resolved
  // at CMake time (gated on WinMLEpCatalog_FOUND in sdk_v2/cpp/CMakeLists.txt).
  //
  // Skip catalog entries for EPs that Foundry installs and registers itself through
  // the CDN bootstrappers below — WebGPU always, and CUDA when an NVIDIA GPU is
  // present. For WebGPU the catalog reports a library path next to onnxruntime.dll
  // rather than the Foundry cache where the EP is actually installed; that path is
  // absent in our deployments, so keeping the catalog entry would add a second
  // bootstrapper for the same provider with a non-existent path and leave
  // GetDiscoverableEps reporting the EP as unregistered even after the Foundry
  // bootstrapper registered it under the correct cache path. Names are stored
  // lowercase and matched case-insensitively because the provider name's casing
  // ("WebGpu" vs "WebGPU") is not consistent across sources.
  std::set<std::string> foundry_managed_ep_names;
  foundry_managed_ep_names.insert("webgpuexecutionprovider");
  if (has_nvidia_gpu) {
    foundry_managed_ep_names.insert("cudaexecutionprovider");
  }

  auto winml_providers = WinMLEpBootstrapper::DiscoverProviders(register_ep, *logger_);
  for (auto& p : winml_providers) {
    if (foundry_managed_ep_names.count(ToLower(p->Name())) != 0) {
      logger_->Log(LogLevel::Information,
                   "WinML EP skipped: " + p->Name() +
                       " (Foundry installs and registers this EP via its own bootstrapper)");
      continue;
    }
    bootstrappers.push_back(std::move(p));
  }
#endif

  const auto cache_dir = std::filesystem::path(*config_.model_cache_dir).parent_path();

  // CUDA EP — only if an NVIDIA GPU is detected
  if (has_nvidia_gpu) {
    const auto cuda_ep_dir = cache_dir / "cuda-ep";
    bootstrappers.push_back(std::make_unique<CudaEpBootstrapper>(cuda_ep_dir.string(), register_ep));
  }

  // WebGPU EP — always available (no hardware detection needed).
  const auto webgpu_ep_dir = cache_dir / "webgpu-ep";
  bootstrappers.push_back(std::make_unique<WebGpuEpBootstrapper>(webgpu_ep_dir.string(), register_ep));

  ep_detector_ = std::make_unique<EpDetector>(*ort_api_, *ort_env_, std::move(bootstrappers), *logger_);

  // Read configurable download concurrency (default 64)
  int download_concurrency = 64;
  auto it = config_.additional_options.find("NumModelDownloadThreads");
  if (it != config_.additional_options.end()) {
    try {
      download_concurrency = std::stoi(it->second);
      if (download_concurrency < 1) {
        download_concurrency = 1;
      }
    } catch (const std::exception&) {
      // Ignore invalid values, use default
    }
  }

  // Read whether cross-region fallback should be disabled (default: enabled).
  // Accepts case-insensitive true/1/yes.
  const bool disable_region_fallback = IsAdditionalOptionEnabled(config_, "DisableRegionFallback");

  download_manager_ = std::make_unique<DownloadManager>(
      *config_.model_cache_dir,
      config_.catalog_region.value_or("auto"),
      download_concurrency,
      *logger_,
      disable_region_fallback);
  model_load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);
  session_manager_ = std::make_unique<SessionManager>(*logger_);
  telemetry_ = std::make_unique<TelemetryLogger>(config_.app_name, *logger_);
  catalog_ = std::make_unique<AzureModelCatalog>(
      config_.catalog_urls,
      download_manager_->GetCacheDirectory(),
      [this](ModelInfo info, std::string local_path) {
        return CreateModel(std::move(info), std::move(local_path));
      },
      *ep_detector_, *logger_,
      config_.external_service_url.has_value(),
      config_.catalog_region.value_or("auto"),
      disable_region_fallback);
}

Manager::~Manager() {
  // Signal subsystems to drain before tearing down infrastructure
  try {
    Shutdown();
  } catch (const std::exception& e) {
    logger_->Log(LogLevel::Error,
                 std::string("Exception while shutting down Manager subsystems during destruction: ") + e.what());
  } catch (...) {
    // Suppress exceptions during destruction
    logger_->Log(LogLevel::Error, "Unknown exception while shutting down Manager subsystems during destruction.");
  }

  // Tear down members that hold OrtEnv references / live ORT sessions before
  // we unregister EPs and release the env. C++ would destroy these in reverse
  // declaration order after this function returns, but the env release below
  // requires they be gone *now*.
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
  web_service_.reset();
#endif
  session_manager_.reset();
  model_load_manager_.reset();
  download_manager_.reset();
  catalog_.reset();
  telemetry_.reset();
  ep_detector_.reset();

  // Unregister EPs we registered, then drop our OrtEnv refcount. Best-effort:
  // log failures but don't throw from a destructor.
  if (ort_api_ != nullptr && ort_env_ != nullptr) {
    for (const auto& name : registered_ep_libraries_) {
      OrtStatus* status = ort_api_->UnregisterExecutionProviderLibrary(ort_env_, name.c_str());
      if (status != nullptr) {
        const char* msg = ort_api_->GetErrorMessage(status);
        logger_->Log(LogLevel::Warning,
                     std::string("EP unregister: UnregisterExecutionProviderLibrary failed for '") +
                         name + "': " + (msg ? msg : "unknown"));
        ort_api_->ReleaseStatus(status);
      }
    }

    ort_api_->ReleaseEnv(ort_env_);
    ort_env_ = nullptr;
  }

  if (s_oga_logger.load(std::memory_order_acquire) != nullptr) {
    SetOgaLogCallback(nullptr);
  }

  logger_->Log(LogLevel::Information, "Manager is being disposed.");

  // ORT may still emit late teardown logs from internal static cleanup after Manager destruction
  // due to GenAI keeping the OrtEnv alive until the process exits.
  // Clear s_ort_logger so OrtLogCallback does not dereference a dangling pointer and ignores late logs.
  s_ort_logger.store(nullptr, std::memory_order_release);
}

Manager& Manager::Create(const Configuration& config) {
  std::lock_guard<std::mutex> lock(s_mutex_);

  if (s_instance_ != nullptr) {
    FL_LOG_AND_THROW(s_instance_->GetLogger(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     "Manager already created. Call Destroy() first.");
  }

  // Construct into a local unique_ptr so a throw between construction and the post-init
  // telemetry/log calls cleans up the partially-initialized Manager instead of leaking it.
  // The constructor validates and resolves defaults; if it throws, no Manager exists.
  auto created = std::unique_ptr<Manager>(new Manager(config));

  // Telemetry/log failure during init must not leave the singleton in a half-initialized
  // state: catch and log, then proceed. The Manager itself is fully constructed at this
  // point — only the post-construction signaling can fail, and it's not load-bearing.
  try {
    created->telemetry_->RecordAction(Action::kCoreInitialize, ActionStatus::kSuccess, "", false, 0);
  } catch (const std::exception& ex) {
    created->GetLogger().Log(LogLevel::Error,
                             fmt::format("telemetry RecordAction failed during Create: {}", ex.what()));
  }

  created->GetLogger().Log(LogLevel::Information, "Manager initialized successfully.");

  s_instance_ = std::move(created);
  return *s_instance_;
}

Manager& Manager::Instance() {
  std::lock_guard<std::mutex> lock(s_mutex_);
  if (s_instance_ == nullptr) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "Manager not created. Call Create() first. "
             "Manager must remain valid until all Model and Session instances are destroyed.");
  }

  return *s_instance_;
}

void Manager::Destroy() {
  std::lock_guard<std::mutex> lock(s_mutex_);
  s_instance_.reset();
}

ICatalog& Manager::GetCatalog() {
  return *catalog_;
}

void Manager::StartWebService() {
  if (web_service_running_) {
    FL_LOG_AND_THROW(*logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "web service is already running");
  }

  if (config_.external_service_url.has_value()) {
    FL_LOG_AND_THROW(*logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     "cannot start local web service when external_service_url is configured");
  }

  ActionTracker tracker(Action::kCoreServiceStart, *telemetry_);

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
  web_service_ = std::make_unique<WebService>(*catalog_, *logger_, *config_.model_cache_dir, *model_load_manager_,
                                              *session_manager_, *telemetry_,
                                              [this]() { Shutdown(); });

  auto endpoints = config_.web_service_endpoints;
  if (endpoints.empty()) {
    endpoints.push_back("http://127.0.0.1:0");
  }

  bound_urls_ = web_service_->Start(endpoints);
  web_service_running_ = true;
  tracker.SetStatus(ActionStatus::kSuccess);
#else
  FL_LOG_AND_THROW(*logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                   "web service requires oatpp (build with FOUNDRY_LOCAL_BUILD_SERVICE=ON)");
#endif
}

const std::vector<std::string>& Manager::GetWebServiceUrls() const {
  // No "not running" check: bound_urls_ is cleared in StopWebService() and is empty before
  // StartWebService(), so the empty vector is the documented "service is not running" signal
  // (see GetWebServiceEndpoints() docstring in foundry_local_cpp.h).
  return bound_urls_;
}

void Manager::StopWebService() {
  if (!web_service_running_) {
    // No-op rather than throw: the public-API contract treats StopWebService() as idempotent so
    // callers can shut down unconditionally without first probing service state.
    logger_->Log(LogLevel::Information, "StopWebService called but web service is not running; ignoring");
    return;
  }

  ActionTracker tracker(Action::kCoreServiceStop, *telemetry_);

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
  web_service_->Stop();
  web_service_.reset();
  web_service_running_ = false;
  bound_urls_.clear();
  tracker.SetStatus(ActionStatus::kSuccess);
#else
  FL_LOG_AND_THROW(*logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                   "web service requires oatpp (build with FOUNDRY_LOCAL_BUILD_SERVICE=ON)");
#endif
}

void Manager::Shutdown() {
  bool expected = false;
  if (!shutdown_requested_.compare_exchange_strong(expected, true)) {
    return;  // already shutting down
  }

  logger_->Log(LogLevel::Information, "Shutdown requested");

  if (web_service_running_) {
    StopWebService();
  }

  // Order matters:
  //   1. Reject new loads so callers gated on IsShutdownRequested can stop early.
  //   2. Cancel + drain HTTP-tracked sessions (web service path).
  //   3. Unload all models, polling per-model session refcount for direct-API users
  //      who haven't dropped their flSession* yet. Bounded by timeout so a stuck
  //      caller can't block process shutdown indefinitely.
  model_load_manager_->RejectNewLoads();
  session_manager_->CancelAll();
  session_manager_->WaitForDrain();
  model_load_manager_->UnloadAll();
}

bool Manager::IsShutdownRequested() const {
  return shutdown_requested_.load();
}

const Configuration& Manager::GetConfiguration() const {
  return config_;
}

Model Manager::CreateModel(ModelInfo info, std::string local_path) {
  return Model::FromModelInfo(std::move(info),
                              std::move(local_path),
                              *download_manager_,
                              *model_load_manager_);
}

DownloadManager& Manager::GetDownloadManager() {
  return *download_manager_;
}

ModelLoadManager& Manager::GetModelLoadManager() {
  return *model_load_manager_;
}

SessionManager& Manager::GetSessionManager() {
  return *session_manager_;
}

ILogger& Manager::GetLogger() {
  return *logger_;
}

ITelemetry& Manager::GetTelemetry() {
  return *telemetry_;
}

const IEpDetector& Manager::GetEpDetector() const {
  return *ep_detector_;
}

IEpDetector& Manager::GetEpDetector() {
  return *ep_detector_;
}

EpDownloadResult Manager::DownloadAndRegisterEps(
    const std::vector<std::string>* names,
    const IEpBootstrapper::ProgressCallback& progress_cb) {
  auto result = ep_detector_->DownloadAndRegisterEps(names, progress_cb);

  // EP registration changes which device/EP filters the catalog uses.
  // Invalidate so the next catalog query re-fetches with updated filters.
  if (result.success && !result.registered_eps.empty()) {
    catalog_->InvalidateCache();
  }

  return result;
}

}  // namespace fl
