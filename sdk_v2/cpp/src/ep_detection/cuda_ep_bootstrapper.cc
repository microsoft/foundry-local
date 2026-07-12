// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/cuda_ep_bootstrapper.h"

#include "ep_detection/ep_utils.h"
#include "logger.h"
#include "util/file_lock.h"
#include "utils.h"
#include "http/http_download.h"
#include "util/zip_extract.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

constexpr const char* kPackageFileName = "cuda-ep.zip";
constexpr const char* kLockFileName = "cuda-ep.lock";
constexpr const char* kUserAgent = "FoundryLocal";
constexpr int kMaxInstallAttempts = 5;

// CUDA EP package is built against the ONNX Runtime version we link against.
constexpr const char* kDownloadUrl =
    "https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/cuda-ep-20260501-062935.zip";

struct ExpectedBinary {
  const char* filename;
  const char* sha256;
};

constexpr ExpectedBinary kExpectedBinaries[] = {
    {"onnxruntime_providers_cuda.dll", "DD540FCFECFBC68B4675C9ADF09C2858CF6B054563859D79598AA2524406A76F"},
    {"onnxruntime-genai-cuda.dll", "BC953F8E2AAFC6219B2D723B65AB8F1A9426A6B7724D6A01ED756FAE8C3DE6AE"},
};

constexpr const char* kRegistrationName = "Foundry.CUDA";
constexpr const char* kCudaProviderDll = "onnxruntime_providers_cuda.dll";
constexpr const char* kCudaProviderOverrideEnv = "FOUNDRY_LOCAL_CUDA_EP_LIBRARY";

}  // anonymous namespace

namespace fl {

CudaEpBootstrapper::CudaEpBootstrapper(std::string ep_dir, EpRegistrationCallback register_ep)
    : ep_dir_(std::move(ep_dir)), register_ep_(std::move(register_ep)) {}

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

  auto ep_dir = std::filesystem::path(ep_dir_);
  auto lock_path = ep_dir.parent_path() / kLockFileName;
  auto zip_path = ep_dir.parent_path() / kPackageFileName;

  try {
    auto override_path = Utils::GetEnv(kCudaProviderOverrideEnv);
    if (override_path.has_value() && !override_path->empty()) {
      std::filesystem::path provider_path(*override_path);

      if (!std::filesystem::exists(provider_path)) {
        logger.Log(LogLevel::Warning,
                   fmt::format("CUDA EP: {} set but file does not exist ({})",
                               kCudaProviderOverrideEnv, provider_path.string()));
        return false;
      }

      if (progress_cb) {
        progress_cb(name_, 90.0f);
      }

      // Prepend the override directory to PATH so sibling dependency DLLs are discoverable,
      // matching the normal install path. The provider DLL delay-loads CUDA/cuDNN dependencies.
      PrependDirToProcessPath(provider_path.parent_path());

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

    // Cross-process lock to prevent concurrent installs
    FileLock lock(lock_path);

    // Check if package already exists and is valid
    if (fl::VerifyEpBinaries(ep_dir,
            {{kExpectedBinaries[0].filename, kExpectedBinaries[0].sha256},
             {kExpectedBinaries[1].filename, kExpectedBinaries[1].sha256}},
            "CUDA EP", logger)) {
      logger.Log(LogLevel::Information, "CUDA EP: package already valid, skipping download");
    } else {
      // Clean up any partial install
      if (std::filesystem::exists(ep_dir)) {
        std::filesystem::remove_all(ep_dir);
      }

      std::filesystem::create_directories(ep_dir);

      // Download
      logger.Log(LogLevel::Information, "CUDA EP: downloading from CDN...");

      // Bridge callback-based cancellation to the atomic flag HttpDownloadFile expects
      std::atomic<bool> cancel_flag{false};

      auto download_progress = [&](float pct) {
        if (progress_cb) {
          // 0-80% for download phase
          if (!progress_cb(name_, pct * 0.8f)) {
            cancel_flag.store(true);
          }
        }
      };

      if (!HttpDownloadFile(kDownloadUrl, zip_path, kUserAgent,
                            &cancel_flag, download_progress, logger)) {
        logger.Log(LogLevel::Warning, "CUDA EP: download failed (see prior log for details)");
        return false;
      }

      // Extract
      logger.Log(LogLevel::Information, "CUDA EP: extracting...");

      if (!ExtractZip(zip_path, ep_dir, logger)) {
        logger.Log(LogLevel::Warning, "CUDA EP: extraction failed");
        return false;
      }

      // Clean up zip
      std::filesystem::remove(zip_path);

      // Verify
      if (!fl::VerifyEpBinaries(ep_dir,
               {{kExpectedBinaries[0].filename, kExpectedBinaries[0].sha256},
                {kExpectedBinaries[1].filename, kExpectedBinaries[1].sha256}},
               "CUDA EP", logger)) {
        logger.Log(LogLevel::Warning, "CUDA EP: verification failed after download");
        return false;
      }
    }

    if (progress_cb) {
      progress_cb(name_, 90.0f);
    }

    // Register with ORT
#ifdef _WIN32
    // Permanently prepend the EP directory to PATH. The zip bundles all
    // required CUDA/cuDNN DLLs, so no system CUDA install is needed.
    // PATH must stay modified for the process lifetime because:
    //   - onnxruntime_providers_cuda.dll delay-loads some dependencies
    //   - onnxruntime-genai-cuda.dll is loaded later at model-load time
    //   - ORT creates CUDA sessions after registration
    PrependDirToProcessPath(ep_dir);
#endif

    auto cuda_dll_path = ep_dir / kCudaProviderDll;

    if (!register_ep_(kRegistrationName, cuda_dll_path)) {
      logger.Log(LogLevel::Warning, "CUDA EP: ORT registration failed");
      return false;
    }

    registered_ = true;

    if (progress_cb) {
      progress_cb(name_, 100.0f);
    }

    // Bootstrapper-side log — captures the install dir, which the central
    // register_ep callback (logs library + version) doesn't have.
    logger.Log(LogLevel::Information,
               fmt::format("CUDA EP: ready (install_path={})", ep_dir.string()));
    return true;
  } catch (const std::exception& e) {
    logger.Log(LogLevel::Warning, fmt::format("CUDA EP: error: {}", e.what()));
    return false;
  }
}

bool CudaEpBootstrapper::HasNvidiaGpu() {
#ifdef _WIN32
  FILE* pipe = _popen("nvidia-smi --query-gpu=compute_cap --format=csv,noheader,nounits 2>nul", "r");
#else
  FILE* pipe = popen("nvidia-smi --query-gpu=compute_cap --format=csv,noheader,nounits 2>/dev/null", "r");
#endif

  if (!pipe) {
    return false;
  }

  char buffer[128];
  std::string result;
  while (fgets(buffer, sizeof(buffer), pipe)) {
    result += buffer;
  }

#ifdef _WIN32
  int exit_code = _pclose(pipe);
#else
  int exit_code = pclose(pipe);
#endif

  if (exit_code != 0 || result.empty()) {
    return false;
  }

  // Need compute capability >= 5.0 for CUDA 12
  try {
    float compute_cap = std::stof(result);
    return compute_cap >= 5.0f;
  } catch (...) {
    return false;
  }
}

}  // namespace fl
