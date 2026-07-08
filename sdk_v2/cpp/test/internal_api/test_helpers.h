// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Shared test helpers for internal API tests.
#pragma once

#include "download/download_manager.h"
#include "ep_detection/ep_detector.h"
#include "inferencing/model_load_manager.h"
#include "logger.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

namespace fl::test {

/// Current process id. CTest (gtest_discover_tests) launches a separate process per test, so
/// temp paths must include the pid to stay unique across concurrent test processes. process.h
/// is used instead of windows.h so callers that use std::min/std::max aren't broken by its macros.
inline long CurrentPid() {
#ifdef _WIN32
  return ::_getpid();
#else
  return static_cast<long>(::getpid());
#endif
}

/// Build a unique path under the system temp directory as `<prefix><pid>_<counter>`. The pid
/// separates concurrent test processes and the per-process atomic counter separates callers
/// within one process, so no two live temp paths collide — no randomness required.
inline std::filesystem::path MakeUniqueTempPath(const std::string& prefix) {
  static std::atomic<uint64_t> counter{0};
  return std::filesystem::temp_directory_path() /
         (prefix + std::to_string(CurrentPid()) + "_" +
          std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
}

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
