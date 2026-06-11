// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <foundry_local/foundry_local_c.h>

#include "ep_detection/ep_types.h"
#include "ep_detection/ep_bootstrapper.h"

// Forward declarations — avoid pulling onnxruntime_c_api.h into this header.
struct OrtApi;
struct OrtEnv;

namespace fl {

class ILogger;
class ITelemetry;

/// Interface for detecting available hardware devices and execution providers.
class IEpDetector {
 public:
  virtual ~IEpDetector() = default;

  /// Returns a map of device name → available execution providers.
  /// Example: { "CPU" → ["CPUExecutionProvider"], "GPU" → ["CudaExecutionProvider"] }
  virtual std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const = 0;

  /// Returns metadata for all discoverable EPs (those with bootstrappers).
  /// Default: empty — no discoverable EPs beyond what's already registered.
  virtual const std::vector<EpInfo>& GetDiscoverableEps() const {
    static const std::vector<EpInfo> empty;
    return empty;
  }

  /// C ABI view of the discoverable EPs. Pointers, struct addresses, and name
  /// strings are stable for the detector's lifetime. is_registered values
  /// reflect a recent snapshot — concurrent DownloadAndRegisterEps may have
  /// updated them since the call returned.
  /// Default: empty span.
  virtual std::span<const flEpInfo> GetDiscoverableEpsCApi() const {
    return {};
  }

  /// Downloads and registers EPs. Blocking call with progress reporting.
  /// @param names  EP names to register. nullptr = all discoverable EPs.
  /// @param progress_cb  Called with (ep_name, percent). Returns false to cancel.
  /// Default: returns failure with "EP download not supported" status.
  virtual EpDownloadResult DownloadAndRegisterEps(const std::vector<std::string>* /*names*/,
                                                  const IEpBootstrapper::ProgressCallback& /*progress_cb*/) {
    return EpDownloadResult{false, false, "EP download not supported", {}, {}};
  }

  /// Whether an EP download/registration operation is currently in progress.
  /// Default: false.
  virtual bool IsDownloadInProgress() const { return false; }
};

/// Real EP detector that orchestrates bootstrappers for EP discovery and registration.
/// Replaces the stub for production use.
class EpDetector : public IEpDetector {
 public:
  /// @param ort_api  The ORT C API table (from OrtGetApiBase()->GetApi).
  /// @param ort_env  The ORT environment singleton.
  /// @param bootstrappers  EP bootstrappers for download/registration.
  /// @param logger  Logger instance.
  /// @param telemetry  Optional telemetry sink. When non-null, DownloadAndRegisterEps
  ///                   emits one EPDownloadAndRegister event per provider via
  ///                   EpDownloadTracker, plus one aggregate EPDownloadAttempt
  ///                   event for the whole call. Pass nullptr in tests / when no
  ///                   telemetry sink is available.
  EpDetector(const OrtApi& ort_api, OrtEnv& ort_env,
             std::vector<std::unique_ptr<IEpBootstrapper>> bootstrappers,
             ILogger& logger,
             ITelemetry* telemetry = nullptr);
  ~EpDetector() override = default;

  // Non-copyable, non-movable (owns bootstrappers and mutex state)
  EpDetector(const EpDetector&) = delete;
  EpDetector& operator=(const EpDetector&) = delete;

  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override;
  const std::vector<EpInfo>& GetDiscoverableEps() const override;
  std::span<const flEpInfo> GetDiscoverableEpsCApi() const override;
  EpDownloadResult DownloadAndRegisterEps(const std::vector<std::string>* names,
                                          const IEpBootstrapper::ProgressCallback& progress_cb) override;
  bool IsDownloadInProgress() const override;

 private:
  const OrtApi& ort_api_;
  OrtEnv& ort_env_;
  std::vector<std::unique_ptr<IEpBootstrapper>> bootstrappers_;
  ILogger& logger_;
  ITelemetry* telemetry_ = nullptr;
  std::mutex download_mutex_;
  std::atomic<bool> download_in_progress_{false};
  mutable std::mutex cache_mutex_;
  // Populated once in the constructor; size and element addresses (including name strings)
  // are stable for the detector's lifetime. Only is_registered fields are mutated, under
  // cache_mutex_. cached_eps_c_ mirrors cached_eps_ for the C ABI.
  std::vector<EpInfo> cached_eps_;
  std::vector<flEpInfo> cached_eps_c_;
};

}  // namespace fl
