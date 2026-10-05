// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/sha256.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

std::filesystem::path WriteTempFile(const std::string& contents, const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  f.close();
  return path;
}

}  // namespace

TEST(Sha256FileTest, KnownVectorAbc) {
  auto path = WriteTempFile("abc", "fl_sha256_test_abc.bin");

  auto hash = fl::Sha256File(path);
  EXPECT_EQ(hash, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");

  std::filesystem::remove(path);
}

TEST(Sha256FileTest, EmptyFile) {
  auto path = WriteTempFile("", "fl_sha256_test_empty.bin");

  auto hash = fl::Sha256File(path);
  EXPECT_EQ(hash, "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");

  std::filesystem::remove(path);
}

TEST(Sha256FileTest, MissingFileReturnsEmpty) {
  auto path = std::filesystem::temp_directory_path() / "fl_sha256_test_does_not_exist.bin";
  std::filesystem::remove(path);

  auto hash = fl::Sha256File(path);
  EXPECT_TRUE(hash.empty());
}

TEST(Sha256StringTest, KnownVectorAbc) {
  EXPECT_EQ(fl::Sha256String("abc"), "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
}

TEST(Sha256StringTest, EmptyInput) {
  EXPECT_EQ(fl::Sha256String(""), "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
}
