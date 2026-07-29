// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Shared test helpers for internal API tests.
#pragma once

#include "download/download_manager.h"
#include "ep_detection/ep_detector.h"
#include "inferencing/model_load_manager.h"
#include "logger.h"

#include "utils/temp_path.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fl::test {

/// EP detector that only reports CPU — used by tests that load real models
/// without requiring GPU hardware.
class CpuOnlyEpDetector : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {{"CPU", {"CPUExecutionProvider"}}};
  }
};

/// Sink logger that drops every message. Test-only — production code that needs an `ILogger&`
/// must be wired to a real logger, not silenced.
class NullLogger : public ILogger {
 public:
  void Log(LogLevel /*level*/, std::string_view /*message*/) override {}
};

/// Returns a process-wide `NullLogger` for tests that need to satisfy an `ILogger&` parameter
/// but don't care about log output.
inline ILogger& NullLog() {
  static NullLogger instance;
  return instance;
}

/// One-stop bag of cheap fakes for tests that need to construct a leaf `Model` via
/// `FromModelInfo` but don't exercise Download/Load. Public fields by design — no invariants
/// to protect, and the field names match the matching `FromModelInfo` parameter names so the
/// call site reads as `FromModelInfo(info, "", svc.download_manager, svc.model_load_manager)`.
struct FakeServiceBindings {
  CpuOnlyEpDetector ep_detector;
  NullLogger logger;
  DownloadManager download_manager{/*cache_directory=*/"", /*catalog_region=*/"", /*max_concurrency=*/1, logger};
  ModelLoadManager model_load_manager{ep_detector, logger};
};

}  // namespace fl::test
