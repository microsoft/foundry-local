// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_bundle_installer.h"

#include "internal_api/ep_bundle_test_helpers.h"
#include "internal_api/test_helpers.h"
#include "logger.h"
#include "util/file_lock.h"

#include "utils/temp_path.h"
#include "utils/zip_builder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fl {

namespace {

class RecordingLogger : public ILogger {
 public:
  void Log(LogLevel level, std::string_view message) override {
    entries.emplace_back(level, std::string(message));
  }

  bool Contains(std::string_view fragment) const {
    return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
      return entry.second.find(fragment) != std::string::npos;
    });
  }

  std::vector<std::pair<LogLevel, std::string>> entries;
};

using test::AsBytes;
using test::FakeDownloads;
using test::HashOf;
using test::InstallAndFinalize;
using test::NullLogger;
using test::ReadFile;

EpBundleManifest MakeRawManifest(const std::string& bundle_id, const std::string& url, const std::string& sha256) {
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
                                         .raw_sha256 = sha256,
                                         .raw_max_bytes = 1024}};
  return manifest;
}

EpBundleManifest MakeArchiveManifest(const std::string& bundle_id, const std::string& url,
                                     const std::string& archive_sha256, std::vector<EpBundleFile> extracted_files,
                                     std::vector<std::string> ignored_archive_paths = {}) {
  EpBundleManifest manifest;
  manifest.bundle_id = bundle_id;
  manifest.provider_relative_path = "provider.dll";
  manifest.artifacts = {EpBundleArtifact{.id = "archive",
                                         .url = url,
                                         .is_archive = true,
                                         .archive_sha256 = archive_sha256,
                                         .extracted_files = std::move(extracted_files),
                                         .ignored_archive_paths = std::move(ignored_archive_paths),
                                         .archive_max_bytes = 1024 * 1024,
                                         .raw_relative_path = "",
                                         .raw_sha256 = "",
                                         .raw_max_bytes = 0}};
  return manifest;
}

}  // namespace

TEST(EpBundleInstallerTest, UnsupportedManifestFailsClosedWithoutDownloading) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  FakeDownloads downloads;
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());

  EpBundleManifest manifest;
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  EXPECT_EQ(txn, nullptr);
  EXPECT_EQ(downloads.CallCount(""), 0);
}

TEST(EpBundleInstallerTest, InstallsRawArtifactAndVerifiesContent) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("provider-binary-contents");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});

  auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  EXPECT_EQ(ReadFile(txn->bin_dir() / "provider.so"), "provider-binary-contents");
  EXPECT_EQ(downloads.CallCount("https://example.test/provider.so"), 1);
}

TEST(EpBundleInstallerTest, ReusesValidBundleWithoutRedownloading) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("provider-binary-contents");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});

  auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  std::filesystem::path first_bin;
  {
    auto first = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
    ASSERT_NE(first, nullptr);
    first_bin = first->bin_dir();
    ASSERT_TRUE(first->Activate());
    first->Finalize();
  }

  auto second = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first_bin, second->bin_dir());
  EXPECT_EQ(downloads.CallCount("https://example.test/provider.so"), 1)
      << "reusing an already-verified bundle must not re-download";
}

TEST(EpBundleInstallerTest, ForceDownloadRedownloadsEveryArtifactFromValidActiveBundle) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  const auto first = AsBytes("first");
  const auto second = AsBytes("second");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/first.bin", {first});
  downloads.SetSequence("https://example.test/second.bin", {second});

  EpBundleManifest manifest;
  manifest.bundle_id = "bundle";
  manifest.provider_relative_path = "first.bin";
  manifest.artifacts = {
      EpBundleArtifact{.id = "first",
                       .url = "https://example.test/first.bin",
                       .is_archive = false,
                       .archive_sha256 = "",
                       .extracted_files = {},
                       .ignored_archive_paths = {},
                       .archive_max_bytes = 0,
                       .raw_relative_path = "first.bin",
                       .raw_sha256 = HashOf(first),
                       .raw_max_bytes = 1024},
      EpBundleArtifact{.id = "second",
                       .url = "https://example.test/second.bin",
                       .is_archive = false,
                       .archive_sha256 = "",
                       .extracted_files = {},
                       .ignored_archive_paths = {},
                       .archive_max_bytes = 0,
                       .raw_relative_path = "second.bin",
                       .raw_sha256 = HashOf(second),
                       .raw_max_bytes = 1024},
  };

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;
  ASSERT_TRUE(InstallAndFinalize(installer, manifest, logger).has_value());

  auto replacement =
      installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger, EpBundleInstallPolicy::ForceDownload);

  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(ReadFile(replacement->bin_dir() / "first.bin"), "first");
  EXPECT_EQ(ReadFile(replacement->bin_dir() / "second.bin"), "second");
  EXPECT_EQ(downloads.CallCount("https://example.test/first.bin"), 2);
  EXPECT_EQ(downloads.CallCount("https://example.test/second.bin"), 2);
}

TEST(EpBundleInstallerTest, RawHashMismatchFailsWithoutRetry) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto wrong_payload = AsBytes("this-is-not-what-you-expected");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {wrong_payload});

  auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(AsBytes("expected")));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  EXPECT_EQ(txn, nullptr);
  EXPECT_EQ(downloads.CallCount("https://example.test/provider.so"), 1)
      << "raw artifacts get zero retries on a hash mismatch";
}

TEST(EpBundleInstallerTest, ArchiveHashMismatchRetriesOnceThenSucceeds) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");

  test::ZipBuilder builder;
  builder.AddEntry("provider.dll", AsBytes("dll-bytes"));
  auto good_archive = builder.Build();
  auto bad_archive = AsBytes("not-a-real-zip-at-all");

  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/archive.zip", {bad_archive, good_archive});

  auto manifest =
      MakeArchiveManifest("bundle-1", "https://example.test/archive.zip", HashOf(good_archive),
                          {EpBundleFile{.relative_path = "provider.dll", .sha256 = HashOf(AsBytes("dll-bytes"))}});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  EXPECT_EQ(ReadFile(txn->bin_dir() / "provider.dll"), "dll-bytes");
  EXPECT_EQ(downloads.CallCount("https://example.test/archive.zip"), 2);
}

TEST(EpBundleInstallerTest, ArchiveHashMismatchTwiceFails) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto bad_archive = AsBytes("still-not-a-zip");

  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/archive.zip", {bad_archive});

  auto manifest =
      MakeArchiveManifest("bundle-1", "https://example.test/archive.zip", HashOf(AsBytes("expected-archive")),
                          {EpBundleFile{.relative_path = "provider.dll", .sha256 = HashOf(AsBytes("provider"))}});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  EXPECT_EQ(txn, nullptr);
  EXPECT_EQ(downloads.CallCount("https://example.test/archive.zip"), 2)
      << "archives get exactly one fresh retry on a hash mismatch";
}

TEST(EpBundleInstallerTest, ExtractedFileMismatchFailsWithoutRetryEvenThoughArchiveHashMatched) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");

  test::ZipBuilder builder;
  builder.AddEntry("provider.dll", AsBytes("actual-content"));
  auto archive = builder.Build();

  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/archive.zip", {archive});

  auto manifest = MakeArchiveManifest(
      "bundle-1", "https://example.test/archive.zip", HashOf(archive),
      {EpBundleFile{.relative_path = "provider.dll", .sha256 = HashOf(AsBytes("different-content"))}});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  EXPECT_EQ(txn, nullptr);
  EXPECT_EQ(downloads.CallCount("https://example.test/archive.zip"), 1)
      << "extracted-member mismatches are never retried within the same call";
}

TEST(EpBundleInstallerTest, MissingExpectedExtractedFileFails) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");

  test::ZipBuilder builder;
  builder.AddEntry("provider.dll", AsBytes("actual-content"));
  auto archive = builder.Build();

  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/archive.zip", {archive});

  auto manifest =
      MakeArchiveManifest("bundle-1", "https://example.test/archive.zip", HashOf(archive),
                          {EpBundleFile{.relative_path = "provider.dll", .sha256 = HashOf(AsBytes("actual-content"))},
                           EpBundleFile{.relative_path = "missing.dll", .sha256 = HashOf(AsBytes("whatever"))}});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  EXPECT_EQ(txn, nullptr);
}

TEST(EpBundleInstallerTest, ActivateWritesActiveMarkerFile) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});

  auto manifest = MakeRawManifest("bundle-42", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  EXPECT_TRUE(txn->Activate());

  EXPECT_TRUE(ReadFile(root.path() / "active").starts_with("bundle-42-"));
}

TEST(EpBundleInstallerTest, ActivateReplacesExistingActiveMarker) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  std::filesystem::create_directories(root.path());
  {
    std::ofstream(root.path() / "active") << "stale-previous-generation";
  }

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  EXPECT_TRUE(txn->Activate());
  EXPECT_TRUE(ReadFile(root.path() / "active").starts_with("bundle-1-"))
      << "publishing replaces an existing active marker atomically";
}

TEST(EpBundleInstallerTest, InstallTransactionHoldsLockUntilReleased) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  const auto lock_path = root.path() / "test.lock";
  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);

  EXPECT_THROW({ FileLock probe(lock_path, /*timeout_ms=*/0); }, std::runtime_error);

  txn.reset();

  EXPECT_NO_THROW({ FileLock probe(lock_path, /*timeout_ms=*/0); });
}

TEST(EpBundleInstallerTest, ActivateRevalidatesUnderLockAndRefusesTamperedBundle) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndFinalize(installer, manifest_v1, logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");
  ASSERT_TRUE(active_v1.starts_with("bundle-v1-"));

  auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/v2.so", HashOf(payload_v2));
  auto txn = installer.EnsureInstalled(manifest_v2, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  {
    std::ofstream(txn->bin_dir() / "unexpected.txt") << "surprise";
  }

  EXPECT_FALSE(txn->Activate()) << "re-verification under the lock must reject a tampered bundle";
  EXPECT_EQ(ReadFile(root.path() / "active"), active_v1)
      << "a bundle failing re-verification must not advance the active marker";
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1))
      << "the previous generation is preserved when activation fails";
}

TEST(EpBundleInstallerTest, ActivatePreservesPreviousMarkerWhenPublicationFails) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndFinalize(installer, manifest_v1, logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");
  ASSERT_TRUE(active_v1.starts_with("bundle-v1-"));

  auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/v2.so", HashOf(payload_v2));
  auto txn = installer.EnsureInstalled(manifest_v2, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);

  std::filesystem::remove(root.path() / "active");
  std::filesystem::create_directory(root.path() / "active");
  {
    std::ofstream(root.path() / "active" / "blocker") << "x";
  }

  EXPECT_FALSE(txn->Activate()) << "a failed marker publication is reported as failure";
  EXPECT_TRUE(std::filesystem::is_directory(root.path() / "active"))
      << "the failed publication left the marker path untouched";
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1))
      << "the previous generation is preserved when publication fails";
  EXPECT_TRUE(std::filesystem::exists(txn->bin_dir()));
}

TEST(EpBundleInstallerTest, FinalizeRemovesOldGenerationsOnlyAfterSuccessfulActivation) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndFinalize(installer, manifest_v1, logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");
  ASSERT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1));

  auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/v2.so", HashOf(payload_v2));
  auto txn = installer.EnsureInstalled(manifest_v2, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  ASSERT_TRUE(txn->Activate());
  const auto active_v2 = ReadFile(root.path() / "active");

  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1))
      << "activation must not delete the previously active generation";
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v2));

  txn->Finalize();

  EXPECT_FALSE(std::filesystem::exists(root.path() / "bundles" / active_v1))
      << "finalization removes the previous generation only after registration succeeds";
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v2));
}

TEST(EpBundleInstallerTest, RollbackRestoresPreviousMarkerAndRetainsGenerations) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  const auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndFinalize(installer, manifest_v1, logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");

  const auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/v2.so", HashOf(payload_v2));
  auto txn = installer.EnsureInstalled(manifest_v2, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  ASSERT_TRUE(txn->Activate());
  ASSERT_NE(ReadFile(root.path() / "active"), active_v1);

  const auto candidate_generation = txn->bin_dir().parent_path().filename().string();
  EXPECT_TRUE(txn->Rollback());
  EXPECT_EQ(ReadFile(root.path() / "active"), active_v1);
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1));
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / candidate_generation));
}

TEST(EpBundleInstallerTest, RollbackFirstInstallRemovesCandidateMarkerAndRetainsGeneration) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("v1");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  const auto manifest = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload));
  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  ASSERT_TRUE(txn->Activate());

  const auto candidate_generation = txn->bin_dir().parent_path().filename().string();
  ASSERT_EQ(ReadFile(root.path() / "active"), candidate_generation);

  EXPECT_TRUE(txn->Rollback());
  EXPECT_FALSE(std::filesystem::exists(root.path() / "active"));
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / candidate_generation));
}

TEST(EpBundleInstallerTest, RollbackOnReusedActiveBundleLeavesMarkerUnchanged) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("v1");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  const auto manifest = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload));
  ASSERT_TRUE(InstallAndFinalize(installer, manifest, logger).has_value());
  const auto active_generation = ReadFile(root.path() / "active");

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  ASSERT_TRUE(txn->Activate());
  ASSERT_EQ(ReadFile(root.path() / "active"), active_generation);

  EXPECT_TRUE(txn->Rollback());
  EXPECT_EQ(ReadFile(root.path() / "active"), active_generation);
}

TEST(EpBundleInstallerTest, ActivatedTransactionDestructorRestoresPreviousMarkerBeforeReleasingLock) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  const auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndFinalize(installer, manifest_v1, logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");

  const auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/v2.so", HashOf(payload_v2));
  auto txn = installer.EnsureInstalled(manifest_v2, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  ASSERT_TRUE(txn->Activate());

  const auto lock_path = root.path() / "test.lock";
  EXPECT_THROW({ FileLock probe(lock_path, /*timeout_ms=*/0); }, std::runtime_error);

  txn.reset();

  EXPECT_EQ(ReadFile(root.path() / "active"), active_v1);
  EXPECT_NO_THROW({ FileLock probe(lock_path, /*timeout_ms=*/0); });
}

TEST(EpBundleInstallerTest, RollbackFailureLogsCacheRecoveryMessageAndRetainsGenerations) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  RecordingLogger logger;

  const auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndFinalize(installer, manifest_v1, logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");

  const auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/v2.so", HashOf(payload_v2));
  auto txn = installer.EnsureInstalled(manifest_v2, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  ASSERT_TRUE(txn->Activate());

  const auto candidate_generation = txn->bin_dir().parent_path().filename().string();
  std::filesystem::remove(root.path() / "active");
  std::filesystem::create_directory(root.path() / "active");
  {
    std::ofstream(root.path() / "active" / "blocker") << "x";
  }

  EXPECT_FALSE(txn->Rollback());
  EXPECT_TRUE(logger.Contains("failed to recover the active bundle marker after bootstrap failure"));
  EXPECT_TRUE(logger.Contains(root.path().string()));
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1));
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / candidate_generation));
}

TEST(EpBundleInstallerTest, FinalizeCleanupFailureKeepsPublishedMarker) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  RecordingLogger logger;

  const auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndFinalize(installer, manifest_v1, logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");

  const auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/v2.so", HashOf(payload_v2));
  auto txn = installer.EnsureInstalled(manifest_v2, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  ASSERT_TRUE(txn->Activate());
  const auto active_v2 = ReadFile(root.path() / "active");

  std::filesystem::rename(root.path() / "bundles", root.path() / "bundles_saved");
  {
    std::ofstream(root.path() / "bundles") << "block cleanup";
  }

  txn->Finalize();
  txn.reset();

  EXPECT_EQ(ReadFile(root.path() / "active"), active_v2);
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles_saved" / active_v1));
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles_saved" / active_v2));
}

TEST(EpBundleInstallerTest, StaleStagingDirectoryIsCleanedUpOnNextInstall) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  std::filesystem::create_directories(root.path() / "staging" / "leftover-from-a-crash");

  auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  EXPECT_FALSE(std::filesystem::exists(root.path() / "staging" / "leftover-from-a-crash"));
}

TEST(EpBundleInstallerTest, RejectsSymlinkedStagingDirectoryWithoutTraversingIt) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto outside = test::TempPath::CreateTempDir("fl_bundle_installer_outside_");
  std::ofstream(outside.path() / "must-remain.txt") << "preserved";
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside.path(), root.path() / "staging", ec);
  if (ec) {
    GTEST_SKIP() << "Directory symlinks are unavailable: " << ec.message();
  }

  const auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  const auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  EXPECT_EQ(installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger), nullptr);
  EXPECT_EQ(ReadFile(outside.path() / "must-remain.txt"), "preserved");
  EXPECT_EQ(downloads.CallCount("https://example.test/provider.so"), 0);
}

TEST(EpBundleInstallerTest, RejectsSymlinkedBundlesDirectoryWithoutTraversingIt) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto outside = test::TempPath::CreateTempDir("fl_bundle_installer_outside_");
  std::filesystem::create_directories(outside.path() / "orphan" / "bin");
  std::ofstream(outside.path() / "orphan" / "bin" / "must-remain.txt") << "preserved";
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside.path(), root.path() / "bundles", ec);
  if (ec) {
    GTEST_SKIP() << "Directory symlinks are unavailable: " << ec.message();
  }

  const auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  const auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  EXPECT_EQ(installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger), nullptr);
  EXPECT_EQ(ReadFile(outside.path() / "orphan" / "bin" / "must-remain.txt"), "preserved");
  EXPECT_EQ(downloads.CallCount("https://example.test/provider.so"), 0);
}

TEST(EpBundleInstallerTest, RejectsManagedDirectoryThatIsARegularFile) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  std::ofstream(root.path() / "bundles") << "not a directory";

  const auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  const auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  EXPECT_EQ(installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger), nullptr);
  EXPECT_EQ(ReadFile(root.path() / "bundles"), "not a directory");
  EXPECT_EQ(downloads.CallCount("https://example.test/provider.so"), 0);
}

TEST(EpBundleInstallerTest, DoesNotCopyUnexpectedFilesFromExistingBundle) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  ASSERT_TRUE(InstallAndFinalize(installer, manifest, logger).has_value());

  auto bin_dir = root.path() / "bundles" / ReadFile(root.path() / "active") / "bin";
  {
    std::ofstream(bin_dir / "unexpected.txt") << "surprise";
  }

  auto second = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(second, nullptr);
  EXPECT_FALSE(std::filesystem::exists(second->bin_dir() / "unexpected.txt"));
  EXPECT_EQ(downloads.CallCount("https://example.test/provider.so"), 1);
}

TEST(EpBundleInstallerTest, FreshInstallRejectsArchiveWithUndeclaredExtraFile) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");

  test::ZipBuilder builder;
  builder.AddEntry("provider.dll", AsBytes("dll-bytes"));
  builder.AddEntry("extra.txt", AsBytes("undeclared-payload"));
  auto archive = builder.Build();

  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/archive.zip", {archive});

  auto manifest =
      MakeArchiveManifest("bundle-1", "https://example.test/archive.zip", HashOf(archive),
                          {EpBundleFile{.relative_path = "provider.dll", .sha256 = HashOf(AsBytes("dll-bytes"))}});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  EXPECT_EQ(txn, nullptr);
  EXPECT_FALSE(std::filesystem::exists(root.path() / "active"))
      << "a bundle that never published must never be activated either";

  auto staging_dir = root.path() / "staging";
  if (std::filesystem::exists(staging_dir)) {
    EXPECT_TRUE(std::filesystem::is_empty(staging_dir));
  }
}

TEST(EpBundleInstallerTest, MultipleArchivesMayIgnoreSamePathWithoutInstallingIt) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");

  test::ZipBuilder first_builder;
  first_builder.AddEntry("first.dll", AsBytes("first"));
  first_builder.AddEntry("version.json", AsBytes("first metadata"));
  const auto first_archive = first_builder.Build();

  test::ZipBuilder second_builder;
  second_builder.AddEntry("provider.dll", AsBytes("provider"));
  second_builder.AddEntry("version.json", AsBytes("different metadata"));
  const auto second_archive = second_builder.Build();

  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/first.zip", {first_archive});
  downloads.SetSequence("https://example.test/second.zip", {second_archive});

  EpBundleManifest manifest;
  manifest.bundle_id = "bundle";
  manifest.provider_relative_path = "provider.dll";
  manifest.artifacts = {
      EpBundleArtifact{
          .id = "first",
          .url = "https://example.test/first.zip",
          .is_archive = true,
          .archive_sha256 = HashOf(first_archive),
          .extracted_files = {{.relative_path = "first.dll", .sha256 = HashOf(AsBytes("first"))}},
          .ignored_archive_paths = {"version.json"},
          .archive_max_bytes = 1024 * 1024,
          .raw_relative_path = "",
          .raw_sha256 = "",
          .raw_max_bytes = 0,
      },
      EpBundleArtifact{
          .id = "second",
          .url = "https://example.test/second.zip",
          .is_archive = true,
          .archive_sha256 = HashOf(second_archive),
          .extracted_files = {{.relative_path = "provider.dll", .sha256 = HashOf(AsBytes("provider"))}},
          .ignored_archive_paths = {"version.json"},
          .archive_max_bytes = 1024 * 1024,
          .raw_relative_path = "",
          .raw_sha256 = "",
          .raw_max_bytes = 0,
      },
  };

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;
  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);

  ASSERT_NE(txn, nullptr);
  EXPECT_EQ(ReadFile(txn->bin_dir() / "first.dll"), "first");
  EXPECT_EQ(ReadFile(txn->bin_dir() / "provider.dll"), "provider");
  EXPECT_FALSE(std::filesystem::exists(txn->bin_dir() / "version.json"));
  EXPECT_FALSE(std::filesystem::exists(txn->bin_dir().parent_path() / "artifacts"));
  EXPECT_TRUE(std::filesystem::is_empty(root.path() / "staging"));
}

TEST(EpBundleInstallerTest, RejectsRuntimeAndIgnoredPathOverlapWithoutDownloading) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  FakeDownloads downloads;
  const auto payload = AsBytes("provider");
  const auto manifest =
      MakeArchiveManifest("bundle", "https://example.test/archive.zip", HashOf(payload),
                          {{.relative_path = "provider.dll", .sha256 = HashOf(payload)}}, {"provider.dll"});

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  EXPECT_EQ(installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger), nullptr);
  EXPECT_EQ(downloads.CallCount("https://example.test/archive.zip"), 0);
}

TEST(EpBundleInstallerTest, RejectsArchiveMissingDeclaredIgnoredEntryAndCleansPrivateStaging) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  test::ZipBuilder builder;
  builder.AddEntry("provider.dll", AsBytes("provider"));
  const auto archive = builder.Build();

  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/archive.zip", {archive});
  const auto manifest =
      MakeArchiveManifest("bundle", "https://example.test/archive.zip", HashOf(archive),
                          {{.relative_path = "provider.dll", .sha256 = HashOf(AsBytes("provider"))}}, {"version.json"});

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  EXPECT_EQ(installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger), nullptr);
  EXPECT_TRUE(std::filesystem::is_empty(root.path() / "staging"));
}

TEST(EpBundleInstallerTest, ReuseDependsOnlyOnInstalledRuntimeFiles) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  test::ZipBuilder builder;
  builder.AddEntry("provider.dll", AsBytes("provider"));
  builder.AddEntry("version.json", AsBytes("metadata"));
  const auto archive = builder.Build();

  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/archive.zip", {archive});
  auto manifest =
      MakeArchiveManifest("bundle", "https://example.test/archive.zip", HashOf(archive),
                          {{.relative_path = "provider.dll", .sha256 = HashOf(AsBytes("provider"))}}, {"version.json"});

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;
  ASSERT_TRUE(InstallAndFinalize(installer, manifest, logger).has_value());

  manifest.artifacts.front().ignored_archive_paths = {"different-packaging-metadata.json"};
  auto reused = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);

  ASSERT_NE(reused, nullptr);
  EXPECT_EQ(downloads.CallCount("https://example.test/archive.zip"), 1);
  EXPECT_EQ(ReadFile(reused->bin_dir() / "provider.dll"), "provider");
}

TEST(EpBundleInstallerTest, ReusesValidArtifactsAndDownloadsOnlyMismatches) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  const auto first = AsBytes("first");
  const auto second = AsBytes("second");

  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/first.bin", {first});
  downloads.SetSequence("https://example.test/second.bin", {second});

  EpBundleManifest manifest;
  manifest.bundle_id = "bundle";
  manifest.provider_relative_path = "first.bin";
  manifest.artifacts = {
      EpBundleArtifact{.id = "first",
                       .url = "https://example.test/first.bin",
                       .is_archive = false,
                       .archive_sha256 = "",
                       .extracted_files = {},
                       .ignored_archive_paths = {},
                       .archive_max_bytes = 0,
                       .raw_relative_path = "first.bin",
                       .raw_sha256 = HashOf(first),
                       .raw_max_bytes = 1024},
      EpBundleArtifact{.id = "second",
                       .url = "https://example.test/second.bin",
                       .is_archive = false,
                       .archive_sha256 = "",
                       .extracted_files = {},
                       .ignored_archive_paths = {},
                       .archive_max_bytes = 0,
                       .raw_relative_path = "second.bin",
                       .raw_sha256 = HashOf(second),
                       .raw_max_bytes = 1024},
  };

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;
  ASSERT_TRUE(InstallAndFinalize(installer, manifest, logger).has_value());

  const auto active_bin = root.path() / "bundles" / ReadFile(root.path() / "active") / "bin";
  std::ofstream(active_bin / "second.bin", std::ios::binary | std::ios::trunc) << "corrupt";

  downloads.SetSequence("https://example.test/second.bin", {second});
  auto replacement = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(ReadFile(replacement->bin_dir() / "first.bin"), "first");
  EXPECT_EQ(ReadFile(replacement->bin_dir() / "second.bin"), "second");
  EXPECT_EQ(downloads.CallCount("https://example.test/first.bin"), 1);
  EXPECT_EQ(downloads.CallCount("https://example.test/second.bin"), 2);
}

TEST(EpBundleInstallerTest, CancellationDuringDownloadFails) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  IEpBootstrapper::ProgressCallback cancel_immediately = [](const std::string&, float) { return false; };
  auto txn = installer.EnsureInstalled(manifest, cancel_immediately, logger);
  EXPECT_EQ(txn, nullptr);
}

TEST(EpBundleInstallerTest, CancellationAtTerminalDownloaderProgressRemovesPartialGeneration) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  const auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  const auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto cancel_at_artifact_completion = [](const std::string&, float percent) { return percent < 80.0f; };

  EXPECT_EQ(installer.EnsureInstalled(manifest, cancel_at_artifact_completion, logger), nullptr);
  EXPECT_TRUE(std::filesystem::is_empty(root.path() / "staging"));
  EXPECT_TRUE(std::filesystem::is_empty(root.path() / "bundles"));
  EXPECT_FALSE(std::filesystem::exists(root.path() / "active"));
}

TEST(EpBundleInstallerTest, CancellationBeforeReturningNewTransactionRemovesPublishedGeneration) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  const auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  const auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto cancel_before_transaction = [](const std::string&, float percent) {
    return percent < kEpReadyToRegisterProgress;
  };

  EXPECT_EQ(installer.EnsureInstalled(manifest, cancel_before_transaction, logger), nullptr);
  EXPECT_TRUE(std::filesystem::is_empty(root.path() / "staging"));
  EXPECT_TRUE(std::filesystem::is_empty(root.path() / "bundles"));
  EXPECT_FALSE(std::filesystem::exists(root.path() / "active"));
}

TEST(EpBundleInstallerTest, CancellationOnVerifiedBundleReuseReturnsNoTransaction) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  const auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  const auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;
  ASSERT_TRUE(InstallAndFinalize(installer, manifest, logger).has_value());
  const auto active_generation = ReadFile(root.path() / "active");

  auto cancel_reuse = [](const std::string&, float percent) { return percent != kEpReadyToRegisterProgress; };

  EXPECT_EQ(installer.EnsureInstalled(manifest, cancel_reuse, logger), nullptr);
  EXPECT_EQ(downloads.CallCount("https://example.test/provider.so"), 1);
  EXPECT_EQ(ReadFile(root.path() / "active"), active_generation);
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_generation));
}

TEST(EpBundleInstallerTest, CancellationAfterCopiedArtifactRemovesStagingCopy) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  const auto first = AsBytes("first");
  const auto second = AsBytes("second");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/first.bin", {first});
  downloads.SetSequence("https://example.test/second.bin", {second});

  EpBundleManifest manifest;
  manifest.bundle_id = "bundle";
  manifest.provider_relative_path = "first.bin";
  manifest.artifacts = {
      EpBundleArtifact{.id = "first",
                       .url = "https://example.test/first.bin",
                       .is_archive = false,
                       .archive_sha256 = "",
                       .extracted_files = {},
                       .ignored_archive_paths = {},
                       .archive_max_bytes = 0,
                       .raw_relative_path = "first.bin",
                       .raw_sha256 = HashOf(first),
                       .raw_max_bytes = 1024},
      EpBundleArtifact{.id = "second",
                       .url = "https://example.test/second.bin",
                       .is_archive = false,
                       .archive_sha256 = "",
                       .extracted_files = {},
                       .ignored_archive_paths = {},
                       .archive_max_bytes = 0,
                       .raw_relative_path = "second.bin",
                       .raw_sha256 = HashOf(second),
                       .raw_max_bytes = 1024},
  };

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;
  ASSERT_TRUE(InstallAndFinalize(installer, manifest, logger).has_value());
  const auto active_generation = ReadFile(root.path() / "active");
  const auto active_bin = root.path() / "bundles" / active_generation / "bin";
  std::ofstream(active_bin / "second.bin", std::ios::binary | std::ios::trunc) << "corrupt";

  auto cancel_after_copy = [](const std::string&, float percent) { return percent < 40.0f; };

  EXPECT_EQ(installer.EnsureInstalled(manifest, cancel_after_copy, logger), nullptr);
  EXPECT_TRUE(std::filesystem::is_empty(root.path() / "staging"));
  EXPECT_EQ(downloads.CallCount("https://example.test/first.bin"), 1);
  EXPECT_EQ(downloads.CallCount("https://example.test/second.bin"), 1);
}

}  // namespace fl
