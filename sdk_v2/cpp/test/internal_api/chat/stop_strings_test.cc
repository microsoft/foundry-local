// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/stop_strings.h"

#include "exception.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace fl;

namespace {

template <typename Fn>
void ExpectInvalidArgument(Fn&& fn, const std::string& message_fragment) {
  try {
    fn();
    FAIL() << "Expected fl::Exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(e.what()).find(message_fragment), std::string::npos) << e.what();
  }
}

}  // namespace

TEST(StopStringFilterTest, EmptyConfigurationIsPassthrough) {
  StopStringFilter filter;
  EXPECT_EQ(filter.Push("hello"), "hello");
  EXPECT_EQ(filter.Flush(), "");
  EXPECT_FALSE(filter.matched());
  EXPECT_FALSE(filter.matched_index().has_value());
}

TEST(StopStringFilterTest, ReportsWhetherOutputRemainsAlignedWithTheCurrentToken) {
  StopStringFilter filter({"END"});

  auto passthrough = filter.PushWithTokenAlignment("hello", 1);
  EXPECT_EQ(passthrough.text, "hello");
  EXPECT_TRUE(passthrough.token_aligned);
  ASSERT_EQ(passthrough.fragments.size(), 1u);
  EXPECT_EQ(passthrough.fragments[0].token_id, 1);

  auto buffered = filter.PushWithTokenAlignment("E", 2);
  EXPECT_TRUE(buffered.text.empty());
  EXPECT_FALSE(buffered.token_aligned);
  EXPECT_TRUE(buffered.fragments.empty());

  // The released prefix happens to equal the current fragment, but belongs to the preceding token.
  auto combined = filter.PushWithTokenAlignment("E", 3);
  EXPECT_EQ(combined.text, "E");
  EXPECT_FALSE(combined.token_aligned);
  ASSERT_EQ(combined.fragments.size(), 1u);
  EXPECT_EQ(combined.fragments[0].token_id, 2);
}

TEST(StopStringFilterTest, PreservesTokenProvenanceWhenMatchShortensBufferedFragment) {
  StopStringFilter filter({"STOP"});

  auto first = filter.PushWithTokenAlignment("thought ST", 10);
  EXPECT_TRUE(first.fragments.empty());

  auto matched = filter.PushWithTokenAlignment("OPtail", 11);
  ASSERT_EQ(matched.fragments.size(), 1u);
  EXPECT_EQ(matched.fragments[0].text, "thought ");
  EXPECT_EQ(matched.fragments[0].token_id, 10);
  EXPECT_TRUE(filter.matched());
}

TEST(StopStringFilterTest, MatchAcrossFragmentsDropsStopAndLaterBytes) {
  StopStringFilter filter({"END"});

  EXPECT_EQ(filter.Push("hello E"), "hello ");
  EXPECT_EQ(filter.Push("ND later"), "");
  EXPECT_TRUE(filter.matched());
  ASSERT_TRUE(filter.matched_index().has_value());
  EXPECT_EQ(*filter.matched_index(), 0u);
  EXPECT_EQ(filter.Flush(), "");
}

TEST(StopStringFilterTest, MatchStartingMidFragmentDropsTrailingBytesInSameChunk) {
  StopStringFilter filter({"STOP"});

  EXPECT_EQ(filter.Push("abcSTOPtail"), "abc");
  EXPECT_TRUE(filter.matched());
  EXPECT_EQ(filter.Push("ignored"), "");
  EXPECT_EQ(filter.Flush(), "");
}

TEST(StopStringFilterTest, EarliestEndingMatchWins) {
  StopStringFilter filter({"abcd", "bc"});

  EXPECT_EQ(filter.Push("abcd"), "a");
  EXPECT_TRUE(filter.matched());
  ASSERT_TRUE(filter.matched_index().has_value());
  EXPECT_EQ(*filter.matched_index(), 1u);
}

TEST(StopStringFilterTest, SameEndPrefersLongestThenLowestCallerIndex) {
  StopStringFilter filter({"END", "ND", "END"});

  EXPECT_EQ(filter.Push("END"), "");
  EXPECT_TRUE(filter.matched());
  ASSERT_TRUE(filter.matched_index().has_value());
  EXPECT_EQ(*filter.matched_index(), 0u);
}

TEST(StopStringFilterTest, FlushReleasesPendingPartialPrefixAtEndOfStream) {
  StopStringFilter filter({"END"});

  EXPECT_EQ(filter.Push("hello EN"), "hello ");
  EXPECT_FALSE(filter.matched());
  EXPECT_EQ(filter.Flush(), "EN");
}

TEST(StopStringFilterTest, DisambiguationReleasesBufferedPrefixImmediately) {
  StopStringFilter filter({"END"});

  EXPECT_EQ(filter.Push("E"), "");
  EXPECT_EQ(filter.Push("x"), "Ex");
  EXPECT_FALSE(filter.matched());
  EXPECT_EQ(filter.Flush(), "");
}

TEST(StopStringFilterTest, HandlesUtf8StopStringsAcrossFragments) {
  StopStringFilter filter({"\xE7\x8C\xAB\xE6\xAD\xA2"});

  EXPECT_EQ(filter.Push("A\xE7\x8C\xAB"), "A");
  EXPECT_EQ(filter.Push("\xE6\xAD\xA2"
                        "B"),
            "");
  EXPECT_TRUE(filter.matched());
  ASSERT_TRUE(filter.matched_index().has_value());
  EXPECT_EQ(*filter.matched_index(), 0u);
}

TEST(StopStringFilterTest, StoreAndLoadRoundTripsInternalOption) {
  KeyValuePairs options;
  StoreStopStringsOption({"END", "STOP"}, options);

  EXPECT_EQ(LoadStopStringsOption(options), (std::vector<std::string>{"END", "STOP"}));

  StoreStopStringsOption({}, options);
  EXPECT_EQ(options.Find(kInternalStopStringsOptionKey), nullptr);
}

TEST(StopStringFilterTest, NormalizeOpenAiStopStringsDeduplicatesInCallerOrder) {
  auto normalized = NormalizeOpenAiStopStrings(nlohmann::json::array({"STOP", "END", "STOP", "END"}));

  EXPECT_EQ(normalized, (std::vector<std::string>{"STOP", "END"}));
}

TEST(StopStringFilterTest, NormalizeOpenAiStopStringsRejectsInvalidUtf8) {
  ExpectInvalidArgument(
      []() {
        NormalizeOpenAiStopStrings(nlohmann::json(std::string("\xC3\x28", 2)));
      },
      "valid UTF-8");
}

TEST(StopStringFilterTest, NormalizeOpenAiStopStringsRejectsOversizedPayload) {
  ExpectInvalidArgument(
      []() {
        NormalizeOpenAiStopStrings(nlohmann::json(std::string(16 * 1024 + 1, 'x')));
      },
      "16384 UTF-8 bytes");
}

TEST(StopStringFilterTest, LoadStopStringsOptionRejectsOversizedSerializedPayloadBeforeParsing) {
  KeyValuePairs options;
  options[kInternalStopStringsOptionKey] = std::string(128 * 1024 + 1, ' ');

  ExpectInvalidArgument([&]() { LoadStopStringsOption(options); }, "131072 serialized bytes");
}

TEST(StopStringFilterTest, LoadStopStringsOptionReportsMalformedJsonAsInvalidArgument) {
  KeyValuePairs options;
  options[kInternalStopStringsOptionKey] = R"(["unterminated")";

  ExpectInvalidArgument([&]() { LoadStopStringsOption(options); }, "not valid JSON");
}
