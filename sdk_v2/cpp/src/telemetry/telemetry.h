// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

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
  kModelDownload = 102,
  kModelDelete = 103,
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

  // Audio
  kCoreAudioTranscribe = 400,

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
  kFailure = 0,
  kSuccess,
  kInvalid,
  kSkipped,
};

/// Convert Action to human-readable string.
std::string_view ActionToString(Action action);

/// Convert ActionStatus to human-readable string.
std::string_view ActionStatusToString(ActionStatus status);

/// Payload for the EPDownloadAttempt event — emitted once per DownloadAndRegisterEps call.
struct EpDownloadAttemptInfo {
  std::string user_agent;
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
  int64_t time_to_first_token_ms = 0;
  int64_t total_time_ms = 0;
  int32_t total_tokens = 0;
  int32_t input_token_count = 0;
  uint64_t num_messages = 0;
  int64_t memory_used_mb = -1;          // -1 if not measured
  int64_t cpu_time_ms = -1;             // -1 if not measured
  int64_t gpu_memory_used_mb = -1;      // -1 if not measured
};

/// Payload for the Download event — emitted once per DownloadManager::DownloadModel call.
struct DownloadInfo {
  std::string model_id;
  std::string user_agent;
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

/// Abstract telemetry interface.
/// Implementations may send events to a telemetry backend (1DS, ETW, OpenTelemetry, …)
/// or simply log them. The OneDsTelemetry implementation sends to 1DS; the
/// TelemetryLogger stub formats them to the ILogger sink.
class ITelemetry {
 public:
  virtual ~ITelemetry() = default;

  /// Record a completed action with timing and status.
  virtual void RecordAction(Action action, ActionStatus status,
                            const std::string& user_agent,
                            bool indirect, int64_t duration_ms) = 0;

  /// Record an exception associated with an action.
  virtual void RecordException(Action action, const std::exception& exception) = 0;

  /// Record an exception associated with an action, with an optional user agent.
  /// Default forwards to RecordException(action, exception) for implementations
  /// that don't yet propagate the user agent.
  virtual void RecordException(Action action, const std::exception& exception,
                               const std::string& /*user_agent*/) {
    RecordException(action, exception);
  }

  /// Record model usage metrics after inference (Model event).
  virtual void RecordModelUsage(const ModelUsageInfo& info) = 0;

  /// Record which model was used for an action (ModelId event).
  virtual void RecordModelId(Action action, const std::string& model_id,
                             ActionStatus status = ActionStatus::kSuccess,
                             const std::string& user_agent = "") = 0;

  /// Record the result of a DownloadAndRegisterEps call (EPDownloadAttempt event).
  virtual void RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) = 0;

  /// Record one EP provider's download+register attempt (EPDownloadAndRegister event).
  virtual void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) = 0;

  /// Record one model file download (Download event).
  virtual void RecordDownload(const DownloadInfo& info) = 0;
};

}  // namespace fl
