// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/blob_download_state.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace fl;

namespace {

using fl::test::TempPath;

constexpr int64_t kBlobSize = 20 * 1024 * 1024;  // 20 MiB
constexpr int32_t kChunkSize = 2 * 1024 * 1024;  // 2 MiB
constexpr int32_t kNumChunks = 10;

}  // namespace

TEST(BlobDownloadStateTest, GetStateFilePathAppendsDlstate) {
  fs::path p = "C:/some/file.bin";
  EXPECT_EQ(BlobDownloadState::GetStateFilePath(p).string(),
            (fs::path("C:/some/file.bin.dlstate")).string());
}

TEST(BlobDownloadStateTest, CreateNewInitializesEmptyBitmap) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->blob_size, kBlobSize);
  EXPECT_EQ(s->chunk_size, kChunkSize);
  EXPECT_EQ(s->total_chunks, kNumChunks);
  EXPECT_EQ(s->completed_count, 0);
  EXPECT_EQ(s->highest_completed_chunk, -1);
  EXPECT_EQ(s->bitmap_byte_aligned_start, 0);
  EXPECT_FALSE(s->IsComplete());
  EXPECT_EQ(s->CalculateDownloadedSize(), 0);
  EXPECT_EQ(s->GetPendingChunks().size(), static_cast<size_t>(kNumChunks));
}

TEST(BlobDownloadStateTest, MarkChunkCompleteUpdatesBitmapAndCounter) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
  s->MarkChunkComplete(3);
  EXPECT_TRUE(s->IsChunkComplete(3));
  EXPECT_FALSE(s->IsChunkComplete(2));
  EXPECT_EQ(s->completed_count, 1);
  EXPECT_EQ(s->highest_completed_chunk, 3);
  EXPECT_EQ(s->CalculateDownloadedSize(), kChunkSize);
}

TEST(BlobDownloadStateTest, MarkChunkCompleteIsIdempotent) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
  s->MarkChunkComplete(5);
  s->MarkChunkComplete(5);
  s->MarkChunkComplete(5);
  EXPECT_EQ(s->completed_count, 1);
}

TEST(BlobDownloadStateTest, CalculateDownloadedSizeAccountsForPartialFinalChunk) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  constexpr int64_t kOddBlobSize = 4 * 1024 * 1024 + 17;  // 3 chunks of 2 MiB; last chunk is a partial 17 bytes
  constexpr int32_t kOddNumChunks = 3;
  auto s = BlobDownloadState::CreateNew("blob", local, kOddBlobSize, kChunkSize, kOddNumChunks);
  for (int32_t i = 0; i < kOddNumChunks; ++i) {
    s->MarkChunkComplete(i);
  }
  EXPECT_TRUE(s->IsComplete());
  EXPECT_EQ(s->CalculateDownloadedSize(), kOddBlobSize);
}

TEST(BlobDownloadStateTest, GetPendingChunksReturnsGaps) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
  for (int32_t i : {0, 1, 2, 5, 7}) {
    s->MarkChunkComplete(i);
  }
  auto pending = s->GetPendingChunks();
  std::vector<int32_t> expected{3, 4, 6, 8, 9};
  EXPECT_EQ(pending, expected);
}

TEST(BlobDownloadStateTest, SaveAndLoadRoundTrip) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  {
    auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
    for (int32_t i : {0, 2, 4, 6, 8}) {
      s->MarkChunkComplete(i);
    }
    s->SaveState(fl::test::NullLog());
  }
  auto loaded = BlobDownloadState::LoadState("blob", local, kBlobSize, kChunkSize, kNumChunks,
                                             fl::test::NullLog());
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->completed_count, 5);
  EXPECT_EQ(loaded->highest_completed_chunk, 8);
  for (int32_t i : {0, 2, 4, 6, 8}) {
    EXPECT_TRUE(loaded->IsChunkComplete(i)) << "chunk " << i;
  }
  for (int32_t i : {1, 3, 5, 7, 9}) {
    EXPECT_FALSE(loaded->IsChunkComplete(i)) << "chunk " << i;
  }
  std::vector<int32_t> expected{1, 3, 5, 7, 9};
  EXPECT_EQ(loaded->GetPendingChunks(), expected);
}

TEST(BlobDownloadStateTest, SaveStateAdvancesBitmapByteAlignedStart) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  // Use a large enough total that whole-word advance is meaningful.
  constexpr int32_t kBigNumChunks = 200;
  constexpr int64_t kBigBlobSize = static_cast<int64_t>(kBigNumChunks) * kChunkSize;
  auto s = BlobDownloadState::CreateNew("blob", local, kBigBlobSize, kChunkSize, kBigNumChunks);
  // Complete the first 80 chunks (10 full bytes worth).
  for (int32_t i = 0; i < 80; ++i) {
    s->MarkChunkComplete(i);
  }
  s->SaveState(fl::test::NullLog());
  // 64 bits = 1 full word; next 16 bits in word 1. Aligned start lands on
  // 80 (multiple of 8).
  EXPECT_EQ(s->bitmap_byte_aligned_start, 80);

  // Reload and verify the implicit prefix is still considered complete.
  auto loaded = BlobDownloadState::LoadState("blob", local, kBigBlobSize, kChunkSize, kBigNumChunks,
                                             fl::test::NullLog());
  ASSERT_NE(loaded, nullptr);
  for (int32_t i = 0; i < 80; ++i) {
    EXPECT_TRUE(loaded->IsChunkComplete(i));
  }
  for (int32_t i = 80; i < kBigNumChunks; ++i) {
    EXPECT_FALSE(loaded->IsChunkComplete(i));
  }
  EXPECT_EQ(loaded->completed_count, 80);
}

// Regression: a second SaveState whose contiguous-complete prefix crosses a
// 64-bit word boundary from a non-word-aligned start must not advance
// bitmap_byte_aligned_start past the first still-pending chunk. The advance
// previously accumulated +64 per word onto the unaligned base and overshot by
// (start % 64), silently marking never-downloaded chunks complete on reload.
TEST(BlobDownloadStateTest, SaveStateFromUnalignedStartDoesNotMarkPendingComplete) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  constexpr int32_t kBigNumChunks = 200;
  constexpr int64_t kBigBlobSize = static_cast<int64_t>(kBigNumChunks) * kChunkSize;
  auto s = BlobDownloadState::CreateNew("blob", local, kBigBlobSize, kChunkSize, kBigNumChunks);

  // First save lands the contiguous prefix on a byte (8) but not a word (64)
  // boundary.
  for (int32_t i = 0; i < 8; ++i) {
    s->MarkChunkComplete(i);
  }
  s->SaveState(fl::test::NullLog());
  EXPECT_EQ(s->bitmap_byte_aligned_start, 8);

  // Extend the contiguous prefix across the word boundary: chunks 0..64 done,
  // chunk 65 is the first still-pending chunk.
  for (int32_t i = 8; i <= 64; ++i) {
    s->MarkChunkComplete(i);
  }
  s->SaveState(fl::test::NullLog());
  // Must round down to 64 (the byte boundary at/below the first pending chunk),
  // never overshoot to 72.
  EXPECT_EQ(s->bitmap_byte_aligned_start, 64);

  // Reload and prove chunks 65..71 (never downloaded) are still pending.
  auto loaded = BlobDownloadState::LoadState("blob", local, kBigBlobSize, kChunkSize, kBigNumChunks,
                                             fl::test::NullLog());
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsChunkComplete(64));
  for (int32_t i = 65; i < 72; ++i) {
    EXPECT_FALSE(loaded->IsChunkComplete(i)) << "chunk " << i << " was never downloaded";
  }
  auto pending = loaded->GetPendingChunks();
  ASSERT_FALSE(pending.empty());
  EXPECT_EQ(pending.front(), 65);
}

TEST(BlobDownloadStateTest, LoadStateReturnsNullWhenFileMissing) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  auto s = BlobDownloadState::LoadState("blob", local, kBlobSize, kChunkSize, kNumChunks, fl::test::NullLog());
  EXPECT_EQ(s, nullptr);
}

TEST(BlobDownloadStateTest, LoadStateRejectsBadMagic) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  auto sidecar = BlobDownloadState::GetStateFilePath(local);
  {
    std::ofstream f(sidecar, std::ios::binary);
    f << "ZZZZ";  // wrong magic
    f.put(static_cast<char>(0));  // version
    for (int i = 0; i < 64; ++i) f.put(0);  // padding
  }
  auto s = BlobDownloadState::LoadState("blob", local, kBlobSize, kChunkSize, kNumChunks, fl::test::NullLog());
  EXPECT_EQ(s, nullptr);
}

TEST(BlobDownloadStateTest, LoadStateRejectsBlobSizeMismatch) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  {
    auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
    s->MarkChunkComplete(0);
    s->SaveState(fl::test::NullLog());
  }
  // Reload with a *different* expected blob_size — should be rejected.
  auto s = BlobDownloadState::LoadState("blob", local, kBlobSize + 1, kChunkSize, kNumChunks,
                                        fl::test::NullLog());
  EXPECT_EQ(s, nullptr);
}

TEST(BlobDownloadStateTest, LoadStateRejectsChunkSizeMismatch) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  {
    auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
    s->MarkChunkComplete(0);
    s->SaveState(fl::test::NullLog());
  }
  auto s = BlobDownloadState::LoadState("blob", local, kBlobSize, kChunkSize + 1, kNumChunks,
                                        fl::test::NullLog());
  EXPECT_EQ(s, nullptr);
}

TEST(BlobDownloadStateTest, LoadStateRejectsTotalChunksMismatch) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  {
    auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
    s->MarkChunkComplete(0);
    s->SaveState(fl::test::NullLog());
  }
  auto s = BlobDownloadState::LoadState("blob", local, kBlobSize, kChunkSize, kNumChunks + 1,
                                        fl::test::NullLog());
  EXPECT_EQ(s, nullptr);
}

TEST(BlobDownloadStateTest, DeleteStateRemovesSidecar) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  {
    auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
    s->MarkChunkComplete(0);
    s->SaveState(fl::test::NullLog());
  }
  EXPECT_TRUE(fs::exists(BlobDownloadState::GetStateFilePath(local)));
  BlobDownloadState::DeleteState(local, fl::test::NullLog());
  EXPECT_FALSE(fs::exists(BlobDownloadState::GetStateFilePath(local)));
  // Re-deletion when the file is already absent is a no-op (best-effort).
  BlobDownloadState::DeleteState(local, fl::test::NullLog());
}

TEST(BlobDownloadStateTest, IsCompleteFlipsTrueWhenAllChunksMarked) {
  auto d = TempPath::CreateTempDir();
  auto local = d.path() / "blob.bin";
  auto s = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
  for (int32_t i = 0; i < kNumChunks; ++i) {
    EXPECT_FALSE(s->IsComplete());
    s->MarkChunkComplete(i);
  }
  EXPECT_TRUE(s->IsComplete());
  EXPECT_EQ(s->GetPendingChunks().size(), 0u);
}
