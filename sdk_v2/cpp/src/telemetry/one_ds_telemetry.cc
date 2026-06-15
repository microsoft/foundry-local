// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// 1DS-backed ITelemetry implementation. Compiled only when FOUNDRY_LOCAL_USE_TELEMETRY=ON
// (which requires the cpp-client-telemetry vcpkg port to be available).

#include "telemetry/one_ds_telemetry.h"

#include "telemetry/telemetry_environment.h"

#include <fmt/format.h>

// 1DS C++ SDK headers. The vcpkg port ships these via find_package(MSTelemetry CONFIG).
#include <LogManager.hpp>
#include <ILogger.hpp>
#include <EventProperties.hpp>

// The LogManager macro must appear exactly once per binary that uses the
// "v1 classic" LogManager API surface. The macro instantiates the singleton
// statics for our module configuration.
LOGMANAGER_INSTANCE

namespace fl {

namespace {

using ::Microsoft::Applications::Events::LogManager;
using MatILogger = ::Microsoft::Applications::Events::ILogger;
using ::Microsoft::Applications::Events::EventProperties;
using ::Microsoft::Applications::Events::EventPriority;
using ::Microsoft::Applications::Events::PiiKind_None;

constexpr uint64_t kCriticalData = MICROSOFT_KEYWORD_CRITICAL_DATA;

void SetCommonContext(MatILogger* mat_logger, const TelemetryMetadata& m) {
  // Process-wide context — stamped on every event uploaded through this ILogger.
  mat_logger->SetContext("AppName", m.app_name);
  mat_logger->SetContext("Version", m.version);
  // 1DS recognizes UTCReplace_AppSessionGuid as a magic field name on Windows UTC
  // and substitutes the OS session GUID. On other platforms we just send the
  // GUID we computed at startup — same correlation semantics either way.
  mat_logger->SetContext("UTCReplace_AppSessionGuid", m.app_session_guid);
  mat_logger->SetContext("OsName", m.os_name);
  mat_logger->SetContext("OsVersion", m.os_version);
  mat_logger->SetContext("CpuArch", m.cpu_arch);
}

EventProperties MakeEvent(const char* name, bool test_mode) {
  EventProperties ev(name);
  ev.SetPriority(EventPriority::EventPriority_Normal);
  ev.SetPolicyBitFlags(kCriticalData);
  // `test` is stamped on every event so CI/test data is distinguishable in the backend
  // when FOUNDRY_TESTING_MODE is set. In CI (IsCiEnvironment()=true) we never reach
  // emission at all, so this only ever toggles for explicit test mode.
  ev.SetProperty("test", test_mode);
  return ev;
}

void SafeLog(MatILogger* mat_logger, EventProperties& ev) {
  if (mat_logger != nullptr) {
    mat_logger->LogEvent(ev);
  }
}

MatILogger* GetMatLogger() {
  // LogManager::GetLogger() returns nullptr until Initialize has been called.
  return LogManager::GetLogger();
}

}  // namespace

OneDsTelemetry::OneDsTelemetry(const std::string& tenant_token,
                               const std::string& app_name,
                               ILogger& logger)
    : local_log_(app_name, logger),
      metadata_(BuildTelemetryMetadata(app_name)),
      logger_(logger) {
  const bool is_ci = TelemetryEnvironment::IsCiEnvironment();
  if (is_ci) {
    logger_.Log(LogLevel::Information,
                "[Telemetry] CI environment detected; 1DS upload disabled (events still logged locally)");
    return;
  }
  if (tenant_token.empty()) {
    logger_.Log(LogLevel::Information,
                "[Telemetry] Tenant token is empty; 1DS upload disabled (events still logged locally)");
    return;
  }

  try {
    auto* mat_logger = LogManager::Initialize(tenant_token);
    if (mat_logger == nullptr) {
      logger_.Log(LogLevel::Warning,
                  "[Telemetry] LogManager::Initialize returned null; 1DS upload disabled");
      return;
    }
    SetCommonContext(mat_logger, metadata_);
    initialized_.store(true, std::memory_order_release);
    logger_.Log(LogLevel::Information,
                fmt::format("[Telemetry] 1DS initialized; AppName={} Version={} Os={} {} Arch={} TestMode={}",
                            metadata_.app_name, metadata_.version, metadata_.os_name,
                            metadata_.os_version, metadata_.cpu_arch, metadata_.test_mode));
  } catch (const std::exception& ex) {
    logger_.Log(LogLevel::Warning,
                fmt::format("[Telemetry] LogManager::Initialize threw: {}; 1DS upload disabled", ex.what()));
  } catch (...) {
    logger_.Log(LogLevel::Warning,
                "[Telemetry] LogManager::Initialize threw unknown exception; 1DS upload disabled");
  }
}

OneDsTelemetry::~OneDsTelemetry() {
  if (!initialized_.load(std::memory_order_acquire)) {
    return;
  }
  try {
    LogManager::FlushAndTeardown();
  } catch (...) {
    // Best-effort: never throw from a destructor.
  }
}

void OneDsTelemetry::RecordAction(Action action, ActionStatus status, const std::string& user_agent,
                                  bool indirect, int64_t duration_ms) {
  local_log_.RecordAction(action, status, user_agent, indirect, duration_ms);
  if (!initialized_.load(std::memory_order_acquire)) {
    return;
  }
  auto ev = MakeEvent("Action", metadata_.test_mode);
  ev.SetProperty("Action", std::string(ActionToString(action)));
  ev.SetProperty("Status", std::string(ActionStatusToString(status)));
  ev.SetProperty("UserAgent", user_agent);
  ev.SetProperty("Direct", !indirect);
  ev.SetProperty("TimeMs", duration_ms);
  SafeLog(GetMatLogger(), ev);
}

void OneDsTelemetry::RecordException(Action action, const std::exception& exception) {
  RecordException(action, exception, std::string{});
}

void OneDsTelemetry::RecordException(Action action, const std::exception& exception,
                                     const std::string& user_agent) {
  local_log_.RecordException(action, exception, user_agent);
  if (!initialized_.load(std::memory_order_acquire)) {
    return;
  }
  auto ev = MakeEvent("Error", metadata_.test_mode);
  ev.SetProperty("Action", std::string(ActionToString(action)));
  ev.SetProperty("UserAgent", user_agent);
  ev.SetProperty("ExceptionType", "std::exception");
  ev.SetProperty("ExceptionMessage", std::string(exception.what()));
  ev.SetProperty("InnerExceptionType", "");
  ev.SetProperty("InnerExceptionMessage", "");
  ev.SetProperty("StackTrace", "");
  ev.SetProperty("InnerStackTrace", "");
  SafeLog(GetMatLogger(), ev);
}

void OneDsTelemetry::RecordModelUsage(const ModelUsageInfo& info) {
  local_log_.RecordModelUsage(info);
  if (!initialized_.load(std::memory_order_acquire)) {
    return;
  }
  auto ev = MakeEvent("Model", metadata_.test_mode);
  ev.SetProperty("ModelId", info.model_id);
  ev.SetProperty("ExecutionProvider", info.execution_provider);
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("TimeToFirstTokenMs", info.time_to_first_token_ms);
  ev.SetProperty("TotalTimeMs", info.total_time_ms);
  ev.SetProperty("TotalTokens", static_cast<int64_t>(info.total_tokens));
  ev.SetProperty("InputTokenCount", static_cast<int64_t>(info.input_token_count));
  ev.SetProperty("NumMessages", static_cast<int64_t>(info.num_messages));
  ev.SetProperty("MemoryUsedMB", info.memory_used_mb);
  ev.SetProperty("CpuTimeMs", info.cpu_time_ms);
  ev.SetProperty("GpuMemoryUsedMB", info.gpu_memory_used_mb);
  SafeLog(GetMatLogger(), ev);
}

void OneDsTelemetry::RecordModelId(Action action, const std::string& model_id,
                                   ActionStatus status, const std::string& user_agent) {
  local_log_.RecordModelId(action, model_id, status, user_agent);
  if (!initialized_.load(std::memory_order_acquire) || model_id.empty()) {
    return;
  }
  auto ev = MakeEvent("ModelId", metadata_.test_mode);
  ev.SetProperty("Action", std::string(ActionToString(action)));
  ev.SetProperty("ModelId", model_id);
  ev.SetProperty("Status", std::string(ActionStatusToString(status)));
  ev.SetProperty("UserAgent", user_agent);
  SafeLog(GetMatLogger(), ev);
}

void OneDsTelemetry::RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) {
  local_log_.RecordEpDownloadAttempt(info);
  if (!initialized_.load(std::memory_order_acquire)) {
    return;
  }
  auto ev = MakeEvent("EPDownloadAttempt", metadata_.test_mode);
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("Attempts", static_cast<int64_t>(info.attempts));
  ev.SetProperty("NumProviders", static_cast<int64_t>(info.num_providers));
  ev.SetProperty("Succeeded", static_cast<int64_t>(info.succeeded));
  ev.SetProperty("Failed", static_cast<int64_t>(info.failed));
  ev.SetProperty("Resolved", info.resolved);
  ev.SetProperty("Status", std::string(ActionStatusToString(info.status)));
  ev.SetProperty("TimeMs", info.duration_ms);
  SafeLog(GetMatLogger(), ev);
}

void OneDsTelemetry::RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) {
  local_log_.RecordEpDownloadAndRegister(info);
  if (!initialized_.load(std::memory_order_acquire)) {
    return;
  }
  auto ev = MakeEvent("EPDownloadAndRegister", metadata_.test_mode);
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("ProviderName", info.provider_name);
  ev.SetProperty("InitReadyState", info.init_ready_state);
  ev.SetProperty("DownloadReadyState", info.download_ready_state);
  ev.SetProperty("DownloadStatus", std::string(ActionStatusToString(info.download_status)));
  ev.SetProperty("DownloadTimeMs", info.download_duration_ms);
  ev.SetProperty("RegisterReadyState", info.register_ready_state);
  ev.SetProperty("RegisterStatus", std::string(ActionStatusToString(info.register_status)));
  ev.SetProperty("RegisterTimeMs", info.register_duration_ms);
  SafeLog(GetMatLogger(), ev);
}

void OneDsTelemetry::RecordDownload(const DownloadInfo& info) {
  local_log_.RecordDownload(info);
  if (!initialized_.load(std::memory_order_acquire)) {
    return;
  }
  auto ev = MakeEvent("Download", metadata_.test_mode);
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("ModelId", info.model_id);
  ev.SetProperty("Status", std::string(ActionStatusToString(info.status)));
  ev.SetProperty("LockWaitTimeMs", info.lock_wait_ms);
  ev.SetProperty("EnumerationTimeMs", info.enumeration_ms);
  ev.SetProperty("DownloadTimeMs", info.download_ms);
  ev.SetProperty("TotalSizeBytes", info.total_size_bytes);
  ev.SetProperty("AlreadyCachedBytes", info.already_cached_bytes);
  ev.SetProperty("FileCount", static_cast<int64_t>(info.file_count));
  ev.SetProperty("SkippedFileCount", static_cast<int64_t>(info.skipped_file_count));
  ev.SetProperty("DownloadWaitResult", info.download_wait_result);
  ev.SetProperty("MaxConcurrency", static_cast<int64_t>(info.max_concurrency));
  SafeLog(GetMatLogger(), ev);
}

}  // namespace fl
