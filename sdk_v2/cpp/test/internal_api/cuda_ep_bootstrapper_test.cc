// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/cuda_ep_bootstrapper.h"

#include "ep_detection/cuda_ep_manifest.h"
#include "ep_detection/ep_bundle_installer.h"
#include "internal_api/ep_bundle_test_helpers.h"
#include "internal_api/test_helpers.h"
#include "logger.h"
#include "utils/scoped_environment_variable.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
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

using test::AsBytes;
using test::FakeDownloads;
using test::FindGenerationWithPrefix;
using test::HashOf;
using test::NullLogger;
using test::ReadFile;

#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || defined(__linux__)
constexpr const char* kLockFileName = "cuda-ep.lock";
#if defined(_WIN32)
constexpr const char* kProviderRelativePath = "onnxruntime_providers_cuda.dll";
#else
constexpr const char* kProviderRelativePath = "libonnxruntime_providers_cuda.so";
#endif
#if defined(__linux__)
constexpr const char* kGenAiCudaLibrary = "libonnxruntime-genai-cuda.so";
#elif defined(_WIN32)
constexpr const char* kGenAiCudaLibrary = "onnxruntime-genai-cuda.dll";
#endif

EpBundleManifest MakeRawManifest(FakeDownloads& downloads, const std::string& bundle_id) {
  const auto provider_url = std::string("https://example.test/") + bundle_id + "/provider";
  const auto provider_payload = AsBytes(bundle_id + "-provider");

  downloads.SetSequence(provider_url, {provider_payload});

  EpBundleManifest manifest;
  manifest.bundle_id = bundle_id;
  manifest.provider_relative_path = kProviderRelativePath;
  manifest.artifacts = {EpBundleArtifact{.id = "provider",
                                         .url = provider_url,
                                         .is_archive = false,
                                         .archive_sha256 = "",
                                         .extracted_files = {},
                                         .ignored_archive_paths = {},
                                         .archive_max_bytes = 0,
                                         .raw_relative_path = kProviderRelativePath,
                                         .raw_sha256 = HashOf(provider_payload),
                                         .raw_max_bytes = 1024}};

#if defined(__linux__)
  const auto genai_url = std::string("https://example.test/") + bundle_id + "/genai";
  const auto genai_payload = AsBytes(bundle_id + "-genai");

  downloads.SetSequence(genai_url, {genai_payload});

  manifest.artifacts.push_back(EpBundleArtifact{.id = "genai",
                                                .url = genai_url,
                                                .is_archive = false,
                                                .archive_sha256 = "",
                                                .extracted_files = {},
                                                .ignored_archive_paths = {},
                                                .archive_max_bytes = 0,
                                                .raw_relative_path = kGenAiCudaLibrary,
                                                .raw_sha256 = HashOf(genai_payload),
                                                .raw_max_bytes = 1024});
#endif

  return manifest;
}

template <typename RegisterEp>
CudaEpBootstrapper MakeInstalledBundleBootstrapper(std::string root_dir, RegisterEp register_ep,
                                                   const EpBundleManifest& manifest, EpArtifactDownloadFn download_fn) {
#if defined(__linux__) || defined(_WIN32)
  auto loader = [](const std::filesystem::path&, ILogger&) -> std::shared_ptr<void> {
    return std::shared_ptr<void>(new int(0), [](void* token) {
      delete static_cast<int*>(token);
    });
  };

  return CudaEpBootstrapper(std::move(root_dir), std::move(register_ep), [manifest] { return std::optional<EpBundleManifest>(manifest); }, std::move(download_fn), loader);
#else
  return CudaEpBootstrapper(std::move(root_dir), std::move(register_ep), [manifest] { return std::optional<EpBundleManifest>(manifest); }, std::move(download_fn));
#endif
}
#endif

}  // namespace

TEST(CudaEpBootstrapperTest, PlatformSupportMatchesPublishedBundles) {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || defined(__linux__)
  EXPECT_TRUE(CudaEpBootstrapper::IsSupportedPlatform());
#else
  EXPECT_FALSE(CudaEpBootstrapper::IsSupportedPlatform());
#endif
}

TEST(CudaEpManifestTest, WindowsX64MetadataMatches20260806Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::WindowsX64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-win-x64-cuda-12.8.4-ort-1.28.0-genai-0.15.2-20260806-182620");
  EXPECT_EQ(manifest->provider_relative_path, "onnxruntime_providers_cuda.dll");
  ASSERT_EQ(manifest->artifacts.size(), 3u);

  ExpectArtifact(manifest->artifacts[0], "cuda-toolkit", "cuda-bins-win-x64-20260806-182620.zip",
                 "a90223e4091cfa63b1e40af27a5d5f0267fdfdd15f0459c2922106afe352d306", 640 * kMiB,
                 {
                     {"cublas64_12.dll", "9513540e4ec4c51ee9e7304138c2cc255c29a8c181f9e80c38efa25738becd99"},
                     {"cublasLt64_12.dll", "b199d1ff892a81b7fd3d57ba1781549609b41500b36008fef326038393ad46c7"},
                     {"cudart64_12.dll", "c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344"},
                 });
  ExpectArtifact(
      manifest->artifacts[1], "cudnn", "cudnn-bins-win-x64-20260806-182620.zip",
      "b82cd271c8c9cbd52ea9e4dedaa4cc3864bf8d7b221d87d5bde81d6ee4a399da", 704 * kMiB,
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
      manifest->artifacts[2], "cuda-ep", "cuda-ep-bins-win-x64-20260806-182620.zip",
      "e62938987e848a0fbb3d215dfefaed40307d2446393909927ba0345eaaf3d263", 256 * kMiB,
      {
          {"onnxruntime-genai-cuda.dll", "7894fb5efaad4a663e834f20b912b44cc383629b24ffe8bbc6382786a7326dbc"},
          {"onnxruntime_providers_cuda.dll", "60f1aeef7ebe27f7e659cb88f597005ca5a5e75832b85dcef3eef02b9322df9a"},
      });
  ExpectUniqueInstalledPaths(*manifest);
}

TEST(CudaEpManifestTest, WindowsArm64MetadataMatches20260806Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::WindowsArm64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-win-arm64-cuda-13.4.1-ort-1.28.0-genai-0.15.2-20260806-182803");
  EXPECT_EQ(manifest->provider_relative_path, "onnxruntime_providers_cuda.dll");
  ASSERT_EQ(manifest->artifacts.size(), 3u);

  ExpectArtifact(manifest->artifacts[0], "cuda-toolkit", "cuda-bins-win-arm64-20260806-182803.zip",
                 "de71001db47deb1b59567c50cd5fb1c7705945a9461c95505e987fb8731d6175", 192 * kMiB,
                 {
                     {"cublas64_13.dll", "80b322ce3fe77d1c6c0348e30a31c5f2682da4197680177a179af69275b57997"},
                     {"cublasLt64_13.dll", "d13048a5f17deeb1a051189c0d5ac898cdf398c6dfca62d100c6eb39329a1d80"},
                     {"cudart64_13.dll", "32504bd5f424a4e73d3bb5ecc69f018538ae371efa0210bd33e88c7c78b9dca7"},
                 });
  ExpectArtifact(
      manifest->artifacts[1], "cudnn", "cudnn-bins-win-arm64-20260806-182803.zip",
      "84338552f83a602e989e2a964ed37c342560486031c955225d265402ccf02bd1", 192 * kMiB,
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
      manifest->artifacts[2], "cuda-ep", "cuda-ep-bins-win-arm64-20260806-182803.zip",
      "212e670c61b3292d4a7d98f16fc2cf61f7b080604e0c145e81c39ec81e7b3259", 96 * kMiB,
      {
          {"onnxruntime-genai-cuda.dll", "ab61145f4bc6284286e663586f634b973072d58ced20c497c7e5259f2ef3fc08"},
          {"onnxruntime_providers_cuda.dll", "d92ffbd23a84f91b976baed9031de267efe1dc892d85c09d0979d25b89f5d1a0"},
      });
  ExpectUniqueInstalledPaths(*manifest);
}

TEST(CudaEpManifestTest, LinuxX64MetadataMatches20260806Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::LinuxX64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-linux-x64-ort-1.28.0-genai-0.15.2-20260806-182830");
  EXPECT_EQ(manifest->provider_relative_path, "libonnxruntime_providers_cuda.so");
  ASSERT_EQ(manifest->artifacts.size(), 1u);

  ExpectArtifact(
      manifest->artifacts[0], "cuda-ep", "cuda-ep-linux-x64-20260806-182830.zip",
      "abf347e7234d7434105efde12a2e0609fdd1d8828167b9873f4463926f1206e6", 448 * kMiB,
      {
          {"libonnxruntime-genai-cuda.so", "d5300fc4413d9e74bd8dfceb5233fca6fcfa1d5ddc247081365fdb5f143091e6"},
          {"libonnxruntime_providers_cuda.so", "b88d7b7f4b2e81d3eff41663fc70f4ae9e03dee9e2301cb53dc250e5a96d7f7a"},
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

#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || defined(__linux__)
TEST(CudaEpBootstrapperTest, BundleActivationFailurePreventsRegistration) {
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  FakeDownloads downloads;
  const auto manifest = MakeRawManifest(downloads, "bundle-v1");

  std::filesystem::create_directories(root.path());
  std::filesystem::create_directory(root.path() / "active");
  {
    std::ofstream(root.path() / "active" / "blocker") << "x";
  }

  int registration_count = 0;
  auto bootstrapper = MakeInstalledBundleBootstrapper(
      root.string(),
      [&](const std::string&, const std::filesystem::path&) {
        ++registration_count;
        return true;
      },
      manifest, downloads.AsFn());
  NullLogger logger;

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  EXPECT_EQ(registration_count, 0);
  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_TRUE(std::filesystem::is_directory(root.path() / "active"));
}

TEST(CudaEpBootstrapperTest, BundleRegistrationFailureRollsBackToPreviousGeneration) {
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  FakeDownloads downloads;
  const auto manifest_v1 = MakeRawManifest(downloads, "bundle-v1");
  const auto manifest_v2 = MakeRawManifest(downloads, "bundle-v2");
  NullLogger logger;

  ASSERT_TRUE(test::InstallAndFinalize(root.path(), kLockFileName, "CUDA EP", manifest_v1, downloads.AsFn(), logger)
                  .has_value());
  const auto active_v1 = ReadFile(root.path() / "active");

  int registration_count = 0;
  auto bootstrapper = MakeInstalledBundleBootstrapper(
      root.string(),
      [&](const std::string&, const std::filesystem::path&) {
        ++registration_count;
        return false;
      },
      manifest_v2, downloads.AsFn());

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  EXPECT_EQ(registration_count, 1);
  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_EQ(ReadFile(root.path() / "active"), active_v1);
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1));

  const auto candidate_v2 = FindGenerationWithPrefix(root.path() / "bundles", "bundle-v2-");
  ASSERT_TRUE(candidate_v2.has_value());
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / *candidate_v2));
}

TEST(CudaEpBootstrapperTest, SuccessfulBundleRegistrationFinalizesPreviousGenerationCleanup) {
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  FakeDownloads downloads;
  const auto manifest_v1 = MakeRawManifest(downloads, "bundle-v1");
  const auto manifest_v2 = MakeRawManifest(downloads, "bundle-v2");
  NullLogger logger;

  ASSERT_TRUE(test::InstallAndFinalize(root.path(), kLockFileName, "CUDA EP", manifest_v1, downloads.AsFn(), logger)
                  .has_value());
  const auto active_v1 = ReadFile(root.path() / "active");

  int registration_count = 0;
  std::filesystem::path registered_path;
  auto bootstrapper = MakeInstalledBundleBootstrapper(
      root.string(),
      [&](const std::string&, const std::filesystem::path& path) {
        ++registration_count;
        registered_path = path;
        return true;
      },
      manifest_v2, downloads.AsFn());

  EXPECT_TRUE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  EXPECT_EQ(registration_count, 1);
  EXPECT_TRUE(bootstrapper.IsRegistered());
  EXPECT_EQ(registered_path.filename().string(), kProviderRelativePath);

  const auto active_v2 = ReadFile(root.path() / "active");
  EXPECT_TRUE(active_v2.starts_with("bundle-v2-"));
  EXPECT_FALSE(std::filesystem::exists(root.path() / "bundles" / active_v1));
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v2));
}

#if defined(_WIN32)
// On Windows the GenAI CUDA bridge is a hard dependency: if it cannot be loaded, CUDA registration
// must fail (before calling register_ep) rather than report a ready EP that a dependent
// NvTensorRTRTX model load would still find broken.
TEST(CudaEpBootstrapperTest, InstalledBundleGenAiBridgeLoadFailureSkipsRegistration) {
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  FakeDownloads downloads;
  const auto manifest = MakeRawManifest(downloads, "bundle-v1");
  NullLogger logger;

  int registration_count = 0;
  std::vector<std::filesystem::path> requested_paths;
  auto loader = [&](const std::filesystem::path& path, ILogger&) -> std::shared_ptr<void> {
    requested_paths.push_back(path);
    return {};  // simulate a bridge that cannot be loaded (e.g. a missing sibling dependency)
  };

  CudaEpBootstrapper bootstrapper(
      root.string(),
      [&](const std::string&, const std::filesystem::path&) {
        ++registration_count;
        return true;
      },
      [manifest] { return std::optional<EpBundleManifest>(manifest); }, downloads.AsFn(), loader);

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  EXPECT_EQ(registration_count, 0);  // bridge load precedes and gates ORT registration
  EXPECT_FALSE(bootstrapper.IsRegistered());
  ASSERT_EQ(requested_paths.size(), 1u);
  EXPECT_EQ(requested_paths[0].filename().string(), kGenAiCudaLibrary);
}
#endif
#endif

#if defined(__linux__)
TEST(CudaEpBootstrapperTest, InstalledBundleGenAiDependencyLoaderFailureRollsBackMarkerAndSkipsRegistration) {
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  FakeDownloads downloads;
  const auto manifest_v1 = MakeRawManifest(downloads, "bundle-v1");
  const auto manifest_v2 = MakeRawManifest(downloads, "bundle-v2");
  NullLogger logger;

  ASSERT_TRUE(test::InstallAndFinalize(root.path(), kLockFileName, "CUDA EP", manifest_v1, downloads.AsFn(), logger)
                  .has_value());
  const auto active_v1 = ReadFile(root.path() / "active");

  int registration_count = 0;
  std::vector<std::filesystem::path> requested_paths;
  auto loader = [&](const std::filesystem::path& path, ILogger&) -> std::shared_ptr<void> {
    requested_paths.push_back(path);
    return {};
  };

  CudaEpBootstrapper bootstrapper(
      root.string(),
      [&](const std::string&, const std::filesystem::path&) {
        ++registration_count;
        return true;
      },
      [manifest_v2] { return std::optional<EpBundleManifest>(manifest_v2); }, downloads.AsFn(), loader);

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  EXPECT_EQ(registration_count, 0);
  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_EQ(ReadFile(root.path() / "active"), active_v1);
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1));

  const auto candidate_v2 = FindGenerationWithPrefix(root.path() / "bundles", "bundle-v2-");
  ASSERT_TRUE(candidate_v2.has_value());
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / *candidate_v2));

  ASSERT_EQ(requested_paths.size(), 1u);
  EXPECT_EQ(requested_paths[0], std::filesystem::absolute(root.path() / "bundles" / *candidate_v2 / "bin" /
                                                          kGenAiCudaLibrary)
                                    .lexically_normal());
}

TEST(CudaEpBootstrapperTest, OverrideRegistrationFailureDoesNotPoisonGenAiCudaRetryState) {
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  const auto first_dir = root.path() / "first";
  const auto second_dir = root.path() / "second";
  std::filesystem::create_directories(first_dir);
  std::filesystem::create_directories(second_dir);
  const auto first_provider_path = first_dir / "custom_cuda_provider";
  const auto second_provider_path = second_dir / "custom_cuda_provider";
  std::ofstream(first_provider_path, std::ios::binary) << "test provider";
  std::ofstream(second_provider_path, std::ios::binary) << "test provider";

  int registration_count = 0;
  int load_count = 0;
  int release_count = 0;
  bool allow_registration = false;
  std::vector<std::filesystem::path> requested_paths;
  auto loader = [&](const std::filesystem::path& path, ILogger&) -> std::shared_ptr<void> {
    requested_paths.push_back(path);
    ++load_count;
    return std::shared_ptr<void>(new int(load_count), [&](void* token) {
      ++release_count;
      delete static_cast<int*>(token);
    });
  };

  CudaEpBootstrapper bootstrapper(root.string(), [&](const std::string&, const std::filesystem::path&) {
    ++registration_count;
    return allow_registration; },
                                  /*manifest_factory=*/nullptr,
                                  /*download_fn=*/nullptr, loader);
  StderrLogger logger;

  {
    test::ScopedEnvironmentVariable override(kOverrideEnv, first_provider_path.string());
    EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  }

  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_EQ(registration_count, 1);
  EXPECT_EQ(load_count, 1);
  EXPECT_EQ(release_count, 1);
  ASSERT_EQ(requested_paths.size(), 1u);
  EXPECT_EQ(requested_paths[0],
            std::filesystem::absolute(first_dir / "libonnxruntime-genai-cuda.so").lexically_normal());

  allow_registration = true;
  {
    test::ScopedEnvironmentVariable override(kOverrideEnv, second_provider_path.string());
    EXPECT_TRUE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  }

  EXPECT_TRUE(bootstrapper.IsRegistered());
  EXPECT_EQ(registration_count, 2);
  EXPECT_EQ(load_count, 2) << "registration failure must not keep a provisional preload alive across retries";
  EXPECT_EQ(release_count, 1) << "the successful retry keeps its preload owned by the bootstrapper";
  ASSERT_EQ(requested_paths.size(), 2u);
  EXPECT_EQ(requested_paths[1],
            std::filesystem::absolute(second_dir / "libonnxruntime-genai-cuda.so").lexically_normal());
}
#endif

}  // namespace fl
