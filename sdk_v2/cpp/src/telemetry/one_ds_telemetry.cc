// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "telemetry/one_ds_telemetry.h"

#include "telemetry/device_id.h"
#include "telemetry/telemetry_environment.h"
#include "telemetry/telemetry_redaction.h"
#include "telemetry/telemetry_sampling.h"

#include <fmt/format.h>

#include <Enums.hpp>
#include <ILogConfiguration.hpp>
#include <ILogManager.hpp>
#include <LogManagerProvider.hpp>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <CommonFields.h>
#include <ILogger.hpp>
#include <EventProperties.hpp>
#include "one_ds_tenant_token.h"

#if defined(__ANDROID__)
extern "C" bool FoundryLocalIsAndroidTelemetryReady() noexcept;
#endif

namespace fl {

namespace {

using MatILogManager = ::Microsoft::Applications::Events::ILogManager;
using MatILogger = ::Microsoft::Applications::Events::ILogger;
using ::Microsoft::Applications::Events::EventProperties;
using ::Microsoft::Applications::Events::EventPriority;
using ::Microsoft::Applications::Events::PiiKind_None;
using ::Microsoft::Applications::Events::SessionState;
using ::Microsoft::Applications::Events::CFG_BOOL_SESSION_RESET_ENABLED;
using ::Microsoft::Applications::Events::CFG_INT_MAX_TEARDOWN_TIME;
using ::Microsoft::Applications::Events::CFG_INT_SDK_MODE;
using ::Microsoft::Applications::Events::CFG_INT_TRACE_LEVEL_MASK;
using ::Microsoft::Applications::Events::CFG_STR_PRIMARY_TOKEN;
using ::Microsoft::Applications::Events::CFG_STR_CACHE_FILE_PATH;
using ::Microsoft::Applications::Events::ILogConfiguration;
using ::Microsoft::Applications::Events::LogManagerProvider;
using ::Microsoft::Applications::Events::SdkModeTypes_CS;
using ::Microsoft::Applications::Events::STATUS_SUCCESS;
using ::Microsoft::Applications::Events::status_t;

constexpr uint64_t kCriticalData = MICROSOFT_KEYWORD_CRITICAL_DATA;
constexpr int kMaxTeardownUploadTimeSec = 1;

std::string DecodeBase64(std::string_view encoded) {
  auto DecodeChar = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };

  std::string result;
  result.reserve(encoded.size() * 3 / 4);
  uint32_t accum = 0;
  int bits = 0;
  for (char c : encoded) {
    int val = DecodeChar(c);
    if (val < 0) continue;
    accum = (accum << 6) | static_cast<uint32_t>(val);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      result.push_back(static_cast<char>((accum >> bits) & 0xFF));
    }
  }
  return result;
}

std::string GetToken() {
#if defined(FOUNDRY_LOCAL_TELEMETRY_TOKEN)
  return FOUNDRY_LOCAL_TELEMETRY_TOKEN;
#else
  static constexpr char kXorKey[] = "FoundryLocal";
  static constexpr char kEncodedToken[] =
      "fwtACgATHC9ZUgReclpDWQZFQXQOUVENIw5GXFBESn1CAQJbc1dEDVJfH3QLBUxYJw5EQ1wXGnpCUgNZJAtNVl0UQHkLTlZfd1o=";
  constexpr size_t klen = sizeof(kXorKey) - 1;
  std::string decoded = DecodeBase64(kEncodedToken);
  for (size_t i = 0; i < decoded.size(); ++i) {
    decoded[i] = static_cast<char>(decoded[i] ^ kXorKey[i % klen]);
  }
  return decoded;
#endif
}

void SetCommonContext(MatILogger* mat_logger, const TelemetryMetadata& m) {
  mat_logger->SetContext("AppName", m.app_name);
  mat_logger->SetContext("AppVersion", m.app_version);
  mat_logger->SetContext("FoundryLocalVersion", m.version);
  mat_logger->SetContext("AppSessionGuid", m.app_session_guid);
  mat_logger->SetContext("OsName", m.os_name);
  mat_logger->SetContext("OsVersion", m.os_version);
  mat_logger->SetContext("CpuArch", m.cpu_arch);
}

EventProperties MakeEvent(
    const char* name, double sample_rate_percent = TelemetryInternal::kTelemetrySampleRatePercent) {
  EventProperties ev(name);
  ev.SetPriority(EventPriority::EventPriority_Normal);
  ev.SetPolicyBitFlags(kCriticalData);
  ev.SetPopsample(sample_rate_percent);
  return ev;
}

void CleanupLogManager(MatILogManager* log_manager, ILogConfiguration& config) noexcept {
  if (log_manager == nullptr) {
    return;
  }

  try {
    log_manager->Flush();
  } catch (...) {
  }

  try {
    log_manager->FlushAndTeardown();
  } catch (...) {
  }

  try {
    LogManagerProvider::Release(config);
  } catch (...) {
  }
}

bool ShouldSampleEvent(std::string_view app_session_guid, std::string_view correlation_id,
                       double sample_rate_percent = TelemetryInternal::kTelemetrySampleRatePercent) {
  return TelemetryInternal::ShouldSampleTelemetryEvent(
      app_session_guid, correlation_id.empty() ? app_session_guid : correlation_id, sample_rate_percent);
}

void SafeLog(MatILogger* mat_logger, EventProperties& ev) {
  if (mat_logger != nullptr) {
    mat_logger->LogEvent(ev);
  }
}

std::string SanitizeTelemetryText(std::string_view value) {
  return ScrubStringForTelemetry(value);
}

}  // namespace

struct OneDsTelemetry::Impl {
  ILogConfiguration config;
  MatILogManager* log_manager = nullptr;
  MatILogger* logger = nullptr;
};

std::shared_lock<std::shared_mutex> OneDsTelemetry::LockForLogging(bool require_upload) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (!initialized_.load(std::memory_order_acquire) || !impl_ || impl_->logger == nullptr ||
      (require_upload && !upload_enabled_.load(std::memory_order_acquire))) {
    return {};
  }
  return lock;
}

OneDsTelemetry::OneDsTelemetry(const std::string& app_name,
                               ILogger& logger,
                               bool disable_nonessential_telemetry)
    : local_log_(app_name, logger),
      metadata_(BuildTelemetryMetadata(app_name)),
      logger_(logger) {
  if (TelemetryEnvironment::IsCiEnvironment()) {
    logger_.Log(LogLevel::Information,
                "[Telemetry] CI environment detected; 1DS upload disabled (events still logged locally)");
    return;
  }
  if (TelemetryEnvironment::IsTelemetryDisabledByEnvVar()) {
    logger_.Log(LogLevel::Information,
                "[Telemetry] Disabled via ORT_TELEMETRY_DISABLED; 1DS upload disabled "
                "(events still logged locally)");
    return;
  }
  if (disable_nonessential_telemetry) {
    upload_enabled_.store(false, std::memory_order_release);
    logger_.Log(LogLevel::Information,
                "[Telemetry] Disabled via configuration; non-essential 1DS upload disabled "
                "(ProcessInfo still uploads)");
  }
#if defined(__ANDROID__)
  if (!FoundryLocalIsAndroidTelemetryReady()) {
    logger_.Log(LogLevel::Information,
                "[Telemetry] Android 1DS Java HTTP bridge is not initialized; 1DS upload disabled "
                "(events still logged locally)");
    return;
  }
#endif
  const auto token = GetToken();
  if (token.empty()) {
    logger_.Log(LogLevel::Information,
                "[Telemetry] Token is empty; 1DS upload disabled (events still logged locally)");
    return;
  }

  bool log_manager_initialized = false;
  try {
    impl_ = std::make_unique<Impl>();
    auto& config = impl_->config;
    config[CFG_STR_PRIMARY_TOKEN] = token;
    config[CFG_BOOL_SESSION_RESET_ENABLED] = true;
    config[CFG_INT_TRACE_LEVEL_MASK] = 0;
    config[CFG_INT_SDK_MODE] = SdkModeTypes_CS;
    config[CFG_INT_MAX_TEARDOWN_TIME] = kMaxTeardownUploadTimeSec;
    if (const auto cache_dir = TelemetryDeviceId::EnsureCacheDirectory(); !cache_dir.empty()) {
      const auto cache_file_name =
          disable_nonessential_telemetry ? "foundry-local-processinfo.db" : "foundry-local.db";
      config[CFG_STR_CACHE_FILE_PATH] = (cache_dir / cache_file_name).string();
    }

    status_t status = STATUS_SUCCESS;
    impl_->log_manager = LogManagerProvider::CreateLogManager("FoundryLocal", true, config, status);
    if (status != STATUS_SUCCESS || impl_->log_manager == nullptr) {
      impl_.reset();
      logger_.Log(LogLevel::Warning,
                  "[Telemetry] LogManagerProvider::CreateLogManager failed; 1DS upload disabled");
      return;
    }
    log_manager_initialized = true;
    impl_->logger = impl_->log_manager->GetLogger(token);
    if (impl_->logger == nullptr) {
      CleanupLogManager(impl_->log_manager, impl_->config);
      impl_.reset();
      logger_.Log(LogLevel::Warning,
                  "[Telemetry] ILogManager::GetLogger returned null; 1DS upload disabled");
      return;
    }
    if (!disable_nonessential_telemetry && impl_->logger->GetSemanticContext() != nullptr) {
      auto* semantic_context = impl_->logger->GetSemanticContext();
      const auto hashed_device_id = TelemetryDeviceId::HashForTelemetry(TelemetryDeviceId::Instance().GetValue());
      if (!hashed_device_id.empty()) {
        semantic_context->SetDeviceId(hashed_device_id);
      }
    }
    SetCommonContext(impl_->logger, metadata_);
    logger_.Log(LogLevel::Information,
                fmt::format("[Telemetry] 1DS initialized; AppName={} AppVersion={} Version={} Os={} {} Arch={}",
                            metadata_.app_name, metadata_.app_version, metadata_.version, metadata_.os_name,
                            metadata_.os_version, metadata_.cpu_arch));
    initialized_.store(true, std::memory_order_release);
  } catch (const std::exception& ex) {
    if (log_manager_initialized) {
      if (impl_ != nullptr) {
        CleanupLogManager(impl_->log_manager, impl_->config);
      }
      impl_.reset();
    }
    logger_.Log(LogLevel::Warning,
                fmt::format("[Telemetry] LogManagerProvider initialization threw: {}; "
                            "1DS upload disabled", ex.what()));
  } catch (...) {
    if (log_manager_initialized) {
      if (impl_ != nullptr) {
        CleanupLogManager(impl_->log_manager, impl_->config);
      }
      impl_.reset();
    }
    logger_.Log(LogLevel::Warning,
                "[Telemetry] LogManagerProvider initialization threw unknown exception; 1DS upload disabled");
  }
}

OneDsTelemetry::~OneDsTelemetry() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (!initialized_.load(std::memory_order_acquire) || !impl_ || impl_->log_manager == nullptr) {
    return;
  }
  initialized_.store(false, std::memory_order_release);
  CleanupLogManager(impl_->log_manager, impl_->config);
  impl_.reset();
}

void OneDsTelemetry::RecordAction(Action action, ActionStatus status, const InvocationContext& context,
                                  int64_t duration_ms, const std::string& model_id) {
  local_log_.RecordAction(action, status, context, duration_ms, model_id);
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  if (!ShouldSampleEvent(metadata_.app_session_guid, context.correlation_id,
                         TelemetryInternal::SampleRateForAction(ActionToString(action)))) {
    return;
  }
  const auto sample_rate_percent = TelemetryInternal::SampleRateForAction(ActionToString(action));
  auto ev = MakeEvent("Action", sample_rate_percent);
  ev.SetProperty("Action", std::string(ActionToString(action)));
  ev.SetProperty("Status", std::string(ActionStatusToString(status)));
  ev.SetProperty("UserAgent", context.user_agent);
  ev.SetProperty("CorrelationId", context.correlation_id);
  ev.SetProperty("Direct", !context.indirect);
  ev.SetProperty("TimeMs", duration_ms);
  if (!model_id.empty()) {
    ev.SetProperty("ModelId", model_id);
  }
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::RecordException(Action action, const std::exception& exception,
                                     const InvocationContext& context) {
  local_log_.RecordException(action, exception, context);
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  if (!ShouldSampleEvent(metadata_.app_session_guid, context.correlation_id)) {
    return;
  }
  auto ev = MakeEvent("Error");
  ev.SetProperty("Action", std::string(ActionToString(action)));
  ev.SetProperty("UserAgent", context.user_agent);
  ev.SetProperty("CorrelationId", context.correlation_id);
  ev.SetProperty("ExceptionType", "std::exception");
  ev.SetProperty("ExceptionMessage", SanitizeTelemetryText(exception.what()));
  ev.SetProperty("InnerExceptionType", "");
  ev.SetProperty("InnerExceptionMessage", "");
  ev.SetProperty("StackTrace", "");
  ev.SetProperty("InnerStackTrace", "");
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::RecordModelUsage(const ModelUsageInfo& info) {
  local_log_.RecordModelUsage(info);
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  if (!ShouldSampleEvent(metadata_.app_session_guid, info.correlation_id)) {
    return;
  }
  auto ev = MakeEvent("Model");
  ev.SetProperty("ModelId", info.model_id);
  ev.SetProperty("ExecutionProvider", info.execution_provider);
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("CorrelationId", info.correlation_id);
  ev.SetProperty("Stream", info.stream);
  ev.SetProperty("Direct", !info.indirect);
  ev.SetProperty("TimeToFirstTokenMs", info.time_to_first_token_ms);
  ev.SetProperty("TotalTimeMs", info.total_time_ms);
  ev.SetProperty("TotalTokens", static_cast<int64_t>(info.total_tokens));
  ev.SetProperty("InputTokenCount", static_cast<int64_t>(info.input_token_count));
  ev.SetProperty("NumMessages", static_cast<int64_t>(info.num_messages));
  ev.SetProperty("MemoryUsedMB", info.memory_used_mb);
  ev.SetProperty("CpuTimeMs", info.cpu_time_ms);
  ev.SetProperty("GpuMemoryUsedMB", info.gpu_memory_used_mb);
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::RecordAudioUsage(const AudioUsageInfo& info) {
  local_log_.RecordAudioUsage(info);
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  if (!ShouldSampleEvent(metadata_.app_session_guid, info.correlation_id)) {
    return;
  }
  auto ev = MakeEvent("AudioModel");
  ev.SetProperty("ModelId", info.model_id);
  ev.SetProperty("ExecutionProvider", info.execution_provider);
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("CorrelationId", info.correlation_id);
  ev.SetProperty("AudioSource", info.audio_source);
  ev.SetProperty("Language", info.language);
  ev.SetProperty("Stream", info.stream);
  ev.SetProperty("Direct", !info.indirect);
  ev.SetProperty("TotalTimeMs", info.total_time_ms);
  ev.SetProperty("TotalTokens", static_cast<int64_t>(info.total_tokens));
  ev.SetProperty("InputTokenCount", static_cast<int64_t>(info.input_token_count));
  ev.SetProperty("CompletionTokenCount", static_cast<int64_t>(info.completion_token_count));
  ev.SetProperty("AudioDurationMs", info.audio_duration_ms);
  ev.SetProperty("SampleRate", static_cast<int64_t>(info.sample_rate));
  ev.SetProperty("Channels", static_cast<int64_t>(info.channels));
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) {
  local_log_.RecordEpDownloadAttempt(info);
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  if (!ShouldSampleEvent(metadata_.app_session_guid, info.correlation_id)) {
    return;
  }
  auto ev = MakeEvent("EPDownloadAttempt");
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("CorrelationId", info.correlation_id);
  ev.SetProperty("Attempts", static_cast<int64_t>(info.attempts));
  ev.SetProperty("NumProviders", static_cast<int64_t>(info.num_providers));
  ev.SetProperty("Succeeded", static_cast<int64_t>(info.succeeded));
  ev.SetProperty("Failed", static_cast<int64_t>(info.failed));
  ev.SetProperty("Resolved", info.resolved);
  ev.SetProperty("Status", std::string(ActionStatusToString(info.status)));
  ev.SetProperty("TimeMs", info.duration_ms);
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) {
  local_log_.RecordEpDownloadAndRegister(info);
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  if (!ShouldSampleEvent(metadata_.app_session_guid, info.correlation_id)) {
    return;
  }
  auto ev = MakeEvent("EPDownloadAndRegister");
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("CorrelationId", info.correlation_id);
  ev.SetProperty("ProviderName", info.provider_name);
  ev.SetProperty("InitReadyState", info.init_ready_state);
  ev.SetProperty("DownloadReadyState", info.download_ready_state);
  ev.SetProperty("DownloadStatus", std::string(ActionStatusToString(info.download_status)));
  ev.SetProperty("DownloadTimeMs", info.download_duration_ms);
  ev.SetProperty("RegisterReadyState", info.register_ready_state);
  ev.SetProperty("RegisterStatus", std::string(ActionStatusToString(info.register_status)));
  ev.SetProperty("RegisterTimeMs", info.register_duration_ms);
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::RecordDownload(const DownloadInfo& info) {
  local_log_.RecordDownload(info);
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  if (!ShouldSampleEvent(metadata_.app_session_guid, info.correlation_id)) {
    return;
  }
  auto ev = MakeEvent("Download");
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("CorrelationId", info.correlation_id);
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
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::RecordCatalogFetch(const CatalogFetchInfo& info) {
  local_log_.RecordCatalogFetch(info);
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  if (!ShouldSampleEvent(metadata_.app_session_guid, info.correlation_id)) {
    return;
  }
  auto ev = MakeEvent("CatalogFetch");
  ev.SetProperty("Operation", info.operation);
  ev.SetProperty("Endpoint", info.endpoint);
  ev.SetProperty("Region", info.region);
  ev.SetProperty("Format", info.format);
  ev.SetProperty("Status", std::string(ActionStatusToString(info.status)));
  ev.SetProperty("TimeMs", info.duration_ms);
  ev.SetProperty("ModelCount", static_cast<int64_t>(info.model_count));
  ev.SetProperty("ErrorMessage", SanitizeTelemetryText(info.error_message));
  ev.SetProperty("UserAgent", info.user_agent);
  ev.SetProperty("CorrelationId", info.correlation_id);
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::RecordProcessInfo(const ProcessInfo& info) {
  local_log_.RecordProcessInfo(info);
  auto lock = LockForLogging(/*require_upload=*/false);
  if (!lock.owns_lock()) {
    return;
  }
  auto ev = MakeEvent("ProcessInfo");
  ev.SetProperty("appVersion", info.app_version);
  ev.SetProperty("appName", info.app_name);
  ev.SetProperty("osName", info.os_name);
  ev.SetProperty("osVersion", info.os_version);
  ev.SetProperty("architecture", info.cpu_arch);
  ev.SetProperty("processName", info.process_name);
  ev.SetProperty("DeviceInfo.Status", info.device_id_status);
  ev.SetProperty("cpuCount", static_cast<int64_t>(info.cpu_count));
  ev.SetProperty("totalMemoryMB", info.total_memory_mb);
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::RecordHardwareInfo(const HardwareInfo& info) {
  local_log_.RecordHardwareInfo(info);
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  auto ev = MakeEvent("HardwareInfo");
  ev.SetProperty("DeviceTypes", info.device_types);
  ev.SetProperty("ExecutionProviders", info.execution_providers);
  ev.SetProperty("DeviceTypeCount", static_cast<int64_t>(info.device_type_count));
  ev.SetProperty("ExecutionProviderCount", static_cast<int64_t>(info.execution_provider_count));
  ev.SetProperty("HasCPU", info.has_cpu);
  ev.SetProperty("HasGPU", info.has_gpu);
  ev.SetProperty("HasNPU", info.has_npu);
  SafeLog(impl_->logger, ev);
}

void OneDsTelemetry::StartSession() {
  local_log_.StartSession();
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  // LogSession(Started) opens an app-usage session; the SDK stamps ext.app.sesId
  // on subsequent events and records session duration on End.
  auto ev = MakeEvent("Session");
  impl_->logger->LogSession(SessionState::Session_Started, ev);
}

void OneDsTelemetry::EndSession() {
  local_log_.EndSession();
  auto lock = LockForLogging();
  if (!lock.owns_lock()) {
    return;
  }
  auto ev = MakeEvent("Session");
  impl_->logger->LogSession(SessionState::Session_Ended, ev);
}

}  // namespace fl
