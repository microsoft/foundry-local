// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Unit tests for util/zip_extract: zip-slip pre-validation plus the bounded in-process extractor
// (STORE + DEFLATE, symlink/special/duplicate rejection, and resource limits). All archives are
// built in-process — no dependency on an external zip/tar tool.
#include "util/zip_extract.h"

#include "logger.h"

#include "utils/temp_path.h"
#include "utils/zip_builder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace fl {

TEST(IsSafeArchiveEntryTest, AcceptsSimpleFilename) { EXPECT_TRUE(IsSafeArchiveEntry("file.bin")); }

TEST(IsSafeArchiveEntryTest, AcceptsNestedRelativePath) { EXPECT_TRUE(IsSafeArchiveEntry("dir/sub/file.bin")); }

TEST(IsSafeArchiveEntryTest, AcceptsBackslashRelativePath) { EXPECT_TRUE(IsSafeArchiveEntry("dir\\sub\\file.bin")); }

TEST(IsSafeArchiveEntryTest, AcceptsEmpty) { EXPECT_TRUE(IsSafeArchiveEntry("")); }

TEST(IsSafeArchiveEntryTest, AcceptsDotComponent) {
  // A single "." component is benign and commonly emitted by tar.
  EXPECT_TRUE(IsSafeArchiveEntry("./file.bin"));
}

TEST(IsSafeArchiveEntryTest, RejectsLeadingParent) { EXPECT_FALSE(IsSafeArchiveEntry("../escape.txt")); }

TEST(IsSafeArchiveEntryTest, RejectsLeadingParentBackslash) { EXPECT_FALSE(IsSafeArchiveEntry("..\\escape.txt")); }

TEST(IsSafeArchiveEntryTest, RejectsMidPathParent) { EXPECT_FALSE(IsSafeArchiveEntry("dir/../escape.txt")); }

TEST(IsSafeArchiveEntryTest, RejectsTrailingParent) { EXPECT_FALSE(IsSafeArchiveEntry("dir/..")); }

TEST(IsSafeArchiveEntryTest, RejectsDeepParentChain) { EXPECT_FALSE(IsSafeArchiveEntry("a/b/../../../etc/passwd")); }

TEST(IsSafeArchiveEntryTest, RejectsAbsolutePosixPath) { EXPECT_FALSE(IsSafeArchiveEntry("/etc/passwd")); }

TEST(IsSafeArchiveEntryTest, RejectsLeadingBackslash) { EXPECT_FALSE(IsSafeArchiveEntry("\\Windows\\System32")); }

TEST(IsSafeArchiveEntryTest, RejectsWindowsDriveLetter) { EXPECT_FALSE(IsSafeArchiveEntry("C:\\Windows\\System32")); }

TEST(IsSafeArchiveEntryTest, RejectsLowerCaseDriveLetter) { EXPECT_FALSE(IsSafeArchiveEntry("c:/Windows")); }

TEST(IsSafeArchiveEntryTest, RejectsAnyColon) {
  // Defensive: archive entries should never legitimately contain ':'.
  EXPECT_FALSE(IsSafeArchiveEntry("dir/foo:bar"));
}

TEST(IsSafeArchiveEntryTest, AcceptsParentLikeFilename) {
  // ".." must be rejected as a component, but "..foo" is just a filename.
  EXPECT_TRUE(IsSafeArchiveEntry("..foo"));
  EXPECT_TRUE(IsSafeArchiveEntry("foo.."));
  EXPECT_TRUE(IsSafeArchiveEntry("dir/...hidden"));
}

// ========================================================================
// ExtractZip — bounded in-process extractor
// ========================================================================

namespace {

class NullLogger : public ILogger {
 public:
  void Log(LogLevel /*level*/, std::string_view /*message*/) override {}
};

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> AsBytes(const std::string& text) { return std::vector<uint8_t>(text.begin(), text.end()); }

}  // namespace

using test::ZipBuilder;

TEST(ExtractZipTest, ExtractsStoredEntry) {
  ZipBuilder builder;
  builder.AddEntry("hello.txt", AsBytes("hello world"));
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  ASSERT_TRUE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_EQ(ReadFileBytes(dest.path() / "hello.txt"), AsBytes("hello world"));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, ExtractsDeflatedEntry) {
  std::string content(4096, 'A');  // large + repetitive so DEFLATE actually compresses it
  ZipBuilder builder;
  builder.AddEntry("big.bin", AsBytes(content), /*compress=*/true);
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  ASSERT_TRUE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_EQ(ReadFileBytes(dest.path() / "big.bin"), AsBytes(content));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, ExtractsNestedDirectoriesAndMultipleEntries) {
  ZipBuilder builder;
  builder.AddDirectory("sub/");
  builder.AddEntry("sub/a.txt", AsBytes("a"));
  builder.AddEntry("b.txt", AsBytes("b"), /*compress=*/true);
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  ASSERT_TRUE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_EQ(ReadFileBytes(dest.path() / "sub" / "a.txt"), AsBytes("a"));
  EXPECT_EQ(ReadFileBytes(dest.path() / "b.txt"), AsBytes("b"));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsDestinationRootSymlink) {
  ZipBuilder builder;
  builder.AddEntry("hello.txt", AsBytes("hello"));
  const auto zip_path = builder.WriteToTempFile();
  auto root = test::TempPath::CreateTempDir("fl_zip_extract_root_");
  auto outside = test::TempPath::CreateTempDir("fl_zip_extract_outside_");
  const auto destination = root.path() / "destination";
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside.path(), destination, ec);
  if (ec) {
    std::filesystem::remove(zip_path);
    GTEST_SKIP() << "Directory symlinks are unavailable: " << ec.message();
  }
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, destination, logger));
  EXPECT_TRUE(std::filesystem::is_empty(outside.path()));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsDestinationUnderSymlinkedDirectoryComponent) {
  ZipBuilder builder;
  builder.AddEntry("hello.txt", AsBytes("hello"));
  const auto zip_path = builder.WriteToTempFile();
  auto root = test::TempPath::CreateTempDir("fl_zip_extract_root_");
  auto outside = test::TempPath::CreateTempDir("fl_zip_extract_outside_");
  std::filesystem::create_directory(outside.path() / "destination");
  const auto component = root.path() / "component";
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside.path(), component, ec);
  if (ec) {
    std::filesystem::remove(zip_path);
    GTEST_SKIP() << "Directory symlinks are unavailable: " << ec.message();
  }
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, component / "destination", logger));
  EXPECT_TRUE(std::filesystem::is_empty(outside.path() / "destination"));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsRegularFileDestination) {
  ZipBuilder builder;
  builder.AddEntry("hello.txt", AsBytes("hello"));
  const auto zip_path = builder.WriteToTempFile();
  auto destination = test::TempPath::CreateTempFile("fl_zip_extract_dest_");
  std::ofstream(destination.path()) << "not a directory";
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, destination.path(), logger));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsNonEmptyDestination) {
  ZipBuilder builder;
  builder.AddEntry("hello.txt", AsBytes("hello"));
  const auto zip_path = builder.WriteToTempFile();
  auto destination = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  std::ofstream(destination.path() / "existing.txt") << "existing";
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, destination.path(), logger));
  EXPECT_EQ(ReadFileBytes(destination.path() / "existing.txt"), AsBytes("existing"));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsPreExistingOutputLeafSymlink) {
  ZipBuilder builder;
  builder.AddEntry("hello.txt", AsBytes("replacement"));
  const auto zip_path = builder.WriteToTempFile();
  auto destination = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  auto outside = test::TempPath::CreateTempFile("fl_zip_extract_outside_");
  std::ofstream(outside.path(), std::ios::binary | std::ios::trunc) << "original";
  std::error_code ec;
  std::filesystem::create_symlink(outside.path(), destination.path() / "hello.txt", ec);
  if (ec) {
    std::filesystem::remove(zip_path);
    GTEST_SKIP() << "File symlinks are unavailable: " << ec.message();
  }
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, destination.path(), logger));
  EXPECT_EQ(ReadFileBytes(outside.path()), AsBytes("original"));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsPathTraversalEntry) {
  ZipBuilder builder;
  builder.AddEntry("../escape.txt", AsBytes("evil"));
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_FALSE(std::filesystem::exists(dest.path() / ".." / "escape.txt"));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsSymlinkEntry) {
  constexpr uint32_t kSymlinkMode = 0xA1FF;  // S_IFLNK
  ZipBuilder builder;
  builder.AddEntry("link", AsBytes("/etc/passwd"), /*compress=*/false, kSymlinkMode);
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsSpecialFileEntry) {
  constexpr uint32_t kCharDeviceMode = 0x21FF;  // S_IFCHR
  ZipBuilder builder;
  builder.AddEntry("dev-null", AsBytes(""), /*compress=*/false, kCharDeviceMode);
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsDuplicateEntries) {
  ZipBuilder builder;
  builder.AddEntry("dup.txt", AsBytes("first"));
  builder.AddEntry("dup.txt", AsBytes("second"));
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsUnsupportedCompressionBeforeWritingAnyEntries) {
  ZipBuilder builder;
  builder.AddEntry("good.txt", AsBytes("good"));
  builder.AddEntryWithCompressionMethod("unsupported.txt", AsBytes("unsupported"), 12);
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_TRUE(std::filesystem::is_empty(dest.path()));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsCaseInsensitivePathAliasesBeforeWriting) {
  ZipBuilder builder;
  builder.AddEntry("FILE.txt", AsBytes("first"));
  builder.AddEntry("file.txt", AsBytes("second"));
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_TRUE(std::filesystem::is_empty(dest.path()));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsWin32TrimmedAndReservedPathComponentsBeforeWriting) {
  for (const auto* unsafe_name : {"file.", "file ", "CON", "nul.txt", "dir/COM1.bin", "dir/LPT9"}) {
    ZipBuilder builder;
    builder.AddEntry("good.txt", AsBytes("good"));
    builder.AddEntry(unsafe_name, AsBytes("unsafe"));
    auto zip_path = builder.WriteToTempFile();
    auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
    NullLogger logger;

    EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger)) << unsafe_name;
    EXPECT_TRUE(std::filesystem::is_empty(dest.path())) << unsafe_name;
    std::filesystem::remove(zip_path);
  }
}

TEST(ExtractZipTest, RejectsEntryCountOverLimit) {
  ZipBuilder builder;
  builder.AddEntry("a.txt", AsBytes("a"));
  builder.AddEntry("b.txt", AsBytes("b"));
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  ZipExtractLimits limits;
  limits.max_entries = 1;
  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger, limits));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsEntryExceedingPerEntrySizeLimit) {
  ZipBuilder builder;
  builder.AddEntry("big.bin", AsBytes(std::string(1024, 'z')));
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  ZipExtractLimits limits;
  limits.max_entry_uncompressed_bytes = 100;
  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger, limits));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsTotalSizeOverLimit) {
  ZipBuilder builder;
  builder.AddEntry("a.bin", AsBytes(std::string(600, 'a')));
  builder.AddEntry("b.bin", AsBytes(std::string(600, 'b')));
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  ZipExtractLimits limits;
  limits.max_total_uncompressed_bytes = 1000;
  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger, limits));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, FailsOnMissingArchive) {
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;
  EXPECT_FALSE(ExtractZip("/nonexistent/path/does-not-exist.zip", dest.path(), logger));
}

TEST(ExtractZipTest, FailsOnTruncatedArchive) {
  ZipBuilder builder;
  builder.AddEntry("a.txt", AsBytes("a"));
  auto full_bytes = builder.Build();
  auto path = test::MakeUniqueTempPath("fl_zip_extract_truncated_");
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(full_bytes.data()), static_cast<std::streamsize>(full_bytes.size() / 2));
  }
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(path, dest.path(), logger));
  std::filesystem::remove(path);
}

namespace {

// Patches a little-endian uint32 at `offset` within `bytes` in place — used to craft a
// malformed EOCD record from an otherwise well-formed archive built by ZipBuilder.
void PatchU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  bytes[offset] = static_cast<uint8_t>(value & 0xFF);
  bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

// EOCD (End Of Central Directory) is the trailing 22 bytes for an archive with no comment;
// cd_size lives at byte 12 of the record and cd_offset at byte 16 — see zip_extract.cc.
constexpr size_t kEocdRecordSize = 22;
constexpr size_t kEocdCdSizeOffset = 12;
constexpr size_t kEocdCdOffsetOffset = 16;

std::filesystem::path WriteBytesToTempFile(const std::vector<uint8_t>& bytes, std::string_view prefix) {
  auto path = test::MakeUniqueTempPath(std::string(prefix));
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return path;
}

// Central-directory record field offsets relative to a record's start (see zip_extract.cc):
// compressed_size at byte 20, uncompressed_size at byte 24.
constexpr size_t kCdRecordCompressedSizeOffset = 20;
constexpr size_t kCdRecordUncompressedSizeOffset = 24;

uint32_t ReadU32LE(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

}  // namespace

TEST(ExtractZipTest, RejectsCentralDirectorySizeExceedingFileWithoutAllocatingOrWriting) {
  // A tiny, otherwise well-formed archive whose EOCD claims a central directory size far larger
  // than the whole file — the attacker-controlled `cd_size` field must be validated against the
  // real file size before it is ever used to size a std::vector allocation.
  ZipBuilder builder;
  builder.AddEntry("hello.txt", AsBytes("hello world"));
  auto bytes = builder.Build();

  PatchU32(bytes, bytes.size() - kEocdRecordSize + kEocdCdSizeOffset, 0x7FFFFFFFu);
  auto zip_path = WriteBytesToTempFile(bytes, "fl_zip_extract_huge_cdsize_");
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_FALSE(std::filesystem::exists(dest.path() / "hello.txt"));
  // dest.path() itself was pre-created by TempPath::CreateTempDir; what matters is that
  // ExtractZip never wrote anything into it before rejecting the malformed cd_size.
  EXPECT_TRUE(std::filesystem::is_empty(dest.path()));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsCentralDirectoryOffsetOutsideFileWithoutAllocatingOrWriting) {
  // The EOCD claims a central directory offset beyond the end of the file — combined with a
  // plausible cd_size, `cd_offset + cd_size` would run past the archive entirely.
  ZipBuilder builder;
  builder.AddEntry("hello.txt", AsBytes("hello world"));
  auto bytes = builder.Build();

  PatchU32(bytes, bytes.size() - kEocdRecordSize + kEocdCdOffsetOffset, 0x10000000u);
  auto zip_path = WriteBytesToTempFile(bytes, "fl_zip_extract_bad_cdoffset_");
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_FALSE(std::filesystem::exists(dest.path() / "hello.txt"));
  EXPECT_TRUE(std::filesystem::is_empty(dest.path()));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, NoPartialExtractionWhenOneEntryIsUnsafe) {
  // The whole archive is validated before any bytes are written — a single unsafe entry must
  // not leave the earlier, otherwise-valid entries behind.
  ZipBuilder builder;
  builder.AddEntry("good.txt", AsBytes("good"));
  builder.AddEntry("../escape.txt", AsBytes("evil"));
  auto zip_path = builder.WriteToTempFile();
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_FALSE(std::filesystem::exists(dest.path() / "good.txt"));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsEntryCompressedSizeLargerThanArchiveWithoutAllocating) {
  // A DEFLATE entry whose central-directory compressed_size is rewritten to a huge value. The
  // streaming extractor must reject it on the up-front size bound and never attempt an allocation
  // or read sized by the attacker-controlled field.
  ZipBuilder builder;
  builder.AddEntry("big.bin", AsBytes(std::string(256, 'A')), /*compress=*/true);
  auto bytes = builder.Build();

  const uint32_t cd_offset = ReadU32LE(bytes, bytes.size() - kEocdRecordSize + kEocdCdOffsetOffset);
  PatchU32(bytes, cd_offset + kCdRecordCompressedSizeOffset, 0x7FFFFFFFu);

  auto zip_path = WriteBytesToTempFile(bytes, "fl_zip_extract_huge_compsize_");
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_TRUE(std::filesystem::is_empty(dest.path()));
  std::filesystem::remove(zip_path);
}

TEST(ExtractZipTest, RejectsStoredEntryWithCompressedUncompressedSizeMismatch) {
  // A STORE entry whose central-directory uncompressed_size is rewritten so it no longer equals the
  // compressed_size. A STORE entry is copied verbatim, so the two must match — the mismatch is
  // rejected during validation, before any extraction begins.
  ZipBuilder builder;
  builder.AddEntry("a.txt", AsBytes("hello"));  // STORE: compressed_size == uncompressed_size == 5
  auto bytes = builder.Build();

  const uint32_t cd_offset = ReadU32LE(bytes, bytes.size() - kEocdRecordSize + kEocdCdOffsetOffset);
  PatchU32(bytes, cd_offset + kCdRecordUncompressedSizeOffset, 4);  // now 5 (compressed) != 4 (uncompressed)

  auto zip_path = WriteBytesToTempFile(bytes, "fl_zip_extract_store_mismatch_");
  auto dest = test::TempPath::CreateTempDir("fl_zip_extract_dest_");
  NullLogger logger;

  EXPECT_FALSE(ExtractZip(zip_path, dest.path(), logger));
  EXPECT_TRUE(std::filesystem::is_empty(dest.path()));
  std::filesystem::remove(zip_path);
}

}  // namespace fl
