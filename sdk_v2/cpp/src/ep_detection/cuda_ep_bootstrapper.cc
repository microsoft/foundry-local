// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/cuda_ep_bootstrapper.h"

#include "ep_detection/cuda_ep_manifest.h"
#include "ep_detection/ep_utils.h"
#include "ep_detection/nvml_gpu_detector.h"
#include "logger.h"
#include "utils.h"

#include <fmt/format.h>

#include <filesystem>
#include <string>

#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace {

constexpr const char* kLockFileName = "cuda-ep.lock";
constexpr int kMaxInstallAttempts = 5;
constexpr const char* kRegistrationName = "CUDAExecutionProvider";
constexpr const char* kCudaProviderOverrideEnv = "FOUNDRY_LOCAL_CUDA_EP_LIBRARY";
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
constexpr const char* kGenAiCudaLibrary = "libonnxruntime-genai-cuda.so";
#endif

fl::CudaEpPlatform HostCudaEpPlatform() {
#if defined(_WIN32) && defined(_M_ARM64)
  return fl::CudaEpPlatform::WindowsArm64;
#elif defined(_WIN32) && defined(_M_X64)
  return fl::CudaEpPlatform::WindowsX64;
#elif defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
  return fl::CudaEpPlatform::LinuxX64;
#elif defined(__linux__) && defined(__aarch64__) && !defined(__ANDROID__)
  return fl::CudaEpPlatform::LinuxArm64;
#else
  return fl::CudaEpPlatform::Unsupported;
#endif
}

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
bool LoadGenAiCudaLibrary(const std::filesystem::path& path, void*& handle, fl::ILogger& logger) {
  if (handle) {
    return true;
  }

  dlerror();
  handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
  if (!handle) {
    const char* error = dlerror();
    logger.Log(fl::LogLevel::Warning,
               fmt::format("CUDA EP: failed to load '{}' ({})", path.string(), error ? error : "unknown error"));
    return false;
  }

  return true;
}
#endif

}  // anonymous namespace

namespace fl {

CudaEpBootstrapper::CudaEpBootstrapper(std::string root_dir, EpRegistrationCallback register_ep)
    : register_ep_(std::move(register_ep)), installer_(std::filesystem::path(root_dir), kLockFileName, "CUDA EP") {}

CudaEpBootstrapper::~CudaEpBootstrapper() {
#if defined(__linux__) && !defined(__ANDROID__)
  if (genai_cuda_handle_) {
    dlclose(genai_cuda_handle_);
  }
#endif
}

const std::string& CudaEpBootstrapper::Name() const { return name_; }

bool CudaEpBootstrapper::IsRegistered() const { return registered_; }

bool CudaEpBootstrapper::DownloadAndRegister(bool force, const ProgressCallback& progress_cb, ILogger& logger) {
  if (registered_ && !force) {
    if (progress_cb) {
      progress_cb(name_, 100.0f);
    }
    return true;
  }

  if (!force && attempts_ >= kMaxInstallAttempts) {
    logger.Log(LogLevel::Warning, "CUDA EP: max install attempts reached");
    return false;
  }

  attempts_++;

  try {
    auto override_path = Utils::GetEnv(kCudaProviderOverrideEnv);
    if (override_path.has_value() && !override_path->empty()) {
      std::filesystem::path provider_path = std::filesystem::absolute(*override_path);

      if (!std::filesystem::exists(provider_path)) {
        logger.Log(LogLevel::Warning, fmt::format("CUDA EP: {} set but file does not exist ({})",
                                                  kCudaProviderOverrideEnv, provider_path.string()));
        return false;
      }

      if (progress_cb && !progress_cb(name_, kEpReadyToRegisterProgress)) {
        return false;
      }

#ifdef _WIN32
      if (!search_path_owner_.Add(provider_path.parent_path(), "CUDA EP", logger)) {
        return false;
      }
#endif

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
      if (!LoadGenAiCudaLibrary(provider_path.parent_path() / kGenAiCudaLibrary, genai_cuda_handle_, logger)) {
        return false;
      }
#endif

      if (!register_ep_(kRegistrationName, provider_path)) {
        logger.Log(LogLevel::Warning, fmt::format("CUDA EP: ORT registration failed for override {}={}",
                                                  kCudaProviderOverrideEnv, provider_path.string()));
        return false;
      }

      registered_ = true;

      if (progress_cb) {
        progress_cb(name_, 100.0f);
      }

      logger.Log(LogLevel::Information, fmt::format("CUDA EP: ready (override_env={} install_path={})",
                                                    kCudaProviderOverrideEnv, provider_path.string()));
      return true;
    }

    auto manifest = BuildCudaEpManifest(HostCudaEpPlatform());
    if (!manifest.has_value()) {
      logger.Log(LogLevel::Warning, "CUDA EP: no bundle available for this platform");
      return false;
    }

    const auto install_policy = force ? EpBundleInstallPolicy::ForceDownload : EpBundleInstallPolicy::ReuseVerified;
    auto txn = installer_.EnsureInstalled(*manifest, progress_cb, logger, install_policy);
    if (!txn) {
      return false;
    }

    if (!txn->CommitActive(logger)) {
      logger.Log(LogLevel::Warning, "CUDA EP: failed to publish active bundle marker");
      return false;
    }

    const auto provider_path = txn->bin_dir() / manifest->provider_relative_path;
#ifdef _WIN32
    if (!search_path_owner_.Add(txn->bin_dir(), "CUDA EP", logger)) {
      return false;
    }
#elif defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
    if (!LoadGenAiCudaLibrary(txn->bin_dir() / kGenAiCudaLibrary, genai_cuda_handle_, logger)) {
      return false;
    }
#endif

    if (!register_ep_(kRegistrationName, provider_path)) {
      logger.Log(LogLevel::Warning, "CUDA EP: ORT registration failed");
      return false;
    }

    registered_ = true;

    if (progress_cb) {
      progress_cb(name_, 100.0f);
    }

    logger.Log(LogLevel::Information, fmt::format("CUDA EP: ready (install_path={})", txn->bin_dir().string()));
    return true;
  } catch (const std::exception& e) {
    logger.Log(LogLevel::Warning, fmt::format("CUDA EP: error: {}", e.what()));
    return false;
  }
}

bool CudaEpBootstrapper::HasNvidiaGpu(ILogger& logger) { return NvmlGpuDetector::HasNvidiaGpu(logger); }

bool CudaEpBootstrapper::IsSupportedPlatform() {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || \
    (defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__))
  return true;
#else
  return false;
#endif
}

}  // namespace fl
