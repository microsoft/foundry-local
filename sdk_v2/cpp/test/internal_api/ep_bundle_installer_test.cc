// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_bundle_installer.h"

#include "logger.h"
#include "util/file_lock.h"
#include "util/sha256.h"

#include "utils/temp_path.h"
#include "utils/zip_builder.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fl {

namespace {

class NullLogger : public ILogger {
 public:
  void Log(LogLevel /*level*/, std::string_view /*message*/) override {}
};

std::vector<uint8_t> AsBytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

std::string HashOf(const std::vector<uint8_t>& bytes) {
  auto tmp = test::TempPath::CreateTempFile("fl_bundle_installer_hash_");
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

  int CallCount(const std::string& url) const {
    auto it = call_counts_.find(url);
    return it == call_counts_.end() ? 0 : it->second;
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
      size_t index = std::min(static_cast<size_t>(count), it->second.size() - 1);
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

EpBundleManifest MakeRawManifest(const std::string& bundle_id, const std::string& url,
                                 const std::string& sha256) {
  EpBundleManifest manifest;
  manifest.bundle_id = bundle_id;
  manifest.provider_relative_path = "provider.so";
  manifest.artifacts = {EpBundleArtifact{.id = "provider",
                                         .url = url,
                                         .is_archive = false,
                                         .archive_sha256 = "",
                                         .extracted_files = {},
                                         .archive_max_bytes = 0,
                                         .raw_relative_path = "provider.so",
                                         .raw_sha256 = sha256,
                                         .raw_max_bytes = 1024}};
  return manifest;
}

EpBundleManifest MakeArchiveManifest(const std::string& bundle_id, const std::string& url,
                                     const std::string& archive_sha256,
                                     std::vector<EpBundleFile> extracted_files) {
  EpBundleManifest manifest;
  manifest.bundle_id = bundle_id;
  manifest.provider_relative_path = "provider.dll";
  manifest.artifacts = {EpBundleArtifact{.id = "archive",
                                         .url = url,
                                         .is_archive = true,
                                         .archive_sha256 = archive_sha256,
                                         .extracted_files = std::move(extracted_files),
                                         .archive_max_bytes = 1024 * 1024,
                                         .raw_relative_path = "",
                                         .raw_sha256 = "",
                                         .raw_max_bytes = 0}};
  return manifest;
}

std::optional<std::filesystem::path> InstallAndCommit(EpBundleInstaller& installer,
                                                      const EpBundleManifest& manifest, ILogger& logger) {
  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  if (!txn) {
    return std::nullopt;
  }

  const auto bin_dir = txn->bin_dir();
  if (!txn->CommitActive(logger)) {
    return std::nullopt;
  }

  return bin_dir;
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
    ASSERT_TRUE(first->CommitActive(logger));
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
                       .archive_max_bytes = 0,
                       .raw_relative_path = "first.bin",
                       .raw_sha256 = HashOf(first),
                       .raw_max_bytes = 1024},
      EpBundleArtifact{.id = "second",
                       .url = "https://example.test/second.bin",
                       .is_archive = false,
                       .archive_sha256 = "",
                       .extracted_files = {},
                       .archive_max_bytes = 0,
                       .raw_relative_path = "second.bin",
                       .raw_sha256 = HashOf(second),
                       .raw_max_bytes = 1024},
  };

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;
  ASSERT_TRUE(InstallAndCommit(installer, manifest, logger).has_value());

  auto replacement = installer.EnsureInstalled(
      manifest, /*progress_cb=*/nullptr, logger, EpBundleInstallPolicy::ForceDownload);

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

  auto manifest = MakeArchiveManifest(
      "bundle-1", "https://example.test/archive.zip", HashOf(good_archive),
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
      MakeArchiveManifest(
          "bundle-1", "https://example.test/archive.zip", HashOf(AsBytes("expected-archive")),
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

  auto manifest = MakeArchiveManifest(
      "bundle-1", "https://example.test/archive.zip", HashOf(archive),
      {EpBundleFile{.relative_path = "provider.dll", .sha256 = HashOf(AsBytes("actual-content"))},
       EpBundleFile{.relative_path = "missing.dll", .sha256 = HashOf(AsBytes("whatever"))}});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  EXPECT_EQ(txn, nullptr);
}

TEST(EpBundleInstallerTest, CommitActiveWritesActiveMarkerFile) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});

  auto manifest = MakeRawManifest("bundle-42", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto txn = installer.EnsureInstalled(manifest, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  EXPECT_TRUE(txn->CommitActive(logger));

  EXPECT_TRUE(ReadFile(root.path() / "active").starts_with("bundle-42-"));
}

TEST(EpBundleInstallerTest, CommitActiveReplacesExistingActiveMarker) {
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
  EXPECT_TRUE(txn->CommitActive(logger));
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

TEST(EpBundleInstallerTest, CommitActiveRevalidatesUnderLockAndRefusesTamperedBundle) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndCommit(installer, manifest_v1, logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");
  ASSERT_TRUE(active_v1.starts_with("bundle-v1-"));

  auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/v2.so", HashOf(payload_v2));
  auto txn = installer.EnsureInstalled(manifest_v2, /*progress_cb=*/nullptr, logger);
  ASSERT_NE(txn, nullptr);
  {
    std::ofstream(txn->bin_dir() / "unexpected.txt") << "surprise";
  }

  EXPECT_FALSE(txn->CommitActive(logger)) << "re-verification under the lock must reject a tampered bundle";
  EXPECT_EQ(ReadFile(root.path() / "active"), active_v1)
      << "a bundle failing re-verification must not advance the active marker";
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1))
      << "the previous generation is preserved when activation fails";
}

TEST(EpBundleInstallerTest, CommitActivePreservesPreviousMarkerWhenPublicationFails) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndCommit(installer, manifest_v1, logger).has_value());
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

  EXPECT_FALSE(txn->CommitActive(logger)) << "a failed marker publication is reported as failure";
  EXPECT_TRUE(std::filesystem::is_directory(root.path() / "active"))
      << "the failed publication left the marker path untouched";
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1))
      << "the previous generation is preserved when publication fails";
  EXPECT_TRUE(std::filesystem::exists(txn->bin_dir()));
}

TEST(EpBundleInstallerTest, CommitActiveRemovesOldGenerations) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload_v1 = AsBytes("v1");
  auto payload_v2 = AsBytes("v2");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/v1.so", {payload_v1});
  downloads.SetSequence("https://example.test/v2.so", {payload_v2});

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  auto manifest_v1 = MakeRawManifest("bundle-v1", "https://example.test/v1.so", HashOf(payload_v1));
  ASSERT_TRUE(InstallAndCommit(installer, manifest_v1, logger).has_value());
  const auto active_v1 = ReadFile(root.path() / "active");
  ASSERT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v1));

  auto manifest_v2 = MakeRawManifest("bundle-v2", "https://example.test/v2.so", HashOf(payload_v2));
  ASSERT_TRUE(InstallAndCommit(installer, manifest_v2, logger).has_value());
  const auto active_v2 = ReadFile(root.path() / "active");

  EXPECT_FALSE(std::filesystem::exists(root.path() / "bundles" / active_v1))
      << "the previous generation is removed once the new one is committed";
  EXPECT_TRUE(std::filesystem::exists(root.path() / "bundles" / active_v2));
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

TEST(EpBundleInstallerTest, DoesNotCopyUnexpectedFilesFromExistingBundle) {
  auto root = test::TempPath::CreateTempDir("fl_bundle_installer_");
  auto payload = AsBytes("content");
  FakeDownloads downloads;
  downloads.SetSequence("https://example.test/provider.so", {payload});
  auto manifest = MakeRawManifest("bundle-1", "https://example.test/provider.so", HashOf(payload));
  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;

  ASSERT_TRUE(InstallAndCommit(installer, manifest, logger).has_value());

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

  auto manifest = MakeArchiveManifest(
      "bundle-1", "https://example.test/archive.zip", HashOf(archive),
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
                       .archive_max_bytes = 0,
                       .raw_relative_path = "first.bin",
                       .raw_sha256 = HashOf(first),
                       .raw_max_bytes = 1024},
      EpBundleArtifact{.id = "second",
                       .url = "https://example.test/second.bin",
                       .is_archive = false,
                       .archive_sha256 = "",
                       .extracted_files = {},
                       .archive_max_bytes = 0,
                       .raw_relative_path = "second.bin",
                       .raw_sha256 = HashOf(second),
                       .raw_max_bytes = 1024},
  };

  EpBundleInstaller installer(root.path(), "test.lock", "TestEP", downloads.AsFn());
  NullLogger logger;
  ASSERT_TRUE(InstallAndCommit(installer, manifest, logger).has_value());

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

}  // namespace fl
