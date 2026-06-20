// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry.h"
#include "logger.h"

#include <string>

namespace fl {

/// ITelemetry implementation that formats telemetry events to ILogger.
/// Used:
///   1. As the fallback when no 1DS backend is available (cpp-client-telemetry
///      not configured, or FOUNDRY_LOCAL_USE_TELEMETRY=OFF).
///   2. Inside OneDsTelemetry to provide local diagnostic logging in addition
///      to the 1DS upload.
/// In both cases, the local logger receives every event regardless of CI /
/// FOUNDRY_TESTING_MODE state — those flags only gate the 1DS upload.
class TelemetryLogger : public ITelemetry {
 public:
  TelemetryLogger(const std::string& app_name, ILogger& logger);

  void RecordAction(Action action, ActionStatus status, const InvocationContext& context,
                    int64_t duration_ms) override;

  void RecordException(Action action, const std::exception& exception,
                       const InvocationContext& context) override;

  void RecordModelUsage(const ModelUsageInfo& info) override;

  void RecordModelId(Action action, const std::string& model_id,
                     ActionStatus status, const InvocationContext& context) override;

  void RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) override;
  void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) override;
  void RecordDownload(const DownloadInfo& info) override;
  void RecordCatalogFetch(const CatalogFetchInfo& info) override;

 private:
  std::string app_name_;
  ILogger& logger_;
};

}  // namespace fl
