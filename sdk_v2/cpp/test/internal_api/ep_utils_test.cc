// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_utils.h"

#include "logger.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

namespace fl {

namespace {

class NullLogger : public ILogger {
 public:
  void Log(LogLevel /*level*/, std::string_view /*message*/) override {}
};

// A representative manifest mixing the provider DLL, non-DLL manifest entries, core ORT runtime DLLs
// (in mixed case, to exercise case-insensitive exclusion), and DLL dependencies that should be
// preloaded, spread across more than one artifact.
EpBundleManifest MakeCudaLikeManifest() {
  EpBundleManifest manifest;
  manifest.bundle_id = "test-cuda-ep";
  manifest.provider_relative_path = "onnxruntime_providers_cuda.dll";
  manifest.artifacts = {
      EpBundleArtifact{
          .id = "cuda-ep",
          .url = "https://example.test/cuda-ep.zip",
          .is_archive = true,
          .archive_sha256 = "archive-hash",
          .extracted_files =
              {
                  {.relative_path = "onnxruntime_providers_cuda.dll", .sha256 = "a"},
                  {.relative_path = "cudart64_12.dll", .sha256 = "b"},
                  {.relative_path = "ONNXRUNTIME.DLL", .sha256 = "c"},
                  {.relative_path = "onnxruntime-genai.dll", .sha256 = "d"},
                  {.relative_path = "version.json", .sha256 = "e"},
              },
          .archive_max_bytes = 0,
          .raw_relative_path = "",
          .raw_sha256 = "",
          .raw_max_bytes = 0,
      },
      EpBundleArtifact{
          .id = "cuda-toolkit",
          .url = "https://example.test/cuda-toolkit.zip",
          .is_archive = true,
          .archive_sha256 = "archive-hash",
          .extracted_files =
              {
                  {.relative_path = "cublas64_12.DLL", .sha256 = "f"},
              },
          .archive_max_bytes = 0,
          .raw_relative_path = "",
          .raw_sha256 = "",
          .raw_max_bytes = 0,
      },
  };
  return manifest;
}

}  // namespace

TEST(EpUtilsTest, SelectEpBundleDependenciesToPreloadExcludesProviderAndCoreRuntimeCaseInsensitively) {
  const std::filesystem::path bin_dir = std::filesystem::path("opt") / "foundry" / "cuda-ep" / "bin";
  const auto manifest = MakeCudaLikeManifest();

  const auto dependencies = SelectEpBundleDependenciesToPreload(bin_dir, manifest);

  // Only the two non-provider, non-core-runtime DLLs should remain, in manifest order, with
  // version.json (not a DLL) and the provider/core runtime DLLs (matched case-insensitively) excluded.
  ASSERT_EQ(dependencies.size(), 2u);
  EXPECT_EQ(dependencies[0], std::filesystem::absolute(bin_dir / "cudart64_12.dll"));
  EXPECT_EQ(dependencies[1], std::filesystem::absolute(bin_dir / "cublas64_12.DLL"));
  EXPECT_TRUE(dependencies[0].is_absolute());
  EXPECT_TRUE(dependencies[1].is_absolute());
}

TEST(EpUtilsTest, SelectEpBundleDependenciesToPreloadReturnsEmptyWhenOnlyProviderAndCoreRuntimePresent) {
  EpBundleManifest manifest;
  manifest.bundle_id = "test-webgpu-ep";
  manifest.provider_relative_path = "onnxruntime_providers_webgpu.dll";
  manifest.artifacts = {
      EpBundleArtifact{
          .id = "webgpu-ep",
          .url = "https://example.test/webgpu-ep.zip",
          .is_archive = true,
          .archive_sha256 = "archive-hash",
          .extracted_files =
              {
                  {.relative_path = "onnxruntime_providers_webgpu.dll", .sha256 = "a"},
                  {.relative_path = "onnxruntime.dll", .sha256 = "b"},
                  {.relative_path = "version.json", .sha256 = "c"},
              },
          .archive_max_bytes = 0,
          .raw_relative_path = "",
          .raw_sha256 = "",
          .raw_max_bytes = 0,
      },
  };

  EXPECT_TRUE(SelectEpBundleDependenciesToPreload("bin", manifest).empty());
}

TEST(EpUtilsTest, SelectEpBundleDependenciesToPreloadHandlesManifestWithNoArtifacts) {
  EpBundleManifest manifest;
  manifest.bundle_id = "empty";
  manifest.provider_relative_path = "provider.dll";

  EXPECT_TRUE(SelectEpBundleDependenciesToPreload("bin", manifest).empty());
}

TEST(EpUtilsTest, LoadEpBundleDependenciesIsNoOpTrueOnNonWindows) {
#ifndef _WIN32
  NullLogger logger;
  const auto manifest = MakeCudaLikeManifest();

  // LoadEpBundleDependencies is documented as a no-op that always returns true on non-Windows
  // platforms; neither the directory nor the dependency files need to exist here.
  EXPECT_TRUE(LoadEpBundleDependencies("nonexistent/bin/dir", manifest, "Test EP", logger));
#else
  GTEST_SKIP() << "Preloading behavior is exercised on Windows only.";
#endif
}

}  // namespace fl
