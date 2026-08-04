// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/cuda_ep_bootstrapper.h"

#include "ep_detection/ep_utils.h"
#include "ep_detection/nvml_gpu_detector.h"
#include "logger.h"
#include "utils.h"

#include <fmt/format.h>

#include <filesystem>
#include <optional>
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

#if defined(_WIN32) && defined(_M_ARM64)
constexpr const char* kCudaBundleId = "cuda-ep-win-arm64-unconfigured";
#elif defined(_WIN32) && defined(_M_X64)
constexpr const char* kCudaBundleId = "cuda-ep-win-x64-unconfigured";
#elif defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
constexpr const char* kCudaBundleId = "cuda-ep-linux-x64-ort-1.28.0-genai-0.15.1-20260804-074520";
constexpr const char* kCudaDownloadUrl =
    "https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/cuda-ep-linux-x64-20260804-074520.zip";
constexpr const char* kCudaArchiveSha256 =
    "97FF54C93A8E4D6622905AD19BCC9D6B5AA03E54B6B38682FC51117086DCF1F6";
constexpr uint64_t kCudaArchiveMaxBytes = 512ULL * 1024 * 1024;
#endif

#if defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))
fl::EpBundleArtifact DisabledArchiveArtifact(std::string id) {
  return fl::EpBundleArtifact{.id = std::move(id),
                              .url = "",
                              .is_archive = true,
                              .archive_sha256 = "",
                              .extracted_files = {},
                              .archive_max_bytes = 0,
                              .raw_relative_path = "",
                              .raw_sha256 = "",
                              .raw_max_bytes = 0};
}
#elif defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
fl::EpBundleArtifact LinuxCudaArchiveArtifact() {
  return fl::EpBundleArtifact{
      .id = "cuda-ep",
      .url = kCudaDownloadUrl,
      .is_archive = true,
      .archive_sha256 = kCudaArchiveSha256,
      .extracted_files =
          {
              {.relative_path = "libonnxruntime-genai-cuda.so",
               .sha256 = "86AED826BC9221ABA24A1B9C856A403FD8AEC082B06E6924F616E08A49C6C2F0"},
              {.relative_path = "libonnxruntime_providers_cuda.so",
               .sha256 = "9418788F29E45F70904DBA8FA21BE7317C92A45D505B1E50322F3B71A94E52F7"},
              {.relative_path = "version.json",
               .sha256 = "65133BC2003C363B4D2C6CB85BC913AFD5291B1D6E2869C3656D940DBF72A505"},
          },
      .archive_max_bytes = kCudaArchiveMaxBytes,
      .raw_relative_path = "",
      .raw_sha256 = "",
      .raw_max_bytes = 0,
  };
}
#endif

std::optional<fl::EpBundleManifest> BuildCudaManifest() {
#if defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))
  fl::EpBundleManifest manifest;
  manifest.bundle_id = kCudaBundleId;
  manifest.provider_relative_path = "onnxruntime_providers_cuda.dll";
  manifest.artifacts = {DisabledArchiveArtifact("cuda-toolkit"), DisabledArchiveArtifact("cudnn"),
                        DisabledArchiveArtifact("cuda-ep")};
  return manifest;
#elif defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
  fl::EpBundleManifest manifest;
  manifest.bundle_id = kCudaBundleId;
  manifest.provider_relative_path = "libonnxruntime_providers_cuda.so";
  manifest.artifacts = {LinuxCudaArchiveArtifact()};
  return manifest;
#else
  return std::nullopt;
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
    : register_ep_(std::move(register_ep)),
      installer_(std::filesystem::path(root_dir), kLockFileName, "CUDA EP") {}

CudaEpBootstrapper::~CudaEpBootstrapper() {
#if defined(__linux__) && !defined(__ANDROID__)
  if (genai_cuda_handle_) {
    dlclose(genai_cuda_handle_);
  }
#endif
}

const std::string& CudaEpBootstrapper::Name() const {
  return name_;
}

bool CudaEpBootstrapper::IsRegistered() const {
  return registered_;
}

bool CudaEpBootstrapper::DownloadAndRegister(bool force,
                                             const ProgressCallback& progress_cb,
                                             ILogger& logger) {
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
        logger.Log(LogLevel::Warning,
                   fmt::format("CUDA EP: {} set but file does not exist ({})",
                               kCudaProviderOverrideEnv, provider_path.string()));
        return false;
      }

      if (progress_cb) {
        progress_cb(name_, 90.0f);
      }

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
      if (!LoadGenAiCudaLibrary(provider_path.parent_path() / kGenAiCudaLibrary, genai_cuda_handle_, logger)) {
        return false;
      }
#endif

      if (!register_ep_(kRegistrationName, provider_path)) {
        logger.Log(LogLevel::Warning,
                   fmt::format("CUDA EP: ORT registration failed for override {}={}",
                               kCudaProviderOverrideEnv, provider_path.string()));
        return false;
      }

      registered_ = true;

      if (progress_cb) {
        progress_cb(name_, 100.0f);
      }

      logger.Log(LogLevel::Information,
                 fmt::format("CUDA EP: ready (override_env={} install_path={})",
                             kCudaProviderOverrideEnv, provider_path.string()));
      return true;
    }

    auto manifest = BuildCudaManifest();
    if (!manifest.has_value()) {
      logger.Log(LogLevel::Warning, "CUDA EP: no bundle available for this platform");
      return false;
    }

    // CUDA force requests another registration attempt, but retains the existing package reuse behavior.
    auto txn = installer_.EnsureInstalled(*manifest, progress_cb, logger,
                                          EpBundleInstallPolicy::ReuseVerified);
    if (!txn) {
      return false;
    }

    auto provider_path = txn->bin_dir() / manifest->provider_relative_path;

#ifdef _WIN32
    if (!LoadEpBundleDependencies(txn->bin_dir(), *manifest, "CUDA EP", logger)) {
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

    if (!txn->CommitActive(logger)) {
      logger.Log(LogLevel::Warning, "CUDA EP: failed to publish active bundle marker");
    }

    if (progress_cb) {
      progress_cb(name_, 100.0f);
    }

    logger.Log(LogLevel::Information,
               fmt::format("CUDA EP: ready (install_path={})", txn->bin_dir().string()));
    return true;
  } catch (const std::exception& e) {
    logger.Log(LogLevel::Warning, fmt::format("CUDA EP: error: {}", e.what()));
    return false;
  }
}

bool CudaEpBootstrapper::HasNvidiaGpu() {
  return NvmlGpuDetector::HasNvidiaGpu();
}

bool CudaEpBootstrapper::IsSupportedPlatform() {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || \
    (defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__))
  return true;
#else
  return false;
#endif
}

}  // namespace fl
