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

  /// Returns a recent snapshot of all discoverable EPs (those with bootstrappers).
  /// The returned reference is valid until this method is next called on the same thread.
  /// Default: empty — no discoverable EPs beyond what's already registered.
  virtual const std::vector<EpInfo>& GetDiscoverableEps() const {
    static const std::vector<EpInfo> empty;
    return empty;
  }

  /// C ABI view of a recent discoverable EPs snapshot. Pointers are valid until
  /// this method is next called on the same thread.
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
  /// @param telemetry  Telemetry sink. DownloadAndRegisterEps emits one EPDownloadAndRegister event per provider via
  ///                   EpDownloadTracker, plus one aggregate EPDownloadAttempt event for the whole call.
  EpDetector(const OrtApi& ort_api, OrtEnv& ort_env,
             std::vector<std::unique_ptr<IEpBootstrapper>> bootstrappers,
             ILogger& logger,
             ITelemetry& telemetry);
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
  ITelemetry& telemetry_;
  std::mutex download_mutex_;
  std::atomic<bool> download_in_progress_{false};
  mutable std::mutex cache_mutex_;
  // Populated once in the constructor; mutated only under cache_mutex_ and copied
  // into per-thread snapshots for callers.
  std::vector<EpInfo> cached_eps_;
};

}  // namespace fl
