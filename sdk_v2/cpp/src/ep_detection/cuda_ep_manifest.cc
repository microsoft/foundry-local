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
      .bundle_id = "cuda-ep-win-x64-cuda-12.8.4-ort-1.28.0-genai-0.15.2-20260805-050438",
      .artifacts =
          {
              Archive(
                  "cuda-toolkit", "cuda-bins-win-x64-20260805-050438.zip",
                  "b47716cbd9a1c92722a6bc914ca57b0e8efea15f7b1a46eecfb2637cadc1bee5", 640 * kMiB,
                  {
                      {.relative_path = "cublas64_12.dll",
                       .sha256 = "9513540e4ec4c51ee9e7304138c2cc255c29a8c181f9e80c38efa25738becd99"},
                      {.relative_path = "cublasLt64_12.dll",
                       .sha256 = "b199d1ff892a81b7fd3d57ba1781549609b41500b36008fef326038393ad46c7"},
                      {.relative_path = "cudart64_12.dll",
                       .sha256 = "c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344"},
                  }),
              Archive(
                  "cudnn", "cudnn-bins-win-x64-20260805-050438.zip",
                  "1b065e115c2ac35040053ebe594a8c089906f8cbe5b8d8ed832ba5eb27cdeb5e", 704 * kMiB,
                  {
                      {.relative_path = "cudnn64_9.dll",
                       .sha256 = "0d1d71325eb5e91570ab8ba8e399e07bf717ffd76511b2407229a8f45e0b1305"},
                      {.relative_path = "cudnn_adv64_9.dll",
                       .sha256 = "6d66bce22502c2582a9c0e5398ee8cc38addce2c837eb6db8786abc650e48dd8"},
                      {.relative_path = "cudnn_engines_precompiled64_9.dll",
                       .sha256 = "b410c3b42921afc6e668ff994fce1bf12c5a8a9b1a9445ebee61958bf49b1e0a"},
                      {.relative_path = "cudnn_engines_runtime_compiled64_9.dll",
                       .sha256 = "8e62214495c96b93c6333c084fec49b43f272b7e1977a12fe62275e9070647eb"},
                      {.relative_path = "cudnn_graph64_9.dll",
                       .sha256 = "82f710b01d15d20c311009721c771b76360a4954ebf7b5f4a407b0f96587f568"},
                      {.relative_path = "cudnn_heuristic64_9.dll",
                       .sha256 = "50719eefb6692074096bf83c87e9cd186f7ce5b953201da33669c1277a61949b"},
                      {.relative_path = "cudnn_ops64_9.dll",
                       .sha256 = "49487537744256a3d4365c4792b03bf31130ad1faea0a13eafa219620941d837"},
                  }),
              Archive(
                  "cuda-ep", "cuda-ep-bins-win-x64-20260805-050438.zip",
                  "65044a715a2d4b74e77f019988f77c936f5b62973c27cf2d59704cf39057e567", 256 * kMiB,
                  {
                      {.relative_path = "onnxruntime-genai-cuda.dll",
                       .sha256 = "612ad6cf3d099431af886537080223a62522e58caba0c7d278b9b4b1eb03c4ce"},
                      {.relative_path = "onnxruntime_providers_cuda.dll",
                       .sha256 = "971c1002ce7c16338273f693316bec4862ac74c7efa2ffbd644630cfe10d6e37"},
                  }),
          },
      .provider_relative_path = "onnxruntime_providers_cuda.dll",
  };
}

EpBundleManifest WindowsArm64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-win-arm64-cuda-13.4.1-ort-1.28.0-genai-0.15.2-20260805-050639",
      .artifacts =
          {
              Archive(
                  "cuda-toolkit", "cuda-bins-win-arm64-20260805-050639.zip",
                  "b4e0ce6beea87843d02c7d41e04fee3d1a9fb22e0f4fb5e587914cf4f4b94113", 192 * kMiB,
                  {
                      {.relative_path = "cublas64_13.dll",
                       .sha256 = "80b322ce3fe77d1c6c0348e30a31c5f2682da4197680177a179af69275b57997"},
                      {.relative_path = "cublasLt64_13.dll",
                       .sha256 = "d13048a5f17deeb1a051189c0d5ac898cdf398c6dfca62d100c6eb39329a1d80"},
                      {.relative_path = "cudart64_13.dll",
                       .sha256 = "32504bd5f424a4e73d3bb5ecc69f018538ae371efa0210bd33e88c7c78b9dca7"},
                  }),
              Archive(
                  "cudnn", "cudnn-bins-win-arm64-20260805-050639.zip",
                  "24347fc6b596ae28c32659c82da688bc386da36228e65329df031c028d8527ad", 192 * kMiB,
                  {
                      {.relative_path = "cudnn64_9.dll",
                       .sha256 = "247cecbb33132c829c6ed328b7dd34d077a27d0f0fb0ee0b56469ec6bdfd1c17"},
                      {.relative_path = "cudnn_adv64_9.dll",
                       .sha256 = "b624590960a3ce3ac7c3a5fc683912dbd9ba9de20fa1af52db4485c435c78375"},
                      {.relative_path = "cudnn_engines_precompiled64_9.dll",
                       .sha256 = "c3be7f8a9091865b7fc94ddd69e62024338d42a188633729e4520244b072da2d"},
                      {.relative_path = "cudnn_engines_runtime_compiled64_9.dll",
                       .sha256 = "bd558d60e1dbeeeee8f59dfec8bd5ce992876f923e2f19f7ef03a4a7e110a89a"},
                      {.relative_path = "cudnn_graph64_9.dll",
                       .sha256 = "8f568df300b0733abe2cb35ea6bfcc40d2330db005ab9ebba96d008f6bc0b568"},
                      {.relative_path = "cudnn_heuristic64_9.dll",
                       .sha256 = "59d5aad876ab55d30194f36d3f2c5ff90eeaeed502b256c83ed3d6082030f58d"},
                      {.relative_path = "cudnn_ops64_9.dll",
                       .sha256 = "c9e0ec0e0a4e659393e15897ed1f6e5bac677e0c0fe7e12290f0386f19477b6b"},
                  }),
              Archive(
                  "cuda-ep", "cuda-ep-bins-win-arm64-20260805-050639.zip",
                  "8152d03a0fbef39bd11f5b07dbc6776abd125dbee1dc1d2877a04dc62bbde641", 96 * kMiB,
                  {
                      {.relative_path = "onnxruntime-genai-cuda.dll",
                       .sha256 = "5284fdec9d4e9e25d6b4cf129205f0c88d3c2f5e678907b2bc1581b575266016"},
                      {.relative_path = "onnxruntime_providers_cuda.dll",
                       .sha256 = "b60cd5a26bc180229c9da0dc635d6b3404c306246708291dfae7c9f72ad5e862"},
                  }),
          },
      .provider_relative_path = "onnxruntime_providers_cuda.dll",
  };
}

EpBundleManifest LinuxX64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-linux-x64-ort-1.28.0-genai-0.15.2-20260805-050706",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-linux-x64-20260805-050706.zip",
                  "2bc3e5949b75d7521d903c958716c06602ddaa5c2a1f98bd12811294db738c37", 448 * kMiB,
                  {
                      {.relative_path = "libonnxruntime-genai-cuda.so",
                       .sha256 = "8b26db7a085de61653ebaaa8fc221b720879fe74583eb01204f11bf22638c345"},
                      {.relative_path = "libonnxruntime_providers_cuda.so",
                       .sha256 = "da94d951b89dc84c44b10f7faf52b17e675b3f1a13d8f32808264d425d0464bd"},
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
    case CudaEpPlatform::Unsupported:
      return std::nullopt;
  }

  return std::nullopt;
}

}  // namespace fl
