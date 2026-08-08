// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry_logger.h"

#include "telemetry/telemetry_redaction.h"

#include <fmt/format.h>

namespace fl {

TelemetryLogger::TelemetryLogger(const std::string& app_name, ILogger& logger)
    : app_name_(app_name), logger_(logger) {
}

void TelemetryLogger::RecordAction(Action action, ActionStatus status, const InvocationContext& context,
                                   int64_t duration_ms, const std::string& model_id) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] Action AppName={} UserAgent={} CorrelationId={} Action={} Status={} "
                          "Direct={} TimeMs={} ModelId={}",
                          app_name_, context.user_agent, context.correlation_id, ActionToString(action),
                          ActionStatusToString(status), !context.indirect, duration_ms, model_id));
}

void TelemetryLogger::RecordException(Action action, const std::exception& exception,
                                      const InvocationContext& context) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] Error AppName={} UserAgent={} CorrelationId={} Action={} Exception={}",
                          app_name_, context.user_agent, context.correlation_id, ActionToString(action),
                          ScrubStringForTelemetry(exception.what())));
}

void TelemetryLogger::RecordModelUsage(const ModelUsageInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] Model AppName={} UserAgent={} CorrelationId={} ModelId={} EP={} "
                          "Stream={} Direct={} TimeToFirstTokenMs={} "
                          "TotalTimeMs={} TotalTokens={} InputTokenCount={} NumMessages={} MemoryUsedMB={} "
                          "CpuTimeMs={} GpuMemoryUsedMB={}",
                          app_name_, info.user_agent, info.correlation_id, info.model_id,
                          info.execution_provider, info.stream, !info.indirect,
                          info.time_to_first_token_ms, info.total_time_ms, info.total_tokens,
                          info.input_token_count, info.num_messages, info.memory_used_mb,
                          info.cpu_time_ms, info.gpu_memory_used_mb));
}

void TelemetryLogger::RecordAudioUsage(const AudioUsageInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] AudioModel AppName={} UserAgent={} CorrelationId={} ModelId={} EP={} "
                          "AudioSource={} Language={} Stream={} Direct={} TotalTimeMs={} TotalTokens={} "
                          "InputTokenCount={} CompletionTokenCount={} AudioDurationMs={} SampleRate={} Channels={}",
                          app_name_, info.user_agent, info.correlation_id, info.model_id, info.execution_provider,
                          info.audio_source, info.language, info.stream, !info.indirect, info.total_time_ms,
                          info.total_tokens, info.input_token_count, info.completion_token_count,
                          info.audio_duration_ms, info.sample_rate, info.channels));
}

void TelemetryLogger::RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] EPDownloadAttempt AppName={} UserAgent={} CorrelationId={} Attempts={} "
                          "NumProviders={} Succeeded={} Failed={} Resolved={} Status={} TimeMs={}",
                          app_name_, info.user_agent, info.correlation_id, info.attempts, info.num_providers,
                          info.succeeded, info.failed, info.resolved,
                          ActionStatusToString(info.status), info.duration_ms));
}

void TelemetryLogger::RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] EPDownloadAndRegister AppName={} UserAgent={} CorrelationId={} Provider={} "
                          "InitReadyState={} DownloadReadyState={} DownloadStatus={} DownloadTimeMs={} "
                          "RegisterReadyState={} RegisterStatus={} RegisterTimeMs={}",
                          app_name_, info.user_agent, info.correlation_id, info.provider_name,
                          info.init_ready_state, info.download_ready_state,
                          ActionStatusToString(info.download_status), info.download_duration_ms,
                          info.register_ready_state, ActionStatusToString(info.register_status),
                          info.register_duration_ms));
}

void TelemetryLogger::RecordDownload(const DownloadInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] Download AppName={} UserAgent={} CorrelationId={} ModelId={} Status={} "
                          "LockWaitMs={} EnumerationMs={} DownloadMs={} TotalSizeBytes={} "
                          "AlreadyCachedBytes={} FileCount={} SkippedFileCount={} "
                          "DownloadWaitResult={} MaxConcurrency={}",
                          app_name_, info.user_agent, info.correlation_id, info.model_id,
                          ActionStatusToString(info.status), info.lock_wait_ms,
                          info.enumeration_ms, info.download_ms, info.total_size_bytes,
                          info.already_cached_bytes, info.file_count, info.skipped_file_count,
                          info.download_wait_result, info.max_concurrency));
}

void TelemetryLogger::RecordCatalogFetch(const CatalogFetchInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] CatalogFetch AppName={} Operation={} Endpoint={} Region={} Format={} "
                          "Status={} TimeMs={} ModelCount={} Error={} UserAgent={} CorrelationId={}",
                          app_name_, info.operation, info.endpoint, info.region, info.format,
                          ActionStatusToString(info.status), info.duration_ms, info.model_count,
                          ScrubStringForTelemetry(info.error_message), info.user_agent, info.correlation_id));
}

void TelemetryLogger::RecordProcessInfo(const ProcessInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] ProcessInfo AppName={} AppVersion={} OsName={} "
                          "OsVersion={} CpuArch={} "
                          "ProcessName={} DeviceIdStatus={} CpuCount={} TotalMemoryMB={}",
                          app_name_, info.app_version, info.os_name, info.os_version,
                          info.cpu_arch, info.process_name, info.device_id_status, info.cpu_count,
                          info.total_memory_mb));
}

void TelemetryLogger::RecordHardwareInfo(const HardwareInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] HardwareInfo AppName={} DeviceTypes={} ExecutionProviders={} "
                          "DeviceTypeCount={} ExecutionProviderCount={} HasCPU={} HasGPU={} HasNPU={}",
                          app_name_, info.device_types, info.execution_providers, info.device_type_count,
                          info.execution_provider_count, info.has_cpu, info.has_gpu, info.has_npu));
}

void TelemetryLogger::StartSession() {
  logger_.Log(LogLevel::Debug, fmt::format("[Telemetry] SessionStart AppName={}", app_name_));
}

void TelemetryLogger::EndSession() {
  logger_.Log(LogLevel::Debug, fmt::format("[Telemetry] SessionEnd AppName={}", app_name_));
}

}  // namespace fl
