// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "manager.h"

#include <fmt/format.h>
#include <onnxruntime_c_api.h>
#include <ort_genai_c.h>

#include <atomic>
#include <string_view>

#include "catalog.h"
#include "catalog/azure_model_catalog.h"
#include "download/download_manager.h"
#if FOUNDRY_LOCAL_HAS_EP_BOOTSTRAPPERS
#include "ep_detection/cuda_ep_bootstrapper.h"
#endif
#include "ep_detection/ep_detector.h"
#include "ep_detection/ep_types.h"
#include "ep_detection/runtime_version_info.h"
#if FOUNDRY_LOCAL_HAS_EP_BOOTSTRAPPERS
#include "ep_detection/webgpu_ep_bootstrapper.h"
#endif
#include "exception.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session_manager.h"
#include "spdlog_logger.h"
#include "telemetry/telemetry_action_tracker.h"
#include "telemetry/one_ds_telemetry.h"
#include "telemetry/telemetry_environment.h"
#include "telemetry/telemetry_metadata.h"
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

void ORT_API_CALL OrtLogCallback(void* /*logger_param*/, OrtLoggingLevel severity, const char* category,
                                 const char* logid, const char* code_location, const char* message) {
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

  if (logger == nullptr) {
    OgaDestroyResult(result);
    s_oga_logger.store(nullptr, std::memory_order_release);
    return;
  }

  const char* err = OgaResultGetError(result);
  std::string err_msg = err ? err : "unknown";
  OgaDestroyResult(result);

  logger->Log(LogLevel::Warning, "Failed to set GenAI log callback: " + err_msg);
  s_oga_logger.store(nullptr, std::memory_order_release);
}

}  // namespace

std::mutex Manager::s_mutex_;
std::unique_ptr<Manager> Manager::s_instance_;

Manager::Manager(const Configuration& config) : config_(config) {
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

#if FOUNDRY_LOCAL_HAS_EP_BOOTSTRAPPERS
  // Build the EP registration callback. When a bootstrapper successfully
  // prepares an EP, this callback registers it with ORT via the C API.
  EpRegistrationCallback register_ep = [this, &log = *logger_](const std::string& registration_name,
                                                               const std::filesystem::path& library_path) -> bool {
    OrtStatus* status =
        ort_api_->RegisterExecutionProviderLibrary(ort_env_, registration_name.c_str(), library_path.c_str());
    if (status != nullptr) {
      const char* msg = ort_api_->GetErrorMessage(status);
      log.Log(LogLevel::Warning, std::string("EP registration: RegisterExecutionProviderLibrary failed for '") +
                                     registration_name + "': " + (msg ? msg : "unknown"));
      ort_api_->ReleaseStatus(status);
      return false;
    }

    registered_ep_libraries_.push_back(registration_name);

    auto version = GetEpVersion(*ort_api_, *ort_env_, registration_name);
    log.Log(LogLevel::Information, std::string("EP registration: '") + registration_name +
                                       "' registered successfully (library=" + library_path.string() +
                                       ", version=" + version + ")");
    return true;
  };
#endif

  // Discover bootstrappers from available EP sources
  std::vector<std::unique_ptr<IEpBootstrapper>> bootstrappers;

#if FOUNDRY_LOCAL_HAS_EP_BOOTSTRAPPERS
  // Detected once and reused below for the WinML catalog skip-list and CUDA bootstrapper.
  // Avoid probing NVML on platforms where Foundry Local does not publish a CUDA bundle.
  const bool has_nvidia_gpu =
      CudaEpBootstrapper::IsSupportedPlatform() && CudaEpBootstrapper::HasNvidiaGpu(*logger_);

#if FOUNDRY_LOCAL_HAS_EP_CATALOG
  // WinML EPs — enumerate from the OS EP catalog (Windows 10 19H1+ reg-free runtime).
  // Only present when the WinML EP catalog NuGet package was successfully resolved
  // at CMake time (gated on WinMLEpCatalog_FOUND in sdk_v2/cpp/CMakeLists.txt).

  auto winml_providers = WinMLEpBootstrapper::DiscoverProviders(register_ep, *logger_);
  for (auto& p : winml_providers) {
    bootstrappers.push_back(std::move(p));
  }
#endif

  // CUDA EP — only if an NVIDIA GPU is detected. Rooted under app_data_dir (not the model cache
  // parent) so the install survives a user pointing model_cache_dir somewhere ephemeral.
  if (has_nvidia_gpu) {
    const auto cuda_ep_root = std::filesystem::path(*config_.app_data_dir) / "ep" / "cuda-ep";
    bootstrappers.push_back(std::make_unique<CudaEpBootstrapper>(cuda_ep_root.string(), register_ep));
  }

  // WebGPU EP — only on exact architectures for which a bundle is published.
  if (WebGpuEpBootstrapper::IsSupportedPlatform()) {
    const auto webgpu_ep_root = std::filesystem::path(*config_.app_data_dir) / "ep" / "webgpu-ep";
    bootstrappers.push_back(std::make_unique<WebGpuEpBootstrapper>(webgpu_ep_root.string(), register_ep));
  }
#endif

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

  download_manager_ =
      std::make_unique<DownloadManager>(*config_.model_cache_dir, config_.catalog_region.value_or("auto"),
                                        download_concurrency, *logger_, disable_region_fallback);
  model_load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);
  session_manager_ = std::make_unique<SessionManager>(*logger_);
  const bool disable_nonessential_telemetry =
      config_.disable_nonessential_telemetry ||
      IsAdditionalOptionEnabled(config_, "DisableNonessentialTelemetry");
  const bool telemetry_hard_disabled =
      TelemetryEnvironment::IsCiEnvironment() || TelemetryEnvironment::IsTelemetryDisabledByEnvVar();
  telemetry_ = std::make_unique<OneDsTelemetry>(config_.app_name, *logger_, disable_nonessential_telemetry);
  try {
    telemetry_->RecordProcessInfo(
        BuildProcessInfo(BuildTelemetryMetadata(config_.app_name),
                         !disable_nonessential_telemetry && !telemetry_hard_disabled));
  } catch (const std::exception& ex) {
    logger_->Log(
        LogLevel::Warning,
        fmt::format("telemetry ProcessInfo failed during Manager initialization: {}", ex.what()));
  } catch (...) {
    logger_->Log(LogLevel::Warning, "telemetry ProcessInfo failed during Manager initialization.");
  }
  catalog_ = std::make_unique<AzureModelCatalog>(
      config_.catalog_urls, download_manager_->GetCacheDirectory(),
      [this](ModelInfo info, std::string local_path) { return CreateModel(std::move(info), std::move(local_path)); },
      *ep_detector_, *logger_, config_.external_service_url.has_value(), config_.catalog_region.value_or("auto"),
      disable_region_fallback);
}

Manager::~Manager() {
  const auto safe_log = [this](LogLevel level, std::string_view message) noexcept {
    try {
      if (logger_ != nullptr) {
        logger_->Log(level, message);
      }
    } catch (...) {
    }
  };

  try {
    Shutdown();
  } catch (const std::exception& e) {
    safe_log(LogLevel::Error,
             std::string("Exception while shutting down Manager subsystems during destruction: ") + e.what());
  } catch (...) {
    safe_log(LogLevel::Error, "Unknown exception while shutting down Manager subsystems during destruction.");
  }

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
  web_service_.reset();
#endif
  session_manager_.reset();
  model_load_manager_.reset();
  download_manager_.reset();
  catalog_.reset();
  telemetry_.reset();

  OgaShutdown();

  if (ort_api_ != nullptr && ort_env_ != nullptr) {
    for (auto it = registered_ep_libraries_.rbegin(); it != registered_ep_libraries_.rend(); ++it) {
      const auto& name = *it;
      OrtStatus* status = ort_api_->UnregisterExecutionProviderLibrary(ort_env_, name.c_str());
      if (status != nullptr) {
        const char* msg = ort_api_->GetErrorMessage(status);
        safe_log(LogLevel::Warning, std::string("EP unregister: UnregisterExecutionProviderLibrary failed for '") +
                                        name + "': " + (msg ? msg : "unknown"));
        ort_api_->ReleaseStatus(status);
      }
    }

    ep_detector_.reset();
    ort_api_->ReleaseEnv(ort_env_);
    ort_env_ = nullptr;
  } else {
    ep_detector_.reset();
  }

  if (s_oga_logger.load(std::memory_order_acquire) != nullptr) {
    SetOgaLogCallback(nullptr);
  }

  safe_log(LogLevel::Information, "Manager is being disposed.");

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

ICatalog& Manager::GetCatalog() { return *catalog_; }

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
                                              *session_manager_, *telemetry_, [this]() { Shutdown(); });

  auto endpoints = config_.web_service_endpoints;
  if (endpoints.empty()) {
    endpoints.push_back("http://127.0.0.1:0");
  }

  bound_urls_ = web_service_->Start(endpoints);
  web_service_running_ = true;
  try {
    telemetry_->StartSession();
  } catch (const std::exception& ex) {
    logger_->Log(LogLevel::Warning, std::string("telemetry StartSession failed: ") + ex.what());
  } catch (...) {
    logger_->Log(LogLevel::Warning, "telemetry StartSession failed with unknown error");
  }
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
  try {
    telemetry_->EndSession();
  } catch (const std::exception& ex) {
    logger_->Log(LogLevel::Warning, std::string("telemetry EndSession failed: ") + ex.what());
  } catch (...) {
    logger_->Log(LogLevel::Warning, "telemetry EndSession failed with unknown error");
  }
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

bool Manager::IsShutdownRequested() const { return shutdown_requested_.load(); }

const Configuration& Manager::GetConfiguration() const { return config_; }

Model Manager::CreateModel(ModelInfo info, std::string local_path) {
  return Model::FromModelInfo(std::move(info), std::move(local_path), *download_manager_, *model_load_manager_);
}

DownloadManager& Manager::GetDownloadManager() { return *download_manager_; }

ModelLoadManager& Manager::GetModelLoadManager() { return *model_load_manager_; }

SessionManager& Manager::GetSessionManager() { return *session_manager_; }

ILogger& Manager::GetLogger() { return *logger_; }

ITelemetry& Manager::GetTelemetry() { return *telemetry_; }

const IEpDetector& Manager::GetEpDetector() const { return *ep_detector_; }

IEpDetector& Manager::GetEpDetector() { return *ep_detector_; }

EpDownloadResult Manager::DownloadAndRegisterEps(const std::vector<std::string>* names,
                                                 const IEpBootstrapper::ProgressCallback& progress_cb) {
  auto result = ep_detector_->DownloadAndRegisterEps(names, progress_cb);

  // EP registration changes which device/EP filters the catalog uses. Invalidate whenever at
  // least one EP registered — including partial success, where result.success is false because
  // another EP failed — so the next catalog query re-fetches with the updated filters.
  if (!result.registered_eps.empty()) {
    catalog_->InvalidateCache();
  }

  // Warn if any EPs failed to download or register, but keep going: CPU is always available and
  // any EPs that did register remain usable. This is not treated as an error.
  if (!result.cancelled && !result.failed_eps.empty()) {
    const auto join = [](const std::vector<std::string>& eps) {
      std::string joined;
      for (size_t i = 0; i < eps.size(); ++i) {
        joined += (i ? ", " : "") + eps[i];
      }
      return joined;
    };

    std::string message = "Failed to download or register EP(s) [" + join(result.failed_eps) +
                          "]; continuing with the remaining execution providers (CPU is always "
                          "available";
    if (!result.registered_eps.empty()) {
      message += "; also registered: [" + join(result.registered_eps) + "]";
    }
    message += "). See earlier logs for the underlying cause.";
    logger_->Log(LogLevel::Warning, message);
  }

  return result;
}

}  // namespace fl
