// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/cuda_ep_bootstrapper.h"

#include "ep_detection/cuda_ep_manifest.h"
#include "ep_detection/nvml_gpu_detector.h"
#include "logger.h"
#include "platform/dynlib_loader.h"
#include "utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace {

constexpr const char* kLockFileName = "cuda-ep.lock";
constexpr int kMaxInstallAttempts = 5;
constexpr const char* kRegistrationName = "CUDAExecutionProvider";
constexpr const char* kCudaProviderOverrideEnv = "FOUNDRY_LOCAL_CUDA_EP_LIBRARY";
#if defined(__linux__)
constexpr const char* kGenAiCudaLibrary = "libonnxruntime-genai-cuda.so";
#elif defined(_WIN32)
constexpr const char* kGenAiCudaLibrary = "onnxruntime-genai-cuda.dll";
#endif

fl::CudaEpPlatform HostCudaEpPlatform() {
#if defined(_WIN32) && defined(_M_ARM64)
  return fl::CudaEpPlatform::WindowsArm64;
#elif defined(_WIN32) && defined(_M_X64)
  return fl::CudaEpPlatform::WindowsX64;
#elif defined(__linux__)
  return fl::CudaEpPlatform::LinuxX64;
#else
  return fl::CudaEpPlatform::Unsupported;
#endif
}

#if defined(__linux__)
bool LoadGenAiCudaLibrary(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::filesystem::path, std::shared_ptr<void>>>& loaded_libraries,
    const fl::GenAiCudaLibraryLoader& loader,
    std::pair<std::filesystem::path, std::shared_ptr<void>>& provisional_library, fl::ILogger& logger) {
  const auto absolute_path = std::filesystem::absolute(path).lexically_normal();
  const auto already_loaded =
      std::any_of(loaded_libraries.begin(), loaded_libraries.end(),
                  [&](const auto& loaded_library) { return loaded_library.first == absolute_path; });
  if (already_loaded) {
    return true;
  }

  auto loaded_library = loader(absolute_path, logger);
  if (!loaded_library) {
    return false;
  }

  provisional_library = {absolute_path, std::move(loaded_library)};
  return true;
}
#elif defined(_WIN32)
bool LoadGenAiCudaLibrary(const std::filesystem::path& path, const fl::GenAiCudaLibraryLoader& loader,
                          std::shared_ptr<void>& loaded_library, fl::ILogger& logger) {
  // Keep the library resident so NvTensorRTRTX can resolve it by name during model load.
  if (!loaded_library) {
    loaded_library = loader(path, logger);
  }
  return loaded_library != nullptr;
}
#endif

}  // anonymous namespace

namespace fl {

CudaEpBootstrapper::CudaEpBootstrapper(std::string root_dir, EpRegistrationCallback register_ep,
                                       EpBundleManifestFactory manifest_factory, EpArtifactDownloadFn download_fn
#if defined(__linux__) || defined(_WIN32)
                                       ,
                                       GenAiCudaLibraryLoader genai_cuda_library_loader
#endif
                                       )
    : register_ep_(std::move(register_ep)),
      manifest_factory_(manifest_factory ? std::move(manifest_factory)
                                         : [] { return BuildCudaEpManifest(HostCudaEpPlatform()); }),
      installer_(std::filesystem::path(root_dir), kLockFileName, "CUDA EP", std::move(download_fn))
#if defined(__linux__) || defined(_WIN32)
      ,
      genai_cuda_library_loader_(genai_cuda_library_loader ? std::move(genai_cuda_library_loader)
                                                           : platform::LoadSharedLibrary)
#endif
{
}

CudaEpBootstrapper::~CudaEpBootstrapper() = default;

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

      bundle_dir_ = provider_path.parent_path();
#if defined(__linux__)
      std::pair<std::filesystem::path, std::shared_ptr<void>> provisional_genai_cuda_library;
      if (!LoadGenAiCudaLibrary(provider_path.parent_path() / kGenAiCudaLibrary, genai_cuda_libraries_,
                                genai_cuda_library_loader_, provisional_genai_cuda_library, logger)) {
        return false;
      }
#elif defined(_WIN32)
      if (!LoadGenAiCudaLibrary(bundle_dir_ / kGenAiCudaLibrary, genai_cuda_library_loader_,
                                genai_cuda_library_, logger)) {
        return false;
      }
#endif

      if (!register_ep_(kRegistrationName, provider_path)) {
        logger.Log(LogLevel::Warning, fmt::format("CUDA EP: ORT registration failed for override {}={}",
                                                  kCudaProviderOverrideEnv, provider_path.string()));
        return false;
      }

#if defined(__linux__)
      if (provisional_genai_cuda_library.second) {
        genai_cuda_libraries_.push_back(std::move(provisional_genai_cuda_library));
      }
#endif

      registered_ = true;

      if (progress_cb) {
        progress_cb(name_, 100.0f);
      }

      logger.Log(LogLevel::Information, fmt::format("CUDA EP: ready (override_env={} install_path={})",
                                                    kCudaProviderOverrideEnv, provider_path.string()));
      return true;
    }

    auto manifest = manifest_factory_();
    if (!manifest.has_value()) {
      logger.Log(LogLevel::Warning, "CUDA EP: no bundle available for this platform");
      return false;
    }

    const auto install_policy = force ? EpBundleInstallPolicy::ForceDownload : EpBundleInstallPolicy::ReuseVerified;
    auto txn = installer_.EnsureInstalled(*manifest, progress_cb, logger, install_policy);
    if (!txn) {
      return false;
    }

    if (!txn->Activate()) {
      logger.Log(LogLevel::Warning, "CUDA EP: failed to activate bundle");
      return false;
    }

    const auto provider_path = txn->provider_path();
    bundle_dir_ = txn->bin_dir();
#if defined(__linux__)
    std::pair<std::filesystem::path, std::shared_ptr<void>> provisional_genai_cuda_library;
    if (!LoadGenAiCudaLibrary(txn->bin_dir() / kGenAiCudaLibrary, genai_cuda_libraries_,
                              genai_cuda_library_loader_, provisional_genai_cuda_library, logger)) {
      txn->Rollback();
      return false;
    }
#elif defined(_WIN32)
    if (!LoadGenAiCudaLibrary(bundle_dir_ / kGenAiCudaLibrary, genai_cuda_library_loader_,
                              genai_cuda_library_, logger)) {
      txn->Rollback();
      return false;
    }
#endif

    if (!register_ep_(kRegistrationName, provider_path)) {
      logger.Log(LogLevel::Warning, "CUDA EP: ORT registration failed");
      txn->Rollback();
      return false;
    }

#if defined(__linux__)
    if (provisional_genai_cuda_library.second) {
      genai_cuda_libraries_.push_back(std::move(provisional_genai_cuda_library));
    }
#endif

    registered_ = true;
    txn->Finalize();

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

bool CudaEpBootstrapper::PrepareForModelLoad([[maybe_unused]] ILogger& logger) {
#ifdef _WIN32
  return platform::SetDynamicLibrarySearchDirectory(bundle_dir_, logger);
#else
  return true;
#endif
}

bool CudaEpBootstrapper::HasNvidiaGpu(ILogger& logger) { return NvmlGpuDetector::HasNvidiaGpu(logger); }

bool CudaEpBootstrapper::IsSupportedPlatform() {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || defined(__linux__)
  return true;
#else
  return false;
#endif
}

}  // namespace fl
