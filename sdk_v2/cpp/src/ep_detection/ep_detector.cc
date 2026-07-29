// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_detector.h"

#include "ep_detection/ep_bootstrapper.h"
#include "logger.h"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <mutex>

namespace fl {

EpDetector::EpDetector(const OrtApi& ort_api, OrtEnv& ort_env,
                       std::vector<std::unique_ptr<IEpBootstrapper>> bootstrappers,
                       ILogger& logger)
    : ort_api_(ort_api),
      ort_env_(ort_env),
      bootstrappers_(std::move(bootstrappers)),
      logger_(logger) {
  // Populate both cache vectors exact-sized from bootstrappers_. After this point
  // size and element addresses (including the EpInfo::name string storage backing
  // flEpInfo::name) are immutable for the detector's lifetime — only is_registered
  // is ever updated, in place, under cache_mutex_.
  cached_eps_.reserve(bootstrappers_.size());
  cached_eps_c_.reserve(bootstrappers_.size());

  for (const auto& bs : bootstrappers_) {
    cached_eps_.push_back(EpInfo{bs->Name(), bs->IsRegistered()});
    cached_eps_c_.push_back(flEpInfo{
        FOUNDRY_LOCAL_API_VERSION,
        cached_eps_.back().name.c_str(),
        bs->IsRegistered(),
    });
  }
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
  return cached_eps_c_;
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

    logger_.Log(LogLevel::Information, "Downloading and registering EP: " + bs->Name());

    // Reuse previously downloaded EP packages unless the caller explicitly asks
    // for a forced refresh. Downloading every time made the bootstrapper
    // re-fetch and re-register EPs on every invocation.
    if (bs->DownloadAndRegister(/*force=*/false, wrapped_cb, logger_)) {
      result.registered_eps.push_back(bs->Name());

      // Update cached registration state in place under the cache lock so
      // GetDiscoverableEps[C] readers see the new value.
      std::lock_guard<std::mutex> cache_lock(cache_mutex_);
      cached_eps_[i].is_registered = true;
      cached_eps_c_[i].is_registered = true;
    } else {
      result.failed_eps.push_back(bs->Name());
      result.success = false;
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

  return result;
}

bool EpDetector::IsDownloadInProgress() const {
  return download_in_progress_;
}

}  // namespace fl
