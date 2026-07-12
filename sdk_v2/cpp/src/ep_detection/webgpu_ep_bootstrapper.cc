// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/webgpu_ep_bootstrapper.h"

#include "ep_detection/ep_utils.h"
#include "http/http_download.h"
#include "logger.h"
#include "util/file_lock.h"
#include "util/sha256.h"
#include "utils.h"
#include "util/zip_extract.h"

#include <fmt/format.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

constexpr const char* kPackageFileName = "webgpu-ep.zip";
constexpr const char* kLockFileName = "webgpu-ep.lock";
constexpr const char* kStagingDirName = "webgpu-ep-staging";
constexpr const char* kUserAgent = "FoundryLocal";
constexpr int kMaxInstallAttempts = 5;

struct WebGpuPackageMetadata {
  const char* download_url;
  const char* zip_sha256;
  const char* provider_sha256;
#if defined(_WIN32)
  const char* dxcompiler_sha256;
  const char* dxil_sha256;
#endif
};

#if defined(_WIN32)
const std::unordered_map<std::string_view, WebGpuPackageMetadata> kPackageMetadata = {
    {"win-arm64",
     {"https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/webgpu_ep_0.1.0_win-arm64.zip",
      "90CD6744103F29530C9B1467583543F9460C27F67DF185A8352EA1C7A29DC8F8",
      "C4A77911BDBFC6E2870D1895DA3F5BE476CE3398D772C02AFDDD2B2C49C66659",
      "6DA88B1B24EAF5A7E0CAD46A1A41B9D26FF5244464544986AF85385F6C04F807",
      "60AC8AECDAC6D509CCEF67E127E295EC90BE6CCABDB84606D79B7A1B208082CA"}
    },
    {"win-x64",
     {"https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/webgpu_ep_0.1.0_win-x64.zip",
      "CCD9ADB663D670069BB1369FA7319BB2C91180BA3262CA4000F14DA066AF0059",
      "591E286A211B133E3C4E5C833FEBF2D594B7B548433A2490407B11B906A9271B",
      "2CEC74EF87A1171E4502C7D735169756BF4C528676EA354BFDE24B01394965F0",
      "0767448ABEADE590821E88E56C471E0F2DFE89EC9A7642D08ADC3DC94F14AB92"}
    },
};
#elif defined(__APPLE__)
const std::unordered_map<std::string_view, WebGpuPackageMetadata> kPackageMetadata = {
    {"macos-arm64",
     {"https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/webgpu_ep_0.1.0_macos-arm64.zip",
      "E4BC0329EE60408A9E7183A9A5AA1D1639CB4C61C4C501A499B5911CFFC4DBF4",
      "A08BCEBE097B555E23938FCC71A5FAAD461F586CAB0B63DC9D21E970F6CA4C87"}
    },
};
#else
const std::unordered_map<std::string_view, WebGpuPackageMetadata> kPackageMetadata = {
    {"linux-x64",
     {"https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/webgpu_ep_0.1.0_linux-x64.zip",
      "B3241E6C33FDA7A0D507A3D8256DB432244F9DF1D87A2ACFD6EE470625EB5C76",
      "CBDFF74E6569E3CF66B46F0D194D87CD3CF49E83B7AA46552C39B0218D58B215"}
    },
};
#endif

// Platform-specific package metadata is baked into the binary to keep
// verification inputs fixed at build time.
#if defined(_WIN32) && defined(_M_ARM64)
constexpr const char* kPlatformKey = "win-arm64";
#elif defined(_WIN32)
constexpr const char* kPlatformKey = "win-x64";
#elif defined(__APPLE__)
constexpr const char* kPlatformKey = "macos-arm64";
#else
constexpr const char* kPlatformKey = "linux-x64";
#endif

const WebGpuPackageMetadata& GetPackageMetadata() {
  auto it = kPackageMetadata.find(kPlatformKey);

  if (it == kPackageMetadata.end()) {
    throw std::runtime_error(
        fmt::format("WebGPU EP: no package metadata configured for platform '{}'", kPlatformKey));
  }

  return it->second;
}

// Platform-specific EP library filename.
#if defined(_WIN32)
constexpr const char* kWebGpuProviderLib = "onnxruntime_providers_webgpu.dll";
#elif defined(__APPLE__)
constexpr const char* kWebGpuProviderLib = "libonnxruntime_providers_webgpu.dylib";
#else
constexpr const char* kWebGpuProviderLib = "libonnxruntime_providers_webgpu.so";
#endif

constexpr const char* kRegistrationName = "Foundry.WebGPU";
constexpr const char* kWebGpuProviderOverrideEnv = "FOUNDRY_LOCAL_WEBGPU_EP_LIBRARY";

}  // anonymous namespace

namespace fl {

WebGpuEpBootstrapper::WebGpuEpBootstrapper(std::string ep_dir, EpRegistrationCallback register_ep)
    : ep_dir_(std::move(ep_dir)), register_ep_(std::move(register_ep)) {}

const std::string& WebGpuEpBootstrapper::Name() const {
  return name_;
}

bool WebGpuEpBootstrapper::IsRegistered() const {
  return registered_;
}

bool WebGpuEpBootstrapper::DownloadAndRegister(bool force,
                                               const ProgressCallback& progress_cb,
                                               ILogger& logger) {
  if (registered_ && !force) {
    if (progress_cb) {
      progress_cb(name_, 100.0f);
    }
    return true;
  }

  if (!force && attempts_ >= kMaxInstallAttempts) {
    logger.Log(LogLevel::Warning, "WebGPU EP: max install attempts reached");
    return false;
  }

  attempts_++;

  auto ep_dir = std::filesystem::path(ep_dir_);
  auto parent_dir = ep_dir.parent_path();

  try {
    const auto& package_metadata = GetPackageMetadata();
  #if defined(_WIN32)
    const auto expected_files = std::initializer_list<std::pair<std::string_view, std::string_view>>{
      {kWebGpuProviderLib, package_metadata.provider_sha256},
      {"dxcompiler.dll", package_metadata.dxcompiler_sha256},
      {"dxil.dll", package_metadata.dxil_sha256},
    };
  #else
    const auto expected_files = std::initializer_list<std::pair<std::string_view, std::string_view>>{
      {kWebGpuProviderLib, package_metadata.provider_sha256},
    };
  #endif

    auto override_path = Utils::GetEnv(kWebGpuProviderOverrideEnv);
    if (override_path.has_value() && !override_path->empty()) {
      std::filesystem::path provider_path(*override_path);

      if (!std::filesystem::exists(provider_path)) {
        logger.Log(LogLevel::Warning,
                   fmt::format("WebGPU EP: {} set but file does not exist ({})",
                               kWebGpuProviderOverrideEnv, provider_path.string()));
        return false;
      }

      if (progress_cb) {
        progress_cb(name_, 90.0f);
      }

      // Prepend the override directory to PATH so sibling dependency DLLs are discoverable,
      // matching the normal install path. The WebGPU EP may delay-load dependencies.
      PrependDirToProcessPath(provider_path.parent_path());

      if (!register_ep_(kRegistrationName, provider_path)) {
        logger.Log(LogLevel::Warning,
                   fmt::format("WebGPU EP: ORT registration failed for override {}={}",
                               kWebGpuProviderOverrideEnv, provider_path.string()));
        return false;
      }

      registered_ = true;

      if (progress_cb) {
        progress_cb(name_, 100.0f);
      }

      logger.Log(LogLevel::Information,
                 fmt::format("WebGPU EP: ready (override_env={} install_path={})",
                             kWebGpuProviderOverrideEnv, provider_path.string()));
      return true;
    }

    // Check if package already exists and is valid
    logger.Log(LogLevel::Debug,
               fmt::format("WebGPU EP: verifying existing install at {}", ep_dir.string()));

    if (!force && VerifyEpBinaries(
                      ep_dir,
                      expected_files,
                      "WebGPU EP",
                      logger)) {
      logger.Log(LogLevel::Debug, "WebGPU EP: local binaries match expected hashes, skipping download");
    } else {
      // Ensure parent directory exists for the lock file
      std::filesystem::create_directories(parent_dir);
      auto lock_path = parent_dir / kLockFileName;

      // Cross-process lock to prevent concurrent installs
      FileLock lock(lock_path);

      // Re-check after acquiring lock (another process may have completed the update)
      if (!force && VerifyEpBinaries(
                        ep_dir,
                        expected_files,
                        "WebGPU EP",
                        logger)) {
        logger.Log(LogLevel::Debug, "WebGPU EP: another process already completed the update");
      } else {
        // Download and extract to staging directory for atomic swap
        auto staging_dir = parent_dir / kStagingDirName;
        if (std::filesystem::exists(staging_dir)) {
          std::filesystem::remove_all(staging_dir);
        }
        std::filesystem::create_directories(staging_dir);

        auto zip_path = staging_dir / kPackageFileName;

        // Download
        logger.Log(LogLevel::Information,
                   fmt::format("WebGPU EP: downloading for {} from CDN", kPlatformKey));

        std::atomic<bool> cancel_flag{false};
        auto download_progress = [&](float pct) {
          if (progress_cb) {
            // 0–80% for download phase
            if (!progress_cb(name_, pct * 0.8f)) {
              cancel_flag.store(true);
            }
          }
        };

        if (!HttpDownloadFile(package_metadata.download_url, zip_path, kUserAgent,
                              &cancel_flag, download_progress, logger)) {
          logger.Log(LogLevel::Warning, "WebGPU EP: download failed (see prior log for details)");
          return false;
        }

        logger.Log(LogLevel::Debug,
                   fmt::format("WebGPU EP: verifying downloaded archive {}", zip_path.string()));

        if (!VerifyEpArchive(zip_path,
                             package_metadata.zip_sha256,
                             "WebGPU EP",
                             logger)) {
          logger.Log(LogLevel::Warning, "WebGPU EP: downloaded archive verification failed");
          std::filesystem::remove_all(staging_dir);
          return false;
        }

        logger.Log(LogLevel::Debug, "WebGPU EP: downloaded archive verification succeeded");

        // Extract
        logger.Log(LogLevel::Information,
                   fmt::format("WebGPU EP: extracting package to {}", staging_dir.string()));

        if (!ExtractZip(zip_path, staging_dir, logger)) {
          logger.Log(LogLevel::Warning, "WebGPU EP: extraction failed");
          std::filesystem::remove_all(staging_dir);
          return false;
        }

        // Clean up zip
        std::filesystem::remove(zip_path);

        // Verify staging
        logger.Log(LogLevel::Debug,
                   fmt::format("WebGPU EP: verifying extracted staging contents at {}",
                               staging_dir.string()));

        if (!VerifyEpBinaries(
          staging_dir,
          expected_files,
          "WebGPU EP",
          logger)) {
          logger.Log(LogLevel::Warning,
                     fmt::format("WebGPU EP: verification failed after extraction (attempt {})",
                                 attempts_));
          std::filesystem::remove_all(staging_dir);
          return false;
        }

        logger.Log(LogLevel::Debug, "WebGPU EP: staging verification succeeded");

        // Atomic swap: delete old, rename staging to target
        if (std::filesystem::exists(ep_dir)) {
          std::filesystem::remove_all(ep_dir);
        }

        std::filesystem::rename(staging_dir, ep_dir);

        logger.Log(LogLevel::Information, "WebGPU EP: successfully installed");
      }
    }

    if (progress_cb) {
      progress_cb(name_, 90.0f);
    }

    // Register with ORT
#ifdef _WIN32
    // Prepend the EP directory to PATH for the process lifetime.
    // WebGPU EP may delay-load additional dependencies from the same directory.
    PrependDirToProcessPath(ep_dir);
#endif

    auto provider_path = ep_dir / kWebGpuProviderLib;

    logger.Log(LogLevel::Debug,
           fmt::format("WebGPU EP: registering verified provider library {}",
                 provider_path.string()));

    if (!register_ep_(kRegistrationName, provider_path)) {
      logger.Log(LogLevel::Warning, "WebGPU EP: ORT registration failed");
      return false;
    }

    registered_ = true;

    if (progress_cb) {
      progress_cb(name_, 100.0f);
    }

    logger.Log(LogLevel::Information,
               fmt::format("WebGPU EP: ready (install_path={})", ep_dir.string()));

    logger.Log(LogLevel::Debug,
               fmt::format("WebGPU EP: success path complete for {}", ep_dir.string()));
    return true;
  } catch (const std::exception& e) {
    logger.Log(LogLevel::Warning, fmt::format("WebGPU EP: error: {}", e.what()));
    return false;
  }
}

}  // namespace fl
