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
      .bundle_id = "cuda-ep-win-x64-ort-1.31.0-genai-0.19.0-20261104-010352",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-bins-win-x64-20261104-010352.zip",
                  "bdcf1c5dcd9a439d993fb38d2c06fa492807926127a0735cdc8e993afa852468", 256 * kMiB,
                  {
                      {.relative_path = "onnxruntime-genai-cuda.dll",
                       .sha256 = "9e713362d779b521d9c85b3d89448ad4eb73a171058e7f5bb55dbd295f2c2c3d"},
                      {.relative_path = "onnxruntime_providers_cuda.dll",
                       .sha256 = "a884a89c137db78211c731956e038edde7617122d329d1eeaa732a5926e11962"},
                  }),
          },
      .provider_relative_path = "onnxruntime_providers_cuda.dll",
  };
}

EpBundleManifest WindowsArm64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-win-arm64-ort-1.31.0-genai-0.19.0-20261104-010422",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-bins-win-arm64-20261104-010422.zip",
                  "a7005389f16ce6d30d78a428847f3d7bc8f8956fc19503d29955acd6866e7fa2", 96 * kMiB,
                  {
                      {.relative_path = "onnxruntime-genai-cuda.dll",
                       .sha256 = "2cf9e5cf3e57165ff61bea52d78dbccd8ad3a072dd3901b91d5fd819408d8617"},
                      {.relative_path = "onnxruntime_providers_cuda.dll",
                       .sha256 = "54f4d246a26eda7fc623eac215349a128b43091d2a1563d73142756642250385"},
                  }),
          },
      .provider_relative_path = "onnxruntime_providers_cuda.dll",
  };
}

EpBundleManifest LinuxX64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-linux-x64-ort-1.31.0-genai-0.19.0-20261104-010429",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-linux-x64-20261104-010429.zip",
                  "ddf38784bbb667393b56c11279cab6b68df1e27389d11cd3e9c2289ada931e91", 448 * kMiB,
                  {
                      {.relative_path = "libonnxruntime-genai-cuda.so",
                       .sha256 = "70d8f717209d8ad85e8c6b19f1df836bdf7a80ba1cec5b960d414ee1ba79dfe6"},
                      {.relative_path = "libonnxruntime_providers_cuda.so",
                       .sha256 = "f3a15157d4252001ee0bd4eae3bbc31b3d0bc151dc950cb038d1f6550e9f1c94"},
                  }),
          },
      .provider_relative_path = "libonnxruntime_providers_cuda.so",
  };
}

EpBundleManifest LinuxArm64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-linux-arm64-ort-1.31.0-genai-0.19.0-20261104-010519",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-linux-arm64-20261104-010519.zip",
                  "9716edb0f247976514076eed19898a50cc4b67461ddb5fd527debe7900c067e5", 224 * kMiB,
                  {
                      {.relative_path = "libonnxruntime-genai-cuda.so",
                       .sha256 = "4a3c8598738615b4c4dfa33b7e859bdacb4b03cd1254b5e65f74aeceb2563ca0"},
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
