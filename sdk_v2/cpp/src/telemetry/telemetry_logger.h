// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "telemetry/telemetry.h"
#include "logger.h"

#include <string>

namespace fl {

/// ITelemetry implementation that formats telemetry events to ILogger.
class TelemetryLogger : public ITelemetry {
 public:
  TelemetryLogger(const std::string& app_name, ILogger& logger);

  void RecordAction(Action action, ActionStatus status, const InvocationContext& context,
                    int64_t duration_ms, const std::string& model_id = {}) override;

  void RecordException(Action action, const std::exception& exception,
                       const InvocationContext& context) override;

  void RecordModelUsage(const ModelUsageInfo& info) override;
  void RecordAudioUsage(const AudioUsageInfo& info) override;

  void RecordEpDownloadAttempt(const EpDownloadAttemptInfo& info) override;
  void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& info) override;
  void RecordDownload(const DownloadInfo& info) override;
  void RecordCatalogFetch(const CatalogFetchInfo& info) override;
  void RecordProcessInfo(const ProcessInfo& info) override;
  void RecordHardwareInfo(const HardwareInfo& info) override;
  void StartSession() override;
  void EndSession() override;

 private:
  std::string app_name_;
  ILogger& logger_;
};

}  // namespace fl
