// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/webgpu_ep_bootstrapper.h"

#include "ep_detection/ep_utils.h"
#include "logger.h"
#include "utils.h"

#include <fmt/format.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace {

constexpr const char* kLockFileName = "webgpu-ep.lock";
constexpr int kMaxInstallAttempts = 5;
#if defined(_WIN32) && defined(_M_ARM64)
constexpr const char* kBundleId = "webgpu-ep-0.2.1-win-arm64";
constexpr const char* kDownloadUrl =
    "https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/webgpu_ep_0.2.1_win-arm64.zip";
constexpr const char* kArchiveSha256 = "3674C8BD50F19AB84D3F738AC426DB37EB119BC1B790525B5A4F4139C253AF08";
constexpr const char* kProviderSha256 = "63CFEF0E7FB8FDC2238F69CD8E804F50FDA393B2B60C448DAEC73E031DE75058";
constexpr const char* kDxCompilerSha256 = "3895C1F437E8E91A771F562AD2E5EA9EF918365EA1D7D4216AF4C58BA87E9D7B";
constexpr const char* kDxilSha256 = "9377B286B378AF2ACD7DA7686F25FB60C7D22DEC4BA384BAB0523494DE3E75D0";
#elif defined(_WIN32) && defined(_M_X64)
constexpr const char* kBundleId = "webgpu-ep-0.2.1-win-x64";
constexpr const char* kDownloadUrl =
    "https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/webgpu_ep_0.2.1_win-x64.zip";
constexpr const char* kArchiveSha256 = "91A05B2C9EAF326011FE74604BBDF06E08C5B95A1F40425F4426EF0E90A9984D";
constexpr const char* kProviderSha256 = "BE2EBCC0A96D1558D9123C04E75C2851260FE45C9DBC8959CB2CD8D11B83ABBE";
constexpr const char* kDxCompilerSha256 = "174DBC3DF8F7AF5C32C0E39F43C0D5BC576395EDC3CCDD64119A1B63C081ED55";
constexpr const char* kDxilSha256 = "080C02F62E90D0AB7ACC463BBC10280C37397DC6D036224D7C10F2ED9C20E13D";
#elif defined(__APPLE__) && defined(__aarch64__)
constexpr const char* kBundleId = "webgpu-ep-0.2.1-macos-arm64";
constexpr const char* kDownloadUrl =
    "https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/webgpu_ep_0.2.1_macos-arm64.zip";
constexpr const char* kArchiveSha256 = "5F0F8378172F53EFA281328F33D1EBC793E44197FFB4CC605D02C593B891C0EA";
constexpr const char* kProviderSha256 = "8FAC874A60F32F0127C74CB7DEF915807FCC8A6C30B77629E45F8CEE60272EAE";
#endif

constexpr const char* kRegistrationName = "Foundry.WebGPU";
constexpr const char* kWebGpuProviderOverrideEnv = "FOUNDRY_LOCAL_WEBGPU_EP_LIBRARY";
constexpr const char* kVersionSha256 = "4CB81DA21A42BC8A1DE985A2C6C7DFEE3F634576B0C8C7FA0990FB027F1BB082";
constexpr uint64_t kArchiveMaxBytes = 64ULL * 1024 * 1024;

std::optional<fl::EpBundleManifest> BuildWebGpuManifest() {
#if defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))
  fl::EpBundleManifest manifest;
  manifest.bundle_id = kBundleId;
  manifest.provider_relative_path = "onnxruntime_providers_webgpu.dll";
  manifest.artifacts = {{
      .id = "webgpu-ep",
      .url = kDownloadUrl,
      .is_archive = true,
      .archive_sha256 = kArchiveSha256,
      .extracted_files =
          {
              {.relative_path = "dxcompiler.dll", .sha256 = kDxCompilerSha256},
              {.relative_path = "dxil.dll", .sha256 = kDxilSha256},
              {.relative_path = "onnxruntime_providers_webgpu.dll", .sha256 = kProviderSha256},
              {.relative_path = "version.json", .sha256 = kVersionSha256},
          },
      .archive_max_bytes = kArchiveMaxBytes,
      .raw_relative_path = "",
      .raw_sha256 = "",
      .raw_max_bytes = 0,
  }};
  return manifest;
#elif defined(__APPLE__) && defined(__aarch64__)
  fl::EpBundleManifest manifest;
  manifest.bundle_id = kBundleId;
  manifest.provider_relative_path = "libonnxruntime_providers_webgpu.dylib";
  manifest.artifacts = {{
      .id = "webgpu-ep",
      .url = kDownloadUrl,
      .is_archive = true,
      .archive_sha256 = kArchiveSha256,
      .extracted_files =
          {
              {.relative_path = "libonnxruntime_providers_webgpu.dylib", .sha256 = kProviderSha256},
              {.relative_path = "version.json", .sha256 = kVersionSha256},
          },
      .archive_max_bytes = kArchiveMaxBytes,
      .raw_relative_path = "",
      .raw_sha256 = "",
      .raw_max_bytes = 0,
  }};
  return manifest;
#else
  return std::nullopt;
#endif
}

}  // anonymous namespace

namespace fl {

WebGpuEpBootstrapper::WebGpuEpBootstrapper(std::string root_dir, EpRegistrationCallback register_ep)
    : register_ep_(std::move(register_ep)),
      installer_(std::filesystem::path(root_dir), kLockFileName, "WebGPU EP") {}

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

  try {
    auto override_path = Utils::GetEnv(kWebGpuProviderOverrideEnv);
    if (override_path.has_value() && !override_path->empty()) {
      std::filesystem::path provider_path = std::filesystem::absolute(*override_path);

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

    auto manifest = BuildWebGpuManifest();
    if (!manifest.has_value()) {
      logger.Log(LogLevel::Warning, "WebGPU EP: no bundle available for this platform");
      return false;
    }

    const auto install_policy =
        force ? EpBundleInstallPolicy::ForceDownload : EpBundleInstallPolicy::ReuseVerified;
    auto txn = installer_.EnsureInstalled(*manifest, progress_cb, logger, install_policy);
    if (!txn) {
      return false;
    }

    auto provider_path = txn->bin_dir() / manifest->provider_relative_path;

#ifdef _WIN32
    // The provider delay-loads sibling DirectX compiler binaries after registration; keep PATH
    // primed as a fallback in addition to the explicit preload below.
    PrependDirToProcessPath(txn->bin_dir());
#endif

    if (!LoadEpBundleDependencies(txn->bin_dir(), *manifest, "WebGPU EP", logger)) {
      return false;
    }

    if (!register_ep_(kRegistrationName, provider_path)) {
      logger.Log(LogLevel::Warning, "WebGPU EP: ORT registration failed");
      return false;
    }

    registered_ = true;

    if (!txn->CommitActive(logger)) {
      logger.Log(LogLevel::Warning, "WebGPU EP: failed to publish active bundle marker");
    }

    if (progress_cb) {
      progress_cb(name_, 100.0f);
    }

    logger.Log(LogLevel::Information,
               fmt::format("WebGPU EP: ready (install_path={})", txn->bin_dir().string()));
    return true;
  } catch (const std::exception& e) {
    logger.Log(LogLevel::Warning, fmt::format("WebGPU EP: error: {}", e.what()));
    return false;
  }
}

bool WebGpuEpBootstrapper::IsSupportedPlatform() {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || \
    (defined(__APPLE__) && defined(__aarch64__))
  return true;
#else
  return false;
#endif
}

}  // namespace fl
