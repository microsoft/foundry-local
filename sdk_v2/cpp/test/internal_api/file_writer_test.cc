// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for the FileWriter backing AzureBlobDownloader's chunked writes:
// pre-allocation, resume preservation, and single-thread + concurrent
// disjoint-range positional writes.

#include "download/file_writer.h"
#include "test_helpers.h"
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace fl;

namespace {

class TempPath {
 public:
  TempPath() {
    path_ = fl::test::MakeUniqueTempPath("file_writer_test_");
    path_ += ".bin";
  }
  ~TempPath() {
    std::error_code ec;
    fs::remove(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

}  // namespace

TEST(FileWriterTest, OpenCreatesFileAtRequestedSize) {
  TempPath p;
  FileWriter w(fl::test::NullLog());
  w.Open(p.path(), 4096);
  w.Close();
  EXPECT_TRUE(fs::exists(p.path()));
  EXPECT_EQ(fs::file_size(p.path()), 4096u);
}

TEST(FileWriterTest, OpenPreservesExistingFileAtSameSize) {
  TempPath p;
  // Pre-write a sentinel byte the writer must NOT overwrite.
  {
    std::ofstream f(p.path(), std::ios::binary);
    f.seekp(1023);
    f.put('\0');
  }
  // Plant a known byte at offset 100.
  {
    std::fstream f(p.path(), std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(100);
    f.put(static_cast<char>(0xAB));
  }

  FileWriter w(fl::test::NullLog());
  w.Open(p.path(), 1024);  // same size -> must not truncate
  w.Close();

  // Sentinel byte should still be there.
  std::ifstream f(p.path(), std::ios::binary);
  f.seekg(100);
  int byte = f.get();
  EXPECT_EQ(byte, 0xAB);
}

TEST(FileWriterTest, OpenRecreatesFileWhenSizeDiffers) {
  TempPath p;
  {
    std::ofstream f(p.path(), std::ios::binary);
    f.seekp(100);
    f.put(static_cast<char>(0xCD));
  }
  EXPECT_EQ(fs::file_size(p.path()), 101u);

  FileWriter w(fl::test::NullLog());
  w.Open(p.path(), 4096);
  w.Close();
  EXPECT_EQ(fs::file_size(p.path()), 4096u);
}

TEST(FileWriterTest, SingleThreadWriteAt) {
  TempPath p;
  FileWriter w(fl::test::NullLog());
  w.Open(p.path(), 1024);

  std::vector<uint8_t> data(256, 0xEF);
  w.WriteAt(512, data.data(), data.size());
  w.Close();

  std::ifstream f(p.path(), std::ios::binary);
  std::vector<uint8_t> contents((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
  ASSERT_EQ(contents.size(), 1024u);
  for (size_t i = 512; i < 768; ++i) {
    EXPECT_EQ(contents[i], 0xEF) << "byte " << i;
  }
}

TEST(FileWriterTest, ConcurrentDisjointWritesProduceCorrectFile) {
  TempPath p;
  constexpr int kThreads = 8;
  constexpr int kRegionSize = 256 * 1024;  // 256 KB per thread
  constexpr int kPieceSize = 16 * 1024;    // 16 KB per WriteAt
  constexpr int64_t kTotalSize = int64_t{kThreads} * kRegionSize;
  static_assert(kRegionSize % kPieceSize == 0, "");

  FileWriter w(fl::test::NullLog());
  w.Open(p.path(), kTotalSize);

  std::atomic<int> started{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t]() {
      std::vector<uint8_t> piece(kPieceSize, static_cast<uint8_t>(t + 1));
      started.fetch_add(1);
      while (started.load() < kThreads) {
        // tiny spin to encourage concurrent dispatch
      }
      const int64_t base = int64_t{t} * kRegionSize;
      for (int i = 0; i < kRegionSize / kPieceSize; ++i) {
        w.WriteAt(base + int64_t{i} * kPieceSize, piece.data(), piece.size());
      }
    });
  }
  for (auto& th : workers) th.join();
  w.Close();

  std::ifstream f(p.path(), std::ios::binary);
  std::vector<uint8_t> contents((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
  ASSERT_EQ(contents.size(), static_cast<size_t>(kTotalSize));
  for (int t = 0; t < kThreads; ++t) {
    const uint8_t expected = static_cast<uint8_t>(t + 1);
    for (int64_t i = 0; i < kRegionSize; ++i) {
      const auto idx = static_cast<size_t>(int64_t{t} * kRegionSize + i);
      if (contents[idx] != expected) {
        FAIL() << "mismatch at offset " << idx << " (thread " << t << ", expected "
               << static_cast<int>(expected) << ", got " << static_cast<int>(contents[idx]) << ")";
      }
    }
  }
}
