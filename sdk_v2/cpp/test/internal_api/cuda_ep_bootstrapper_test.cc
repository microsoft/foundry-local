// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/cuda_ep_bootstrapper.h"

#include "ep_detection/cuda_ep_manifest.h"
#include "ep_detection/ep_utils.h"
#include "logger.h"
#include "utils/scoped_environment_variable.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fl {

namespace {

constexpr uint64_t kMiB = 1024ULL * 1024;
constexpr std::string_view kCdnBase = "https://foundrypackages-ffhrdhbxb7gpdreh.b02.azurefd.net/";
constexpr const char* kOverrideEnv = "FOUNDRY_LOCAL_CUDA_EP_LIBRARY";

struct ExpectedFile {
  std::string_view path;
  std::string_view sha256;
};

void ExpectArtifact(const EpBundleArtifact& artifact, std::string_view id, std::string_view filename,
                    std::string_view archive_sha256, uint64_t max_bytes,
                    const std::vector<ExpectedFile>& expected_files) {
  EXPECT_EQ(artifact.id, id);
  EXPECT_EQ(artifact.url, std::string(kCdnBase) + std::string(filename));
  EXPECT_TRUE(artifact.is_archive);
  EXPECT_EQ(artifact.archive_sha256, archive_sha256);
  EXPECT_EQ(artifact.archive_max_bytes, max_bytes);
  EXPECT_EQ(artifact.ignored_archive_paths, std::vector<std::string>{"version.json"});
  ASSERT_EQ(artifact.extracted_files.size(), expected_files.size());

  for (size_t i = 0; i < expected_files.size(); ++i) {
    EXPECT_EQ(artifact.extracted_files[i].relative_path, expected_files[i].path);
    EXPECT_EQ(artifact.extracted_files[i].sha256, expected_files[i].sha256);
  }
}

void ExpectUniqueInstalledPaths(const EpBundleManifest& manifest) {
  std::unordered_set<std::string> paths;
  for (const auto& artifact : manifest.artifacts) {
    for (const auto& file : artifact.extracted_files) {
      EXPECT_TRUE(paths.insert(file.relative_path).second) << file.relative_path;
    }
  }
}

}  // namespace

TEST(CudaEpBootstrapperTest, PlatformSupportMatchesPublishedBundles) {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || \
    (defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__))
  EXPECT_TRUE(CudaEpBootstrapper::IsSupportedPlatform());
#else
  EXPECT_FALSE(CudaEpBootstrapper::IsSupportedPlatform());
#endif
}

TEST(CudaEpManifestTest, WindowsX64MetadataMatches20260805Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::WindowsX64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-win-x64-cuda-12.8.4-ort-1.28.0-genai-0.15.2-20260805-050438");
  EXPECT_EQ(manifest->provider_relative_path, "onnxruntime_providers_cuda.dll");
  ASSERT_EQ(manifest->artifacts.size(), 3u);

  ExpectArtifact(manifest->artifacts[0], "cuda-toolkit", "cuda-bins-win-x64-20260805-050438.zip",
                 "b47716cbd9a1c92722a6bc914ca57b0e8efea15f7b1a46eecfb2637cadc1bee5", 640 * kMiB,
                 {
                     {"cublas64_12.dll", "9513540e4ec4c51ee9e7304138c2cc255c29a8c181f9e80c38efa25738becd99"},
                     {"cublasLt64_12.dll", "b199d1ff892a81b7fd3d57ba1781549609b41500b36008fef326038393ad46c7"},
                     {"cudart64_12.dll", "c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344"},
                 });
  ExpectArtifact(
      manifest->artifacts[1], "cudnn", "cudnn-bins-win-x64-20260805-050438.zip",
      "1b065e115c2ac35040053ebe594a8c089906f8cbe5b8d8ed832ba5eb27cdeb5e", 704 * kMiB,
      {
          {"cudnn64_9.dll", "0d1d71325eb5e91570ab8ba8e399e07bf717ffd76511b2407229a8f45e0b1305"},
          {"cudnn_adv64_9.dll", "6d66bce22502c2582a9c0e5398ee8cc38addce2c837eb6db8786abc650e48dd8"},
          {"cudnn_engines_precompiled64_9.dll", "b410c3b42921afc6e668ff994fce1bf12c5a8a9b1a9445ebee61958bf49b1e0a"},
          {"cudnn_engines_runtime_compiled64_9.dll",
           "8e62214495c96b93c6333c084fec49b43f272b7e1977a12fe62275e9070647eb"},
          {"cudnn_graph64_9.dll", "82f710b01d15d20c311009721c771b76360a4954ebf7b5f4a407b0f96587f568"},
          {"cudnn_heuristic64_9.dll", "50719eefb6692074096bf83c87e9cd186f7ce5b953201da33669c1277a61949b"},
          {"cudnn_ops64_9.dll", "49487537744256a3d4365c4792b03bf31130ad1faea0a13eafa219620941d837"},
      });
  ExpectArtifact(
      manifest->artifacts[2], "cuda-ep", "cuda-ep-bins-win-x64-20260805-050438.zip",
      "65044a715a2d4b74e77f019988f77c936f5b62973c27cf2d59704cf39057e567", 256 * kMiB,
      {
          {"onnxruntime-genai-cuda.dll", "612ad6cf3d099431af886537080223a62522e58caba0c7d278b9b4b1eb03c4ce"},
          {"onnxruntime_providers_cuda.dll", "971c1002ce7c16338273f693316bec4862ac74c7efa2ffbd644630cfe10d6e37"},
      });
  ExpectUniqueInstalledPaths(*manifest);
}

TEST(CudaEpManifestTest, WindowsArm64MetadataMatches20260805Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::WindowsArm64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-win-arm64-cuda-13.4.1-ort-1.28.0-genai-0.15.2-20260805-050639");
  EXPECT_EQ(manifest->provider_relative_path, "onnxruntime_providers_cuda.dll");
  ASSERT_EQ(manifest->artifacts.size(), 3u);

  ExpectArtifact(manifest->artifacts[0], "cuda-toolkit", "cuda-bins-win-arm64-20260805-050639.zip",
                 "b4e0ce6beea87843d02c7d41e04fee3d1a9fb22e0f4fb5e587914cf4f4b94113", 192 * kMiB,
                 {
                     {"cublas64_13.dll", "80b322ce3fe77d1c6c0348e30a31c5f2682da4197680177a179af69275b57997"},
                     {"cublasLt64_13.dll", "d13048a5f17deeb1a051189c0d5ac898cdf398c6dfca62d100c6eb39329a1d80"},
                     {"cudart64_13.dll", "32504bd5f424a4e73d3bb5ecc69f018538ae371efa0210bd33e88c7c78b9dca7"},
                 });
  ExpectArtifact(
      manifest->artifacts[1], "cudnn", "cudnn-bins-win-arm64-20260805-050639.zip",
      "24347fc6b596ae28c32659c82da688bc386da36228e65329df031c028d8527ad", 192 * kMiB,
      {
          {"cudnn64_9.dll", "247cecbb33132c829c6ed328b7dd34d077a27d0f0fb0ee0b56469ec6bdfd1c17"},
          {"cudnn_adv64_9.dll", "b624590960a3ce3ac7c3a5fc683912dbd9ba9de20fa1af52db4485c435c78375"},
          {"cudnn_engines_precompiled64_9.dll", "c3be7f8a9091865b7fc94ddd69e62024338d42a188633729e4520244b072da2d"},
          {"cudnn_engines_runtime_compiled64_9.dll",
           "bd558d60e1dbeeeee8f59dfec8bd5ce992876f923e2f19f7ef03a4a7e110a89a"},
          {"cudnn_graph64_9.dll", "8f568df300b0733abe2cb35ea6bfcc40d2330db005ab9ebba96d008f6bc0b568"},
          {"cudnn_heuristic64_9.dll", "59d5aad876ab55d30194f36d3f2c5ff90eeaeed502b256c83ed3d6082030f58d"},
          {"cudnn_ops64_9.dll", "c9e0ec0e0a4e659393e15897ed1f6e5bac677e0c0fe7e12290f0386f19477b6b"},
      });
  ExpectArtifact(
      manifest->artifacts[2], "cuda-ep", "cuda-ep-bins-win-arm64-20260805-050639.zip",
      "8152d03a0fbef39bd11f5b07dbc6776abd125dbee1dc1d2877a04dc62bbde641", 96 * kMiB,
      {
          {"onnxruntime-genai-cuda.dll", "5284fdec9d4e9e25d6b4cf129205f0c88d3c2f5e678907b2bc1581b575266016"},
          {"onnxruntime_providers_cuda.dll", "b60cd5a26bc180229c9da0dc635d6b3404c306246708291dfae7c9f72ad5e862"},
      });
  ExpectUniqueInstalledPaths(*manifest);
}

TEST(CudaEpManifestTest, LinuxX64MetadataMatches20260805Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::LinuxX64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-linux-x64-ort-1.28.0-genai-0.15.2-20260805-050706");
  EXPECT_EQ(manifest->provider_relative_path, "libonnxruntime_providers_cuda.so");
  ASSERT_EQ(manifest->artifacts.size(), 1u);

  ExpectArtifact(
      manifest->artifacts[0], "cuda-ep", "cuda-ep-linux-x64-20260805-050706.zip",
      "2bc3e5949b75d7521d903c958716c06602ddaa5c2a1f98bd12811294db738c37", 448 * kMiB,
      {
          {"libonnxruntime-genai-cuda.so", "8b26db7a085de61653ebaaa8fc221b720879fe74583eb01204f11bf22638c345"},
          {"libonnxruntime_providers_cuda.so", "da94d951b89dc84c44b10f7faf52b17e675b3f1a13d8f32808264d425d0464bd"},
      });
  ExpectUniqueInstalledPaths(*manifest);
}

TEST(CudaEpManifestTest, UnsupportedPlatformsHaveNoManifest) {
  EXPECT_FALSE(BuildCudaEpManifest(CudaEpPlatform::LinuxArm64).has_value());
  EXPECT_FALSE(BuildCudaEpManifest(CudaEpPlatform::Unsupported).has_value());
}

TEST(CudaEpBootstrapperTest, OverrideCancellationBeforeRegistrationReturnsFalse) {
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  const auto provider_path = root.path() / "custom_cuda_provider";
  std::ofstream(provider_path, std::ios::binary) << "test provider";
  test::ScopedEnvironmentVariable override(kOverrideEnv, provider_path.string());

  int registration_count = 0;
  CudaEpBootstrapper bootstrapper(root.string(), [&](const std::string&, const std::filesystem::path&) {
    ++registration_count;
    return true;
  });
  StderrLogger logger;

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(
      false, [](const std::string&, float percent) { return percent != kEpReadyToRegisterProgress; }, logger));
  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_EQ(registration_count, 0);
}

}  // namespace fl
