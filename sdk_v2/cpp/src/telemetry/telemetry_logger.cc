// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry_logger.h"

#include <fmt/format.h>

namespace fl {

TelemetryLogger::TelemetryLogger(const std::string& app_name, ILogger& logger)
    : app_name_(app_name), logger_(logger) {
}

void TelemetryLogger::RecordAction(Action action, ActionStatus status, const std::string& user_agent,
                                   bool indirect, int64_t duration_ms) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] Action AppName={} UserAgent={} Action={} Status={} Direct={} TimeMs={}",
                          app_name_, user_agent, ActionToString(action),
                          ActionStatusToString(status), !indirect, duration_ms));
}

void TelemetryLogger::RecordException(Action action, const std::exception& exception) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] Error AppName={} Action={} Exception={}",
                          app_name_, ActionToString(action), exception.what()));
}

void TelemetryLogger::RecordException(Action action, const std::exception& exception,
                                      const std::string& user_agent) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] Error AppName={} UserAgent={} Action={} Exception={}",
                          app_name_, user_agent, ActionToString(action), exception.what()));
}

void TelemetryLogger::RecordModelUsage(const ModelUsageInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] Model AppName={} UserAgent={} ModelId={} EP={} TimeToFirstTokenMs={} "
                          "TotalTimeMs={} TotalTokens={} InputTokenCount={} NumMessages={} MemoryUsedMB={} "
                          "CpuTimeMs={} GpuMemoryUsedMB={}",
                          app_name_, info.user_agent, info.model_id, info.execution_provider,
                          info.time_to_first_token_ms, info.total_time_ms, info.total_tokens,
                          info.input_token_count, info.num_messages, info.memory_used_mb,
                          info.cpu_time_ms, info.gpu_memory_used_mb));
}

void TelemetryLogger::RecordModelId(Action action, const std::string& model_id,
                                    ActionStatus status, const std::string& user_agent) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] ModelId AppName={} UserAgent={} Action={} ModelId={} Status={}",
                          app_name_, user_agent, ActionToString(action), model_id,
                          ActionStatusToString(status)));
}

void TelemetryLogger::RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] EPDownloadAttempt AppName={} UserAgent={} Attempts={} "
                          "NumProviders={} Succeeded={} Failed={} Resolved={} Status={} TimeMs={}",
                          app_name_, info.user_agent, info.attempts, info.num_providers,
                          info.succeeded, info.failed, info.resolved,
                          ActionStatusToString(info.status), info.duration_ms));
}

void TelemetryLogger::RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] EPDownloadAndRegister AppName={} UserAgent={} Provider={} "
                          "InitReadyState={} DownloadReadyState={} DownloadStatus={} DownloadTimeMs={} "
                          "RegisterReadyState={} RegisterStatus={} RegisterTimeMs={}",
                          app_name_, info.user_agent, info.provider_name,
                          info.init_ready_state, info.download_ready_state,
                          ActionStatusToString(info.download_status), info.download_duration_ms,
                          info.register_ready_state, ActionStatusToString(info.register_status),
                          info.register_duration_ms));
}

void TelemetryLogger::RecordDownload(const DownloadInfo& info) {
  logger_.Log(LogLevel::Debug,
              fmt::format("[Telemetry] Download AppName={} UserAgent={} ModelId={} Status={} "
                          "LockWaitMs={} EnumerationMs={} DownloadMs={} TotalSizeBytes={} "
                          "AlreadyCachedBytes={} FileCount={} SkippedFileCount={} "
                          "DownloadWaitResult={} MaxConcurrency={}",
                          app_name_, info.user_agent, info.model_id,
                          ActionStatusToString(info.status), info.lock_wait_ms,
                          info.enumeration_ms, info.download_ms, info.total_size_bytes,
                          info.already_cached_bytes, info.file_count, info.skipped_file_count,
                          info.download_wait_result, info.max_concurrency));
}

}  // namespace fl
