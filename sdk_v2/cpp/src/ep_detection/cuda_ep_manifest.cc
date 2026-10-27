// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/cuda_ep_manifest.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fl {

namespace {

constexpr const char* kCdnBase = "https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/";
constexpr uint64_t kMiB = 1024ULL * 1024;

EpBundleArtifact Archive(std::string id, std::string filename, std::string sha256, uint64_t max_bytes,
                         std::vector<EpBundleFile> files) {
  return EpBundleArtifact{
      .id = std::move(id),
      .url = std::string(kCdnBase) + filename,
      .is_archive = true,
      .archive_sha256 = std::move(sha256),
      .extracted_files = std::move(files),
      .ignored_archive_paths = {"version.json"},
      .archive_max_bytes = max_bytes,
      .raw_relative_path = "",
      .raw_sha256 = "",
      .raw_max_bytes = 0,
  };
}

EpBundleManifest WindowsX64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-win-x64-ort-1.31.0-genai-0.18.0-20261014-064231",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-bins-win-x64-20261014-064231.zip",
                  "ad7e1d0ad84d0ca6861848fa31f2cd92c548b67c57d264d819f26bda03383b98", 256 * kMiB,
                  {
                      {.relative_path = "onnxruntime-genai-cuda.dll",
                       .sha256 = "d9bc0e9a737d6169b2e801f13bf87a51e7182f65ad64ba8f0c4ee3037be12457"},
                      {.relative_path = "onnxruntime_providers_cuda.dll",
                       .sha256 = "a884a89c137db78211c731956e038edde7617122d329d1eeaa732a5926e11962"},
                  }),
          },
      .provider_relative_path = "onnxruntime_providers_cuda.dll",
  };
}

EpBundleManifest WindowsArm64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-win-arm64-ort-1.31.0-genai-0.18.0-20261014-064257",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-bins-win-arm64-20261014-064257.zip",
                  "728acba50d92f7c41d9a848bfc122f628959dddf3ca25293f31e3c963aff5209", 96 * kMiB,
                  {
                      {.relative_path = "onnxruntime-genai-cuda.dll",
                       .sha256 = "6727494a28d352dc2992c79f5320410b70a1d954a92dd452b34b0ee2de790617"},
                      {.relative_path = "onnxruntime_providers_cuda.dll",
                       .sha256 = "54f4d246a26eda7fc623eac215349a128b43091d2a1563d73142756642250385"},
                  }),
          },
      .provider_relative_path = "onnxruntime_providers_cuda.dll",
  };
}

EpBundleManifest LinuxX64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-linux-x64-ort-1.31.0-genai-0.18.0-20261014-064302",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-linux-x64-20261014-064302.zip",
                  "d83605c5e963aa571d33c8837cf6c97094bab98877e8143c56228f692cf6c53f", 448 * kMiB,
                  {
                      {.relative_path = "libonnxruntime-genai-cuda.so",
                       .sha256 = "858b1cc5fc258f1ed63f95c98bd2f1fde3002a490bf3e3be2736bfdbef96ecc8"},
                      {.relative_path = "libonnxruntime_providers_cuda.so",
                       .sha256 = "f3a15157d4252001ee0bd4eae3bbc31b3d0bc151dc950cb038d1f6550e9f1c94"},
                  }),
          },
      .provider_relative_path = "libonnxruntime_providers_cuda.so",
  };
}

EpBundleManifest LinuxArm64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-linux-arm64-ort-1.31.0-genai-0.18.0-20261014-064346",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-linux-arm64-20261014-064346.zip",
                  "9ddc52bb1e6ae2abf5a37debb1af4d9c148becc29448dca32b6d94a3a50d480e", 224 * kMiB,
                  {
                      {.relative_path = "libonnxruntime-genai-cuda.so",
                       .sha256 = "208550d347239316be71cc982ae35e0156dd70b2380b49362131e30138cab3d0"},
                      {.relative_path = "libonnxruntime_providers_cuda.so",
                       .sha256 = "3b66400c90219f3527992fa93e39df44f54ea2bcb730e55a729d7d37d1d9fc30"},
                  }),
          },
      .provider_relative_path = "libonnxruntime_providers_cuda.so",
  };
}

}  // namespace

std::optional<EpBundleManifest> BuildCudaEpManifest(CudaEpPlatform platform) {
  switch (platform) {
    case CudaEpPlatform::WindowsX64:
      return WindowsX64Manifest();
    case CudaEpPlatform::WindowsArm64:
      return WindowsArm64Manifest();
    case CudaEpPlatform::LinuxX64:
      return LinuxX64Manifest();
    case CudaEpPlatform::LinuxArm64:
      return LinuxArm64Manifest();
    case CudaEpPlatform::Unsupported:
      return std::nullopt;
  }

  return std::nullopt;
}

}  // namespace fl
