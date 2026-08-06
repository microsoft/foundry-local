// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/webgpu_ep_bootstrapper.h"

#include "ep_detection/ep_bundle_installer.h"
#include "logger.h"
#include "util/sha256.h"
#include "utils/scoped_environment_variable.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fl {

namespace {

constexpr const char* kOverrideEnv = "FOUNDRY_LOCAL_WEBGPU_EP_LIBRARY";
constexpr const char* kScopedEnvironmentVariableTestEnv = "FOUNDRY_LOCAL_SCOPED_ENVIRONMENT_VARIABLE_TEST";
constexpr const char* kLockFileName = "webgpu-ep.lock";

class RecordingLogger : public ILogger {
 public:
  void Log(LogLevel level, std::string_view message) override {
    entries.emplace_back(level, std::string(message));
  }

  std::vector<std::pair<LogLevel, std::string>> entries;
};

std::vector<uint8_t> AsBytes(std::string_view text) { return std::vector<uint8_t>(text.begin(), text.end()); }

std::string HashOf(const std::vector<uint8_t>& bytes) {
  auto tmp = test::TempPath::CreateTempFile("fl_webgpu_bootstrapper_hash_");
  std::ofstream out(tmp.path(), std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  return Sha256File(tmp.path());
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class FakeDownloads {
 public:
  void SetSequence(const std::string& url, std::vector<std::vector<uint8_t>> payloads) {
    payloads_[url] = std::move(payloads);
  }

  EpArtifactDownloadFn AsFn() {
    return [this](const std::string& url, const std::filesystem::path& destination, uint64_t /*max_bytes*/,
                  std::atomic<bool>* cancel_flag, const std::function<void(float)>& progress_cb,
                  ILogger& /*logger*/) -> bool {
      auto it = payloads_.find(url);
      if (it == payloads_.end() || it->second.empty()) {
        return false;
      }

      if (progress_cb) {
        progress_cb(0.0f);
      }

      if (cancel_flag && cancel_flag->load()) {
        return false;
      }

      int& count = call_counts_[url];
      const size_t index = std::min(static_cast<size_t>(count), it->second.size() - 1);
      const auto& bytes = it->second[index];
      count++;

      std::filesystem::create_directories(destination.parent_path());
      std::ofstream out(destination, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      out.close();

      if (progress_cb) {
        progress_cb(100.0f);
      }

      return true;
    };
  }

 private:
  std::map<std::string, std::vector<std::vector<uint8_t>>> payloads_;
  std::map<std::string, int> call_counts_;
};

EpBundleManifest MakeRawManifest(const std::string& bundle_id, const std::string& url) {
  const auto payload = AsBytes(bundle_id + "-provider");

  EpBundleManifest manifest;
  manifest.bundle_id = bundle_id;
  manifest.provider_relative_path = "provider.so";
  manifest.artifacts = {EpBundleArtifact{.id = "provider",
                                         .url = url,
                                         .is_archive = false,
                                         .archive_sha256 = "",
                                         .extracted_files = {},
                                         .ignored_archive_paths = {},
                                         .archive_max_bytes = 0,
                                         .raw_relative_path = "provider.so",
                                         .raw_sha256 = HashOf(payload),
                                         .raw_max_bytes = 1024}};
  return manifest;
}

std::optional<std::filesystem::path> InstallAndFinalize(const std::filesystem::path& root,
                                                        const EpBundleManifest& manifest,
                                                        EpArtifactDownloadFn download_fn,
                                                        ILogger& logger) {
  EpBundleInstaller installer(root, kLockFileName, "WebGPU EP", std::move(download_fn));
  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  if (!txn) {
    return std::nullopt;
  }

  const auto bin_dir = txn->bin_dir();
  if (!txn->Activate(logger)) {
    return std::nullopt;
  }

  txn->Finalize(logger);
  return bin_dir;
}

std::optional<std::string> FindGenerationWithPrefix(const std::filesystem::path& bundles_dir, std::string_view prefix) {
  for (const auto& entry : std::filesystem::directory_iterator(bundles_dir)) {
    const auto generation = entry.path().filename().string();
    if (generation.starts_with(prefix)) {
      return generation;
    }
  }

  return std::nullopt;
}

}  // namespace

TEST(WebGpuEpBootstrapperTest, PlatformSupportMatchesPublishedBundles) {
#if (defined(_WIN32) && (defined(_M_ARM64) || defined(_M_X64))) || (defined(__APPLE__) && defined(__aarch64__))
  EXPECT_TRUE(WebGpuEpBootstrapper::IsSupportedPlatform());
#else
  EXPECT_FALSE(WebGpuEpBootstrapper::IsSupportedPlatform());
#endif
}

TEST(WebGpuEpBootstrapperTest, ScopedEnvironmentVariableRestoresExistingValue) {
  test::ScopedEnvironmentVariable restore_original(kScopedEnvironmentVariableTestEnv, "before");

  {
    test::ScopedEnvironmentVariable environment(kScopedEnvironmentVariableTestEnv, "during");
    EXPECT_EQ(test::GetEnvironmentVariable(kScopedEnvironmentVariableTestEnv), "during");
  }

  EXPECT_EQ(test::GetEnvironmentVariable(kScopedEnvironmentVariableTestEnv), "before");
}

TEST(WebGpuEpBootstrapperTest, ScopedEnvironmentVariableRestoresUnsetValue) {
  test::ScopedEnvironmentVariable restore_original(kScopedEnvironmentVariableTestEnv, "before");
  test::SetEnvironmentVariable(kScopedEnvironmentVariableTestEnv, std::nullopt);

  {
    test::ScopedEnvironmentVariable environment(kScopedEnvironmentVariableTestEnv, "during");
    EXPECT_EQ(test::GetEnvironmentVariable(kScopedEnvironmentVariableTestEnv), "during");
  }

  EXPECT_EQ(test::GetEnvironmentVariable(kScopedEnvironmentVariableTestEnv), std::nullopt);
}

TEST(WebGpuEpBootstrapperTest, OverrideRegistersUsingExistingProviderConvention) {
  auto root = test::TempPath::CreateTempDir("fl_webgpu_bootstrapper_");
  const auto provider_path = root.path() / "custom_webgpu_provider";
  std::ofstream(provider_path, std::ios::binary) << "test provider";
  test::ScopedEnvironmentVariable override(kOverrideEnv, provider_path.string());

  std::string registered_name;
  std::filesystem::path registered_path;
  int registration_count = 0;
  auto register_ep = [&](const std::string& name, const std::filesystem::path& path) {
    registered_name = name;
    registered_path = path;
    ++registration_count;
    return true;
  };
  WebGpuEpBootstrapper bootstrapper(root.string(), register_ep);
  StderrLogger logger;
  std::vector<std::pair<std::string, float>> progress;

  EXPECT_TRUE(bootstrapper.DownloadAndRegister(
      false,
      [&](const std::string& name, float percent) {
        progress.emplace_back(name, percent);
        return true;
      },
      logger));

  EXPECT_TRUE(bootstrapper.IsRegistered());
  EXPECT_EQ(registered_name, "Foundry.WebGPU");
  EXPECT_EQ(registered_path, std::filesystem::absolute(provider_path));
  EXPECT_EQ(registration_count, 1);
  ASSERT_EQ(progress.size(), 2u);
  EXPECT_EQ(progress[0], std::make_pair(std::string("WebGpuExecutionProvider"), kEpReadyToRegisterProgress));
  EXPECT_EQ(progress[1], std::make_pair(std::string("WebGpuExecutionProvider"), 100.0f));

  progress.clear();
  EXPECT_TRUE(bootstrapper.DownloadAndRegister(
      false,
      [&](const std::string& name, float percent) {
        progress.emplace_back(name, percent);
        return true;
      },
      logger));

  EXPECT_EQ(registration_count, 1);
  ASSERT_EQ(progress.size(), 1u);
  EXPECT_EQ(progress[0], std::make_pair(std::string("WebGpuExecutionProvider"), 100.0f));
}

TEST(WebGpuEpBootstrapperTest, OverrideCancellationBeforeRegistrationReturnsFalse) {
  auto root = test::TempPath::CreateTempDir("fl_webgpu_bootstrapper_");
  const auto provider_path = root.path() / "custom_webgpu_provider";
  std::ofstream(provider_path, std::ios::binary) << "test provider";
  test::ScopedEnvironmentVariable override(kOverrideEnv, provider_path.string());

  int registration_count = 0;
  WebGpuEpBootstrapper bootstrapper(root.string(), [&](const std::string&, const std::filesystem::path&) {
    ++registration_count;
    return true;
  });
  StderrLogger logger;

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(
      false, [](const std::string&, float percent) { return percent != kEpReadyToRegisterProgress; }, logger));
  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_EQ(registration_count, 0);
}

TEST(WebGpuEpBootstrapperTest, BundleActivationFailurePreventsRegistration) {
  auto root = test::TempPath::CreateTempDir("fl_webgpu_bootstrapper_");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider-v1.so", {AsBytes("bundle-v1-provider")});
  const auto manifest = MakeRawManifest("bundle-v1", "https://example.test/provider-v1.so");

  std::filesystem::create_directories(root.path());
  std::filesystem::create_directory(root.path() / "active");
  {
    std::ofstream(root.path() / "active" / "blocker") << "x";
  }

  int registration_count = 0;
  RecordingLogger logger;
  WebGpuEpBootstrapper bootstrapper(
      root.string(),
      [&](const std::string&, const std::filesystem::path&) {
        ++registration_count;
        return true;
      },
      [manifest] { return std::optional<EpBundleManifest>(manifest); },
      downloads.AsFn());

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  EXPECT_EQ(registration_count, 0);
  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_TRUE(std::filesystem::is_directory(root.path() / "active"));
}

TEST(WebGpuEpBootstrapperTest, BundleRegistrationFailureRollsBackToPreviousGeneration) {
  auto root = test::TempPath::CreateTempDir("fl_webgpu_bootstrapper_");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider-v1.so", {AsBytes("bundle-v1-provider")});
  downloads.SetSequence("https://example.test/provider-v2.so", {AsBytes("bundle-v2-provider")});
  const auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/provider-v1.so");
  const auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/provider-v2.so");
  RecordingLogger logger;

  ASSERT_TRUE(InstallAndFinalize(root.path(), manifest_v1, downloads.AsFn(), logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");

  int registration_count = 0;
  WebGpuEpBootstrapper bootstrapper(
      root.string(),
      [&](const std::string&, const std::filesystem::path&) {
        ++registration_count;
        return false;
      },
      [manifest_v2] { return std::optional<EpBundleManifest>(manifest_v2); },
      downloads.AsFn());

  EXPECT_FALSE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  EXPECT_EQ(registration_count, 1);
  EXPECT_FALSE(bootstrapper.IsRegistered());
  EXPECT_EQ(ReadFile(root.path() / "active"), active_v1);
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1));

  const auto candidate_v2 = FindGenerationWithPrefix(root.path() / "bundles", "bundle-v2-");
  ASSERT_TRUE(candidate_v2.has_value());
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / *candidate_v2));
}

TEST(WebGpuEpBootstrapperTest, SuccessfulBundleRegistrationFinalizesPreviousGenerationCleanup) {
  auto root = test::TempPath::CreateTempDir("fl_webgpu_bootstrapper_");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider-v1.so", {AsBytes("bundle-v1-provider")});
  downloads.SetSequence("https://example.test/provider-v2.so", {AsBytes("bundle-v2-provider")});
  const auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/provider-v1.so");
  const auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/provider-v2.so");
  RecordingLogger logger;

  ASSERT_TRUE(InstallAndFinalize(root.path(), manifest_v1, downloads.AsFn(), logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");

  int registration_count = 0;
  std::filesystem::path registered_path;
  WebGpuEpBootstrapper bootstrapper(
      root.string(),
      [&](const std::string&, const std::filesystem::path& path) {
        ++registration_count;
        registered_path = path;
        return true;
      },
      [manifest_v2] { return std::optional<EpBundleManifest>(manifest_v2); },
      downloads.AsFn());

  EXPECT_TRUE(bootstrapper.DownloadAndRegister(false, /*progress_cb=*/nullptr, logger));
  EXPECT_EQ(registration_count, 1);
  EXPECT_TRUE(bootstrapper.IsRegistered());
  EXPECT_EQ(registered_path.filename(), "provider.so");

  const auto active_v2 = ReadFile(root.path() / "active");
  EXPECT_TRUE(active_v2.starts_with("bundle-v2-"));
  EXPECT_FALSE(std::filesystem::exists(root.path() / "bundles" / active_v1));
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v2));
}

}  // namespace fl
