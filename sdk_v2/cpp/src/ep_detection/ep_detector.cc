// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_detector.h"

#include "ep_detection/ep_bootstrapper.h"
#include "logger.h"
#include "telemetry/ep_download_tracker.h"
#include "telemetry/telemetry.h"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <chrono>
#include <mutex>

namespace fl {

EpDetector::EpDetector(const OrtApi& ort_api, OrtEnv& ort_env,
                       std::vector<std::unique_ptr<IEpBootstrapper>> bootstrappers,
                       ILogger& logger,
                       ITelemetry* telemetry)
    : ort_api_(ort_api),
      ort_env_(ort_env),
      bootstrappers_(std::move(bootstrappers)),
      logger_(logger),
      telemetry_(telemetry) {
  // Populate the C++ cache once from bootstrappers_. Name string storage is stable
  // because cached_eps_ size is fixed for the detector's lifetime.
  cached_eps_.reserve(bootstrappers_.size());

  for (const auto& bs : bootstrappers_) {
    cached_eps_.push_back(EpInfo{bs->Name(), bs->IsRegistered()});
  }

  PublishCApiSnapshotLocked();
}

void EpDetector::PublishCApiSnapshotLocked() {
  auto& snapshot = cached_eps_c_snapshots_.emplace_back();
  snapshot.reserve(cached_eps_.size());
  for (const auto& ep : cached_eps_) {
    snapshot.push_back(flEpInfo{
        FOUNDRY_LOCAL_API_VERSION,
        ep.name.c_str(),
        ep.is_registered,
    });
  }
  current_cached_eps_c_ = &snapshot;
}

std::map<std::string, std::vector<std::string>> EpDetector::GetAvailableDevicesToEPs() const {
  // Build the result locally and return by value as the available devices may change by DownloadAndRegisterEps
  // running in parallel.
  std::map<std::string, std::vector<std::string>> devices;

  // Query ORT for all registered EP devices. Each OrtEpDevice pairs an
  // execution provider name with a hardware device (CPU/GPU/NPU).
  const OrtEpDevice* const* ep_devices = nullptr;
  size_t num_devices = 0;
  OrtStatus* status = ort_api_.GetEpDevices(&ort_env_, &ep_devices, &num_devices);

  if (status != nullptr) {
    const char* msg = ort_api_.GetErrorMessage(status);
    logger_.Log(LogLevel::Warning,
                std::string("GetEpDevices failed: ") + (msg ? msg : "unknown"));
    ort_api_.ReleaseStatus(status);

    // Fall back to a minimal CPU entry so catalog queries still work.
    devices["CPU"] = {"CPUExecutionProvider"};
    return devices;
  }

  logger_.Log(LogLevel::Debug,
              std::string("GetEpDevices: ORT reports ") + std::to_string(num_devices) + " EP device(s)");

  for (size_t i = 0; i < num_devices; ++i) {
    const OrtEpDevice* ep_device = ep_devices[i];
    const char* ep_name = ort_api_.EpDevice_EpName(ep_device);
    const OrtHardwareDevice* hw = ort_api_.EpDevice_Device(ep_device);
    OrtHardwareDeviceType hw_type = ort_api_.HardwareDevice_Type(hw);

    const char* device_key = nullptr;
    switch (hw_type) {
      case OrtHardwareDeviceType_CPU:
        device_key = "CPU";
        break;
      case OrtHardwareDeviceType_GPU:
        device_key = "GPU";
        break;
      case OrtHardwareDeviceType_NPU:
        device_key = "NPU";
        break;
      default:
        device_key = "CPU";
        break;
    }

    logger_.Log(LogLevel::Debug,
                std::string("  [") + std::to_string(i) + "] ep=" + (ep_name ? ep_name : "<null>") +
                    " device=" + device_key + " (hw_type=" + std::to_string(static_cast<int>(hw_type)) + ")");

    auto& eps = devices[device_key];

    // Avoid duplicates (same EP can appear for multiple hardware instances).
    if (std::find(eps.begin(), eps.end(), ep_name) == eps.end()) {
      eps.push_back(ep_name);
    }
  }

  // Ensure CPU is always present — ORT always has CPUExecutionProvider but
  // GetEpDevices may not list it in some configurations.
  if (devices.find("CPU") == devices.end()) {
    devices["CPU"] = {"CPUExecutionProvider"};
  }

  return devices;
}

const std::vector<EpInfo>& EpDetector::GetDiscoverableEps() const {
  // Take the cache lock for strict correctness of the is_registered field reads.
  // Vector size and element addresses are immutable after construction; only
  // is_registered fields can be mutated (by DownloadAndRegisterEps under the same
  // mutex). The lock is released when this function returns, so the snapshot may
  // be stale by the time the caller reads individual fields — that is documented
  // and acceptable.
  std::lock_guard<std::mutex> lock(cache_mutex_);
  return cached_eps_;
}

std::span<const flEpInfo> EpDetector::GetDiscoverableEpsCApi() const {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  if (current_cached_eps_c_ == nullptr) {
    return {};
  }
  return *current_cached_eps_c_;
}

EpDownloadResult EpDetector::DownloadAndRegisterEps(const std::vector<std::string>* names,
                                                    const IEpBootstrapper::ProgressCallback& progress_cb) {
  std::lock_guard<std::mutex> lock(download_mutex_);
  download_in_progress_ = true;

  // RAII: ensure the flag is reset even if an exception unwinds the stack
  struct ResetFlag {
    std::atomic<bool>& flag;
    ~ResetFlag() { flag = false; }
  } reset_guard{download_in_progress_};

  EpDownloadResult result;
  result.success = true;

  // Telemetry: time the whole call and count provider outcomes for the
  // aggregate EPDownloadAttempt event (emitted at the end below). The whole call
  // shares one correlation id; each per-provider EPDownloadAndRegister event is
  // an indirect child that reuses it.
  const auto attempt_start = std::chrono::steady_clock::now();
  const std::string telemetry_correlation_id = MakeGuidV4Hex();
  int telemetry_num_providers = 0;
  int telemetry_attempts = 0;
  int telemetry_succeeded = 0;
  int telemetry_failed = 0;
  // Some bootstrappers may already be Registered before this call; if so, the
  // EPDownloadAttempt is considered "resolved" even when no work was done.
  bool telemetry_resolved = false;

  // Track cancellation from the progress callback
  bool cancelled = false;
  IEpBootstrapper::ProgressCallback wrapped_cb;

  if (progress_cb) {
    wrapped_cb = [&progress_cb, &cancelled](const std::string& ep_name, float percent) -> bool {
      if (cancelled) {
        return false;
      }

      bool should_continue = progress_cb(ep_name, percent);

      if (!should_continue) {
        cancelled = true;
      }

      return should_continue;
    };
  }

  for (size_t i = 0; i < bootstrappers_.size(); ++i) {
    const auto& bs = bootstrappers_[i];
    if (cancelled) {
      break;
    }

    // If specific names were requested, skip bootstrappers not in the list.
    if (names != nullptr) {
      auto found = std::find(names->begin(), names->end(), bs->Name());
      if (found == names->end()) {
        continue;
      }
    }

    ++telemetry_num_providers;
    logger_.Log(LogLevel::Information, "Downloading and registering EP: " + bs->Name());

    // Per-provider EPDownloadAndRegister event via EpDownloadTracker. When
    // telemetry_ is null (e.g. unit tests instantiate EpDetector directly),
    // the tracker is skipped — the event has no fallback emitter.
    std::unique_ptr<EpDownloadTracker> tracker;
    const bool was_registered_before = bs->IsRegistered();
    if (telemetry_ != nullptr) {
      tracker = std::make_unique<EpDownloadTracker>(bs->Name(), /*user_agent=*/std::string{},
                                                    telemetry_correlation_id, *telemetry_);
      tracker->RecordInitialState(was_registered_before ? "Registered" : "NotPresent");
    }

    ++telemetry_attempts;
    // Reuse previously downloaded EP packages unless the caller explicitly asks
    // for a forced refresh. Downloading every time made the bootstrapper
    // re-fetch and re-register EPs on every invocation.
    bool ok = false;
    try {
      ok = bs->DownloadAndRegister(/*force=*/false, wrapped_cb, logger_);
    } catch (const std::exception& ex) {
      if (tracker) {
        tracker->RecordException(ex);
      }
      // Re-throw to preserve existing semantics — the wrapper RAII guard above
      // resets download_in_progress_; the tracker dtor records the EP event.
      throw;
    }

    if (ok) {
      ++telemetry_succeeded;
      telemetry_resolved = true;
      result.registered_eps.push_back(bs->Name());

      // Update cached registration state in place under the cache lock so
      // GetDiscoverableEps[C] readers see the new value.
      std::lock_guard<std::mutex> cache_lock(cache_mutex_);
      cached_eps_[i].is_registered = true;
      PublishCApiSnapshotLocked();

      if (tracker) {
        tracker->RecordDownloadComplete(ActionStatus::kSuccess, "Installed");
        tracker->RecordRegisterComplete(ActionStatus::kSuccess, "Registered");
      }
    } else {
      ++telemetry_failed;
      result.failed_eps.push_back(bs->Name());
      result.success = false;
      if (tracker) {
        // The bootstrapper conflated download + register and returned false.
        // Record both phases as kFailure so backend dashboards can tell that
        // this EP didn't reach Registered, without claiming a specific phase.
        tracker->RecordDownloadComplete(ActionStatus::kFailure,
                                        was_registered_before ? "Registered" : "NotPresent");
        tracker->RecordRegisterComplete(ActionStatus::kFailure,
                                        was_registered_before ? "Registered" : "NotPresent");
      }
    }
  }

  if (cancelled) {
    result.cancelled = true;
    result.success = false;
    result.status = "EP download cancelled by user";
  } else if (result.failed_eps.empty()) {
    result.status = "All requested EPs registered successfully";
  } else {
    result.status = "Some EPs failed to register";
  }

  if (telemetry_ != nullptr) {
    EpDownloadAttemptInfo attempt_info;
    attempt_info.correlation_id = telemetry_correlation_id;
    attempt_info.attempts = telemetry_attempts;
    attempt_info.num_providers = telemetry_num_providers;
    attempt_info.succeeded = telemetry_succeeded;
    attempt_info.failed = telemetry_failed;
    attempt_info.resolved = telemetry_resolved;
    attempt_info.status = result.success ? ActionStatus::kSuccess
                                         : (result.cancelled ? ActionStatus::kSkipped
                                                             : ActionStatus::kFailure);
    attempt_info.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - attempt_start)
                                   .count();
    telemetry_->RecordEpDownloadAttempt(attempt_info);
  }

  return result;
}

bool EpDetector::IsDownloadInProgress() const {
  return download_in_progress_;
}

}  // namespace fl
