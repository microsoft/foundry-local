// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/file_uri.h"

#include <gtest/gtest.h>

TEST(FileUriTest, PlainPathPassesThrough) {
  EXPECT_EQ(fl::PathFromFileUri("C:/path/to/file.wav"), "C:/path/to/file.wav");
  EXPECT_EQ(fl::PathFromFileUri("/usr/local/file.wav"), "/usr/local/file.wav");
}

TEST(FileUriTest, StripsFileScheme) {
  EXPECT_EQ(fl::PathFromFileUri("file:///C:/path/to/file.wav"), "/C:/path/to/file.wav");
  EXPECT_EQ(fl::PathFromFileUri("file:///usr/local/file.wav"), "/usr/local/file.wav");
}

TEST(FileUriTest, PercentDecodesSpaces) {
  EXPECT_EQ(fl::PathFromFileUri("file:///C:/My%20Audio.wav"), "/C:/My Audio.wav");
  EXPECT_EQ(fl::PathFromFileUri("/tmp/My%20File.txt"), "/tmp/My File.txt");
}

TEST(FileUriTest, PercentDecodeAcceptsMixedCaseHex) {
  EXPECT_EQ(fl::PathFromFileUri("a%2Fb%2fc"), "a/b/c");
  std::string expected{static_cast<char>(0xAB), static_cast<char>(0xCD)};
  EXPECT_EQ(fl::PathFromFileUri("%ab%CD"), expected);
}

TEST(FileUriTest, MalformedPercentSequenceLeftIntact) {
  EXPECT_EQ(fl::PathFromFileUri("%"), "%");
  EXPECT_EQ(fl::PathFromFileUri("%2"), "%2");
  EXPECT_EQ(fl::PathFromFileUri("%2G"), "%2G");
  EXPECT_EQ(fl::PathFromFileUri("file%"), "file%");
}

TEST(FileUriTest, EmptyInput) {
  EXPECT_EQ(fl::PathFromFileUri(""), "");
  EXPECT_EQ(fl::PathFromFileUri("file://"), "");
}
