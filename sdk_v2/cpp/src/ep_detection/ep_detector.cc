// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_detector.h"

#include "ep_detection/ep_bootstrapper.h"
#include "logger.h"
#include "model_info.h"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <vector>

namespace fl {

namespace {

struct EpDeviceSnapshot {
  std::vector<const OrtEpDevice*> devices;
};

std::optional<EpDeviceSnapshot> TryGetEpDeviceSnapshot(const OrtApi& ort_api,
                                                       OrtEnv& ort_env,
                                                       ILogger& logger) {
  const OrtEpDevice* const* ep_devices = nullptr;
  size_t num_devices = 0;
  OrtStatus* status = ort_api.GetEpDevices(&ort_env, &ep_devices, &num_devices);

  if (status != nullptr) {
    const char* message = ort_api.GetErrorMessage(status);
    logger.Log(LogLevel::Warning,
               std::string("GetEpDevices failed: ") + (message ? message : "unknown"));
    ort_api.ReleaseStatus(status);
    return std::nullopt;
  }

  EpDeviceSnapshot snapshot;
  snapshot.devices.reserve(num_devices);
  for (size_t i = 0; i < num_devices; ++i) {
    snapshot.devices.push_back(ep_devices[i]);
  }

  logger.Log(LogLevel::Debug,
             std::string("GetEpDevices: ORT reports ") + std::to_string(snapshot.devices.size()) +
                 " EP device(s)");

  return snapshot;
}

const char* DeviceKey(OrtHardwareDeviceType device_type) {
  switch (device_type) {
    case OrtHardwareDeviceType_CPU:
      return "CPU";
    case OrtHardwareDeviceType_GPU:
      return "GPU";
    case OrtHardwareDeviceType_NPU:
      return "NPU";
    default:
      return "CPU";
  }
}

DeviceType ToDeviceType(OrtHardwareDeviceType device_type) {
  switch (device_type) {
    case OrtHardwareDeviceType_CPU:
      return DeviceType::kCPU;
    case OrtHardwareDeviceType_GPU:
      return DeviceType::kGPU;
    case OrtHardwareDeviceType_NPU:
      return DeviceType::kNPU;
    default:
      return DeviceType::kNotSet;
  }
}

CompiledModelCompatibility ToCompiledModelCompatibility(OrtCompiledModelCompatibility compatibility) {
  switch (compatibility) {
    case OrtCompiledModelCompatibility_EP_SUPPORTED_OPTIMAL:
      return CompiledModelCompatibility::kSupportedOptimal;
    case OrtCompiledModelCompatibility_EP_SUPPORTED_PREFER_RECOMPILATION:
      return CompiledModelCompatibility::kSupportedPreferRecompilation;
    case OrtCompiledModelCompatibility_EP_UNSUPPORTED:
      return CompiledModelCompatibility::kUnsupported;
    case OrtCompiledModelCompatibility_EP_NOT_APPLICABLE:
    default:
      return CompiledModelCompatibility::kUnknown;
  }
}

}  // namespace

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

  const auto snapshot = TryGetEpDeviceSnapshot(ort_api_, ort_env_, logger_);
  if (!snapshot.has_value()) {
    // Fall back to a minimal CPU entry so catalog queries still work.
    devices["CPU"] = {"CPUExecutionProvider"};
    return devices;
  }

  for (size_t i = 0; i < snapshot->devices.size(); ++i) {
    const OrtEpDevice* ep_device = snapshot->devices[i];
    const char* ep_name = ort_api_.EpDevice_EpName(ep_device);
    const OrtHardwareDevice* hw = ort_api_.EpDevice_Device(ep_device);
    const auto hw_type = hw != nullptr ? ort_api_.HardwareDevice_Type(hw) : OrtHardwareDeviceType_CPU;
    const char* device_key = DeviceKey(hw_type);

    logger_.Log(LogLevel::Debug,
                std::string("  [") + std::to_string(i) + "] ep=" + (ep_name ? ep_name : "<null>") +
                    " device=" + device_key + " (hw_type=" + std::to_string(static_cast<int>(hw_type)) + ")");

    if (ep_name == nullptr) {
      continue;
    }

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

  // Expand the requested set for EPs that depend on another EP's runtime. NvTensorRTRTX (a WinML EP)
  // reuses the GenAI CUDA library that ships in the CUDA EP bundle, so requesting it by name must also
  // register the CUDA EP. Only applies when both bootstrappers exist: the NvTensorRTRTX bootstrapper
  // (so we don't act on an unknown name on hosts without it, e.g. Linux) and the CUDA bootstrapper
  // (i.e. an NVIDIA GPU is present).
  std::vector<std::string> expanded_names;
  if (names != nullptr) {
    constexpr const char* kTrtRtxEp = "NvTensorRTRTXExecutionProvider";
    constexpr const char* kCudaEp = "CUDAExecutionProvider";
    const bool trtrtx_requested = std::find(names->begin(), names->end(), kTrtRtxEp) != names->end();
    const bool cuda_requested = std::find(names->begin(), names->end(), kCudaEp) != names->end();
    const bool has_trtrtx_bootstrapper =
        std::any_of(bootstrappers_.begin(), bootstrappers_.end(),
                    [&](const auto& bs) { return bs->Name() == kTrtRtxEp; });
    const bool has_cuda_bootstrapper =
        std::any_of(bootstrappers_.begin(), bootstrappers_.end(),
                    [&](const auto& bs) { return bs->Name() == kCudaEp; });
    if (trtrtx_requested && has_trtrtx_bootstrapper && !has_cuda_bootstrapper) {
      expanded_names = *names;
      std::erase(expanded_names, kTrtRtxEp);
      names = &expanded_names;
      result.failed_eps.emplace_back(kTrtRtxEp);
      result.success = false;
      logger_.Log(LogLevel::Warning, "NvTensorRTRTX EP requires the CUDA EP, but CUDA is not available");
    } else if (trtrtx_requested && has_trtrtx_bootstrapper && !cuda_requested) {
      expanded_names = *names;
      expanded_names.emplace_back(kCudaEp);
      names = &expanded_names;
      logger_.Log(LogLevel::Information,
                  "Auto-registering CUDA EP alongside NvTensorRTRTX (shared GenAI CUDA library)");
    }
  }

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

CompiledModelCompatibility EpDetector::GetModelCompatibilityForEpDevices(
    std::string_view execution_provider,
    std::optional<DeviceType> device_type,
    std::string_view compatibility_string) const {
  if (execution_provider.empty() || compatibility_string.empty()) {
    return CompiledModelCompatibility::kUnknown;
  }

  // Collapse the optional into plain locals up front. Dereferencing it inside the device loop below trips a
  // -Wmaybe-uninitialized false positive on GCC.
  DeviceType required_device_type = DeviceType::kNotSet;
  if (device_type.has_value()) {
    required_device_type = *device_type;
  }

  const bool filter_by_device = required_device_type != DeviceType::kNotSet;

  const auto snapshot = TryGetEpDeviceSnapshot(ort_api_, ort_env_, logger_);
  if (!snapshot.has_value()) {
    return CompiledModelCompatibility::kUnknown;
  }

  std::vector<const OrtEpDevice*> matching_devices;
  for (const OrtEpDevice* ep_device : snapshot->devices) {
    if (ep_device == nullptr) {
      continue;
    }

    const char* ep_name = ort_api_.EpDevice_EpName(ep_device);
    if (ep_name == nullptr || execution_provider != ep_name) {
      continue;
    }

    if (filter_by_device) {
      const OrtHardwareDevice* hw_device = ort_api_.EpDevice_Device(ep_device);
      const auto hw_type = hw_device != nullptr ? ort_api_.HardwareDevice_Type(hw_device)
                                                : OrtHardwareDeviceType_CPU;
      if (ToDeviceType(hw_type) != required_device_type) {
        continue;
      }
    }

    matching_devices.push_back(ep_device);
  }

  if (matching_devices.empty()) {
    logger_.Log(LogLevel::Debug,
                std::string("GetModelCompatibilityForEpDevices: no registered devices matched EP '") +
                    std::string(execution_provider) + "'.");
    return CompiledModelCompatibility::kUnknown;
  }

  std::string compatibility_copy(compatibility_string);
  OrtCompiledModelCompatibility ort_compatibility = OrtCompiledModelCompatibility_EP_NOT_APPLICABLE;
  OrtStatus* status = ort_api_.GetModelCompatibilityForEpDevices(
      matching_devices.data(),
      matching_devices.size(),
      compatibility_copy.c_str(),
      &ort_compatibility);

  if (status != nullptr) {
    const char* message = ort_api_.GetErrorMessage(status);
    logger_.Log(LogLevel::Warning,
                std::string("GetModelCompatibilityForEpDevices failed for EP '") +
                    std::string(execution_provider) + "': " + (message ? message : "unknown"));
    ort_api_.ReleaseStatus(status);
    return CompiledModelCompatibility::kUnknown;
  }

  return ToCompiledModelCompatibility(ort_compatibility);
}

bool EpDetector::PrepareForModelLoad(std::string_view ep_name) {
  auto it = std::find_if(bootstrappers_.begin(), bootstrappers_.end(),
                         [&](const auto& bootstrapper) { return bootstrapper->Name() == ep_name; });
  return it == bootstrappers_.end() || (*it)->PrepareForModelLoad(logger_);
}

}  // namespace fl
