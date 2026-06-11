// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// No-op telemetry implementation for tests that don't care about telemetry events.
#pragma once

#include "telemetry/telemetry.h"

namespace fl::test {

class NullTelemetry : public ITelemetry {
 public:
  void RecordAction(Action /*action*/, ActionStatus /*status*/,
                    const std::string& /*user_agent*/,
                    bool /*indirect*/, int64_t /*duration_ms*/) override {}

  void RecordException(Action /*action*/, const std::exception& /*exception*/) override {}

  void RecordModelUsage(const ModelUsageInfo& /*info*/) override {}

  void RecordModelId(Action /*action*/, const std::string& /*model_id*/,
                     ActionStatus /*status*/ = ActionStatus::kSuccess,
                     const std::string& /*user_agent*/ = "") override {}

  void RecordEpDownloadAttempt(const EpDownloadAttemptInfo& /*info*/) override {}

  void RecordEpDownloadAndRegister(const EpDownloadAndRegisterInfo& /*info*/) override {}

  void RecordDownload(const DownloadInfo& /*info*/) override {}
};

}  // namespace fl::test
