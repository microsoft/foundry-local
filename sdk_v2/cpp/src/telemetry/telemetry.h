// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/invocation_context.h"

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace fl {

/// Telemetry action identifiers. The values are stable IDs for log greppability;
/// the over-the-wire field is the human-readable name produced by ActionToString.
/// The 1DS implementation emits the string, so changing numeric values is safe.
enum class Action {
  kInvalid = 0,

  // Core lifecycle
  kCoreInitialize = 1,
  kCoreServiceStart = 2,
  kCoreServiceStop = 3,

  // Session (inference)
  kSessionCreate = 50,
  kSessionProcessRequest = 51,

  // Model management
  kModelLoad = 100,
  kModelUnload = 101,
  kModelList = 104,

  // OpenAI Chat/Audio/Embeddings APIs
  kOpenAIChatCompletions = 200,
  kOpenAIModelList = 201,
  kOpenAIModelRetrieve = 202,
  kOpenAIAudioTranscribe = 203,
  kOpenAIEmbeddings = 204,

  // OpenAI Responses API
  kOpenAIResponsesCreate = 300,
  kOpenAIResponsesGet = 301,
  kOpenAIResponsesList = 302,
  kOpenAIResponsesDelete = 303,
  kOpenAIResponsesGetInputItems = 304,

  // EP catalog operations
  kEpDownloadAttempt = 500,         // Wraps the entire DownloadAndRegisterEps call
  kEpDownloadAndRegister = 501,     // One per-provider attempt within DownloadAndRegisterEps

  // Model file download
  kModelFileDownload = 600,         // Wraps the per-model DownloadManager flow

  // EP runtime usage (TimeToFirstToken / total tokens / memory)
  kModelInference = 700,            // The "Model" event in the C# implementation

};

/// Status of a tracked telemetry action.
enum class ActionStatus {
  kFailure = 0,    // Internal failure while executing valid work
  kSuccess,
  kInvalid,
  kSkipped,
  kClientError,    // Rejected due to invalid client input (maps to HTTP 4xx) — not a service fault
  kCanceled,
  kDependencyFailure,
  kTimeout,
};

/// Convert Action to human-readable string.
std::string_view ActionToString(Action action);

/// Convert ActionStatus to human-readable string.
std::string_view ActionStatusToString(ActionStatus status);

/// Classify a thrown exception into an action status.
ActionStatus ActionStatusFromException(const std::exception& exception);

/// Payload for the EPDownloadAttempt event — emitted once per DownloadAndRegisterEps call.
struct EpDownloadAttemptInfo {
  std::string user_agent;
  std::string correlation_id;
  int attempts = 0;            // Total per-provider attempts made
  int num_providers = 0;       // Number of providers requested
  int succeeded = 0;           // Number of providers that registered successfully
  int failed = 0;              // Number of providers that failed
  bool resolved = false;       // True if at least one provider became Registered
  ActionStatus status = ActionStatus::kInvalid;
  int64_t duration_ms = 0;
};

/// Payload for the EPDownloadAndRegister event — emitted once per provider attempt.
struct EpDownloadAndRegisterInfo {
  std::string user_agent;
  std::string correlation_id;
  std::string provider_name;
  std::string init_ready_state;        // EP state before this call (e.g. "NotPresent")
  std::string download_ready_state;    // EP state after the download phase (e.g. "Installed")
  ActionStatus download_status = ActionStatus::kInvalid;
  int64_t download_duration_ms = 0;
  std::string register_ready_state;    // EP state after the register phase (e.g. "Registered")
  ActionStatus register_status = ActionStatus::kInvalid;
  int64_t register_duration_ms = 0;
};

/// Payload for the Model event — emitted once per inference completion with token / memory metrics.
struct ModelUsageInfo {
  std::string model_id;
  std::string execution_provider;
  std::string user_agent;
  std::string correlation_id;
  bool stream = false;                  // True if the inference was streamed (SSE) vs a single response
  bool indirect = false;                // True if the inference was driven by another action (e.g. an HTTP route)
  int64_t time_to_first_token_ms = -1;   // -1 if not measured
  int64_t total_time_ms = 0;
  int32_t total_tokens = 0;
  int32_t input_token_count = 0;
  uint64_t num_messages = 0;
  int64_t memory_used_mb = -1;          // -1 if not measured
  int64_t cpu_time_ms = -1;             // -1 if not measured
  int64_t gpu_memory_used_mb = -1;      // -1 if not measured
};

/// Payload for the AudioModel event — emitted once per successful audio inference with audio-specific metrics.
struct AudioUsageInfo {
  std::string model_id;
  std::string execution_provider;
  std::string user_agent;
  std::string correlation_id;
  std::string audio_source;              // "file", "openai_json_file", or "streaming_pcm"; never a path/name
  std::string language;                  // Request/session language hint when provided; empty if unset
  bool stream = false;
  bool indirect = false;
  int64_t total_time_ms = 0;
  int32_t total_tokens = 0;
  int32_t input_token_count = 0;
  int32_t completion_token_count = 0;
  int64_t audio_duration_ms = -1;        // -1 if not measured
  int32_t sample_rate = 0;               // 0 if not known
  int32_t channels = 0;                  // 0 if not known
};

/// Payload for the Download event — emitted once per DownloadManager::DownloadModel call.
struct DownloadInfo {
  std::string model_id;
  std::string user_agent;
  std::string correlation_id;
  ActionStatus status = ActionStatus::kInvalid;
  int64_t lock_wait_ms = 0;
  int64_t enumeration_ms = 0;
  int64_t download_ms = 0;
  int64_t total_size_bytes = 0;
  int64_t already_cached_bytes = 0;
  int32_t file_count = 0;
  int32_t skipped_file_count = 0;
  std::string download_wait_result;    // e.g. "Completed", "TimedOut", "AlreadyHeld"
  int32_t max_concurrency = 0;
};

/// Payload for the CatalogFetch event — emitted for primary catalog refreshes
/// and cache-miss/cached-id lookups against a model catalog source.
struct CatalogFetchInfo {
  std::string operation;       // "FetchAll" (full catalog) or "FetchByIds" (cached-id lookup)
  std::string endpoint;        // catalog host (e.g. "ai.azure.com"), or "static" for the embedded snapshot
  std::string region;          // region parsed from the catalog URL (e.g. "eastus"); empty if not present
  std::string format;          // catalog API path/version after the region (e.g. "ux/v1.0")
  ActionStatus status = ActionStatus::kInvalid;
  int64_t duration_ms = 0;
  int32_t model_count = 0;     // models returned by this access
  std::string error_message;   // populated on failure
  std::string user_agent;
  std::string correlation_id;  // shared across the accesses of one catalog refresh
};

/// Payload for coarse hardware/accelerator availability at startup.
struct HardwareInfo {
  bool has_cpu = false;
  bool has_gpu = false;
  bool has_npu = false;
  int32_t device_type_count = 0;
  int32_t execution_provider_count = 0;
  std::string device_types;         // comma-separated coarse device classes, e.g. "CPU,GPU"
  std::string execution_providers;  // comma-separated provider names, e.g. "CPUExecutionProvider,CUDAExecutionProvider"
};

/// Payload for the ProcessInfo event — emitted once during process startup when telemetry is not CI-suppressed.
struct ProcessInfo {
  std::string app_name;
  std::string app_version;
  std::string os_name;
  std::string os_version;
  std::string cpu_arch;
  std::string process_name;
  std::string device_id_status;
  int32_t cpu_count = 0;
  int64_t total_memory_mb = -1;
};

/// Abstract telemetry interface.
/// Implementations may send events to a telemetry backend (1DS, ETW, OpenTelemetry, …)
/// or simply log them. The OneDsTelemetry implementation sends to 1DS; the
/// TelemetryLogger stub formats them to the ILogger sink.
class ITelemetry {
 public:
  virtual ~ITelemetry() = default;

  void RecordAction(Action action, ActionStatus status, const std::string& user_agent,
                    bool indirect, int64_t duration_ms) {
    auto context = InvocationContext::Direct(user_agent);
    context.indirect = indirect;
    RecordAction(action, status, context, duration_ms);
  }

  /// Record a completed action with timing and status. The context carries the
  /// user agent, the correlation id grouping this operation's events, and whether
  /// the action was indirect (triggered by another action). ModelId is included
  /// when the action resolved a model.
  virtual void RecordAction(Action action, ActionStatus status,
                            const InvocationContext& context, int64_t duration_ms,
                            const std::string& model_id = {}) = 0;

  /// Record an exception associated with an action.
  virtual void RecordException(Action action, const std::exception& exception,
                               const InvocationContext& context) = 0;

  void RecordException(Action action, const std::exception& exception) {
    RecordException(action, exception, InvocationContext::Direct());
  }

  /// Record model usage metrics after inference (Model event).
  virtual void RecordModelUsage(const ModelUsageInfo& info) = 0;

  /// Record audio-specific inference metrics after audio inference (AudioModel event).
  virtual void RecordAudioUsage(const AudioUsageInfo& /*info*/) {}

  /// Record the result of a DownloadAndRegisterEps call (EPDownloadAttempt event).
  virtual void RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) = 0;

  /// Record one EP provider's download+register attempt (EPDownloadAndRegister event).
  virtual void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) = 0;

  /// Record one model file download (Download event).
  virtual void RecordDownload(const DownloadInfo& info) = 0;

  /// Record one access to a model catalog source (CatalogFetch event).
  virtual void RecordCatalogFetch(const CatalogFetchInfo& info) = 0;

  /// Record one process/system metadata snapshot. Default no-op for test fakes
  /// and embedders that do not care about startup telemetry.
  virtual void RecordProcessInfo(const ProcessInfo& /*info*/) {}

  /// Record coarse hardware/accelerator availability. Default no-op for test fakes
  /// and embedders that do not care about startup telemetry.
  virtual void RecordHardwareInfo(const HardwareInfo& /*info*/) {}

  /// Mark the start / end of an app-usage session via 1DS LogSession. Between
  /// these the SDK stamps a session id (ext.app.sesId) on events and emits a
  /// session start/end with duration — standard, cross-platform engagement
  /// sessions. Default no-op for the non-1DS implementations.
  virtual void StartSession() {}
  virtual void EndSession() {}
};

}  // namespace fl
