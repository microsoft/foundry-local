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
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || \
    (defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__)))
  EXPECT_TRUE(CudaEpBootstrapper::IsSupportedPlatform());
#else
  EXPECT_FALSE(CudaEpBootstrapper::IsSupportedPlatform());
#endif
}

TEST(CudaEpManifestTest, WindowsX64MetadataMatches20261014Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::WindowsX64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-win-x64-ort-1.31.0-genai-0.18.0-20261014-064231");
  EXPECT_EQ(manifest->provider_relative_path, "onnxruntime_providers_cuda.dll");
  ASSERT_EQ(manifest->artifacts.size(), 1u);
  ExpectArtifact(
      manifest->artifacts[0], "cuda-ep", "cuda-ep-bins-win-x64-20261014-064231.zip",
      "ad7e1d0ad84d0ca6861848fa31f2cd92c548b67c57d264d819f26bda03383b98", 256 * kMiB,
      {
          {"onnxruntime-genai-cuda.dll", "d9bc0e9a737d6169b2e801f13bf87a51e7182f65ad64ba8f0c4ee3037be12457"},
          {"onnxruntime_providers_cuda.dll", "a884a89c137db78211c731956e038edde7617122d329d1eeaa732a5926e11962"},
      });
  ExpectUniqueInstalledPaths(*manifest);
}

TEST(CudaEpManifestTest, WindowsArm64MetadataMatches20261014Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::WindowsArm64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-win-arm64-ort-1.31.0-genai-0.18.0-20261014-064257");
  EXPECT_EQ(manifest->provider_relative_path, "onnxruntime_providers_cuda.dll");
  ASSERT_EQ(manifest->artifacts.size(), 1u);
  ExpectArtifact(
      manifest->artifacts[0], "cuda-ep", "cuda-ep-bins-win-arm64-20261014-064257.zip",
      "728acba50d92f7c41d9a848bfc122f628959dddf3ca25293f31e3c963aff5209", 96 * kMiB,
      {
          {"onnxruntime-genai-cuda.dll", "6727494a28d352dc2992c79f5320410b70a1d954a92dd452b34b0ee2de790617"},
          {"onnxruntime_providers_cuda.dll", "54f4d246a26eda7fc623eac215349a128b43091d2a1563d73142756642250385"},
      });
  ExpectUniqueInstalledPaths(*manifest);
}

TEST(CudaEpManifestTest, LinuxX64MetadataMatches20261014Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::LinuxX64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-linux-x64-ort-1.31.0-genai-0.18.0-20261014-064302");
  EXPECT_EQ(manifest->provider_relative_path, "libonnxruntime_providers_cuda.so");
  ASSERT_EQ(manifest->artifacts.size(), 1u);

  ExpectArtifact(
      manifest->artifacts[0], "cuda-ep", "cuda-ep-linux-x64-20261014-064302.zip",
      "d83605c5e963aa571d33c8837cf6c97094bab98877e8143c56228f692cf6c53f", 448 * kMiB,
      {
          {"libonnxruntime-genai-cuda.so", "858b1cc5fc258f1ed63f95c98bd2f1fde3002a490bf3e3be2736bfdbef96ecc8"},
          {"libonnxruntime_providers_cuda.so", "f3a15157d4252001ee0bd4eae3bbc31b3d0bc151dc950cb038d1f6550e9f1c94"},
      });
  ExpectUniqueInstalledPaths(*manifest);
}

TEST(CudaEpManifestTest, LinuxArm64MetadataMatches20261014Bundle) {
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::LinuxArm64);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->bundle_id, "cuda-ep-linux-arm64-ort-1.31.0-genai-0.18.0-20261014-064346");
  EXPECT_EQ(manifest->provider_relative_path, "libonnxruntime_providers_cuda.so");
  ASSERT_EQ(manifest->artifacts.size(), 1u);

  ExpectArtifact(
      manifest->artifacts[0], "cuda-ep", "cuda-ep-linux-arm64-20261014-064346.zip",
      "9ddc52bb1e6ae2abf5a37debb1af4d9c148becc29448dca32b6d94a3a50d480e", 224 * kMiB,
      {
          {"libonnxruntime-genai-cuda.so", "208550d347239316be71cc982ae35e0156dd70b2380b49362131e30138cab3d0"},
          {"libonnxruntime_providers_cuda.so", "3b66400c90219f3527992fa93e39df44f54ea2bcb730e55a729d7d37d1d9fc30"},
      });
  ExpectUniqueInstalledPaths(*manifest);
}

TEST(CudaEpManifestTest, UnsupportedPlatformsHaveNoManifest) {
  EXPECT_FALSE(BuildCudaEpManifest(CudaEpPlatform::Unsupported).has_value());
}

#if defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
TEST(CudaEpBootstrapperTest, DefaultManifestMatchesHostArchitecture) {
#if defined(__aarch64__)
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::LinuxArm64);
#else
  const auto manifest = BuildCudaEpManifest(CudaEpPlatform::LinuxX64);
#endif
  ASSERT_TRUE(manifest.has_value());
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  test::ScopedEnvironmentVariable override(kOverrideEnv, "");
  std::string requested_url;
  CudaEpBootstrapper bootstrapper(
      root.string(),
      [](const std::string&, const std::filesystem::path&) {
        ADD_FAILURE() << "Registration should not run after a failed download";
        return false;
      },
      nullptr,
      [&](const std::string& url, const std::filesystem::path&, uint64_t, std::atomic<bool>*,
          const std::function<void(float)>&, ILogger&) {
        requested_url = url;
        return false;
      });
  NullLogger logger;

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, nullptr, logger));
  EXPECT_EQ(requested_url, manifest->artifacts.front().url);
}
#endif

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

TEST(CudaEpBootstrapperTest, ProviderPreloadFailureReleasesBridgeAndSkipsRegistration) {
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  FakeDownloads downloads;
  const auto manifest = MakeRawManifest(downloads, "bundle-v1");
  NullLogger logger;
  std::weak_ptr<void> bridge;
  int load_count = 0;
  int registration_count = 0;
  auto loader = [&](const std::filesystem::path& path, ILogger&) -> std::shared_ptr<void> {
    ++load_count;
    if (path.filename() != kGenAiCudaLibrary) return {};
    auto library = std::make_shared<int>(0);
    bridge = library;
    return library;
  };

  CudaEpBootstrapper bootstrapper(
      root.string(),
      [&](const std::string&, const std::filesystem::path&) {
        ++registration_count;
        return true;
      },
      [manifest] { return std::optional<EpBundleManifest>(manifest); }, downloads.AsFn(), loader);

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, nullptr, logger));
  EXPECT_EQ(load_count, 2);
  EXPECT_EQ(registration_count, 0);
  EXPECT_TRUE(bridge.expired());
  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_FALSE(std::filesystem::exists(root.path() / "active"));
}

TEST(CudaEpBootstrapperTest, RegistrationFailureReleasesBothPreloadsAndRetryRetainsThem) {
  auto root = test::TempPath::CreateTempDir("fl_cuda_bootstrapper_");
  FakeDownloads downloads;
  const auto manifest = MakeRawManifest(downloads, "bundle-v1");
  NullLogger logger;
  int live_libraries = 0;
  int load_count = 0;
  int registration_count = 0;
  std::vector<std::filesystem::path> requested_paths;
  auto loader = [&](const std::filesystem::path& path, ILogger&) -> std::shared_ptr<void> {
    requested_paths.push_back(path);
    ++load_count;
    ++live_libraries;
    return std::shared_ptr<void>(new int(0), [&](void* token) {
      --live_libraries;
      delete static_cast<int*>(token);
    });
  };

  {
    CudaEpBootstrapper bootstrapper(
        root.string(),
        [&](const std::string&, const std::filesystem::path& path) {
          EXPECT_EQ(live_libraries, 2);
          EXPECT_EQ(requested_paths,
                    (std::vector<std::filesystem::path>{path.parent_path() / kGenAiCudaLibrary, path}));
          requested_paths.clear();
          return ++registration_count > 1;
        },
        [manifest] { return std::optional<EpBundleManifest>(manifest); }, downloads.AsFn(), loader);

    EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, nullptr, logger));
    EXPECT_EQ(live_libraries, 0);
    EXPECT_TRUE(bootstrapper.DownloadAndRegister(false, nullptr, logger));
    EXPECT_EQ(live_libraries, 2);
    EXPECT_TRUE(bootstrapper.DownloadAndRegister(false, nullptr, logger));
    EXPECT_EQ(load_count, 4);
  }

  EXPECT_EQ(live_libraries, 0);
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
