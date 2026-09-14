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
      .bundle_id = "cuda-ep-win-x64-cuda-12.8.4-ort-1.30.0-genai-0.16.0-20260913-071149",
      .artifacts =
          {
              Archive(
                  "cuda-toolkit", "cuda-bins-win-x64-20260806-182620.zip",
                  "a90223e4091cfa63b1e40af27a5d5f0267fdfdd15f0459c2922106afe352d306", 640 * kMiB,
                  {
                      {.relative_path = "cublas64_12.dll",
                       .sha256 = "9513540e4ec4c51ee9e7304138c2cc255c29a8c181f9e80c38efa25738becd99"},
                      {.relative_path = "cublasLt64_12.dll",
                       .sha256 = "b199d1ff892a81b7fd3d57ba1781549609b41500b36008fef326038393ad46c7"},
                      {.relative_path = "cudart64_12.dll",
                       .sha256 = "c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344"},
                  }),
              Archive(
                  "cudnn", "cudnn-bins-win-x64-20260806-182620.zip",
                  "b82cd271c8c9cbd52ea9e4dedaa4cc3864bf8d7b221d87d5bde81d6ee4a399da", 704 * kMiB,
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
                  "cuda-ep", "cuda-ep-bins-win-x64-20260913-071149.zip",
                  "5d1c38eb4058b6898d78ae5e7881ac07dda8ef37156be232f0f63846a1405f85", 256 * kMiB,
                  {
                      {.relative_path = "onnxruntime-genai-cuda.dll",
                       .sha256 = "132d28f988ee8bc1ce050e8dcdda7036ab436616d827508af8e0a4f6ec21f2da"},
                      {.relative_path = "onnxruntime_providers_cuda.dll",
                       .sha256 = "7d014892b64d03092c01e99ae5786c6080c5239eeee30e6fb902fd2a800fa95e"},
                  }),
          },
      .provider_relative_path = "onnxruntime_providers_cuda.dll",
  };
}

EpBundleManifest WindowsArm64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-win-arm64-cuda-13.4.1-ort-1.30.0-genai-0.16.0-20260913-071350",
      .artifacts =
          {
              Archive(
                  "cuda-toolkit", "cuda-bins-win-arm64-20260806-182803.zip",
                  "de71001db47deb1b59567c50cd5fb1c7705945a9461c95505e987fb8731d6175", 192 * kMiB,
                  {
                      {.relative_path = "cublas64_13.dll",
                       .sha256 = "80b322ce3fe77d1c6c0348e30a31c5f2682da4197680177a179af69275b57997"},
                      {.relative_path = "cublasLt64_13.dll",
                       .sha256 = "d13048a5f17deeb1a051189c0d5ac898cdf398c6dfca62d100c6eb39329a1d80"},
                      {.relative_path = "cudart64_13.dll",
                       .sha256 = "32504bd5f424a4e73d3bb5ecc69f018538ae371efa0210bd33e88c7c78b9dca7"},
                  }),
              Archive(
                  "cudnn", "cudnn-bins-win-arm64-20260806-182803.zip",
                  "84338552f83a602e989e2a964ed37c342560486031c955225d265402ccf02bd1", 192 * kMiB,
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
                  "cuda-ep", "cuda-ep-bins-win-arm64-20260913-071350.zip",
                  "fc8f0a01daafedc82aa57072f023b11cf801dd94ed0a449ddc8ac4698e43e519", 96 * kMiB,
                  {
                      {.relative_path = "onnxruntime-genai-cuda.dll",
                       .sha256 = "d27a02b8a83d0aaac47904d1baff4f7954aa25dcc130dba539fe9aafc53eab30"},
                      {.relative_path = "onnxruntime_providers_cuda.dll",
                       .sha256 = "82a3887c64791fc7131a0f705130f02f456952915ecea71372236656c59a47b2"},
                  }),
          },
      .provider_relative_path = "onnxruntime_providers_cuda.dll",
  };
}

EpBundleManifest LinuxX64Manifest() {
  return EpBundleManifest{
      .bundle_id = "cuda-ep-linux-x64-ort-1.30.0-genai-0.16.0-20260913-071417",
      .artifacts =
          {
              Archive(
                  "cuda-ep", "cuda-ep-linux-x64-20260913-071417.zip",
                  "482e615e4c66b3f14a980af00e330e28fa5897a34161ec2972f75289685546be", 448 * kMiB,
                  {
                      {.relative_path = "libonnxruntime-genai-cuda.so",
                       .sha256 = "3c95bb3b92f560718482dd3638388b6fd7374fb5ba6d9d5f4c4ca22c095e7681"},
                      {.relative_path = "libonnxruntime_providers_cuda.so",
                       .sha256 = "8589ae31ed4941e72693a6a2e9dd8b8e721c888cfd9e6ffcb26da4e6e58ce720"},
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
