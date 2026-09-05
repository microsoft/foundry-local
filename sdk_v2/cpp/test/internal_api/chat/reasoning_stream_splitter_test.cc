// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/reasoning_stream_splitter.h"
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

using namespace fl;

namespace {

using Segment = ReasoningStreamSplitter::Segment;

void Append(std::vector<Segment>& destination, const std::vector<Segment>& source) {
  for (const auto& segment : source) {
    if (!destination.empty() && destination.back().type == segment.type) {
      destination.back().text += segment.text;
    } else {
      destination.push_back(segment);
    }
  }
}

std::string Collect(const std::vector<Segment>& segments, flTextItemType type) {
  std::string text;
  for (const auto& segment : segments) {
    if (segment.type == type) {
      text += segment.text;
    }
  }
  return text;
}

}  // namespace

TEST(ReasoningStreamSplitterTest, EmptyDecodedSpecialMarkersClassifyVisibleAndReasoningText) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "before "));
  Append(segments, splitter.Push(101, ""));
  Append(segments, splitter.Push(2, "hidden"));
  Append(segments, splitter.Push(102, ""));
  Append(segments, splitter.Push(3, "after"));
  Append(segments, splitter.Flush());

  ASSERT_EQ(segments.size(), 3u);
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(segments[0].text, "before ");
  EXPECT_EQ(segments[1].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  EXPECT_EQ(segments[1].text, "hidden");
  EXPECT_EQ(segments[2].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(segments[2].text, "after");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, MissingEndMarkerDisablesReasoningClassification) {
  ReasoningStreamSplitter splitter("<think>", "", {101}, {});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(101, "<think>"));
  Append(segments, splitter.Push(1, "visible answer"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "<think>visible answer");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "");
  EXPECT_FALSE(splitter.InsideReasoning());
  EXPECT_EQ(splitter.ReasoningTokenCount(), 0);
}

TEST(ReasoningStreamSplitterTest, MissingStartMarkerDisablesReasoningClassification) {
  ReasoningStreamSplitter splitter("", "</think>", {}, {102});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "visible answer"));
  Append(segments, splitter.Push(102, "</think>"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "visible answer</think>");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "");
  EXPECT_FALSE(splitter.InsideReasoning());
  EXPECT_EQ(splitter.ReasoningTokenCount(), 0);
}

TEST(ReasoningStreamSplitterTest, NoTextMarkersOmitIgnoredControlToken) {
  ReasoningStreamSplitter splitter("", "", {}, {}, {99});

  EXPECT_TRUE(splitter.Push(99, "<|im_end|>").empty());
  auto segments = splitter.Push(1, "visible");

  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(segments[0].text, "visible");
}

TEST(ReasoningStreamSplitterTest, IgnoredEmptyDecodedTokensAreNotReasoningContent) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102}, {99});

  EXPECT_TRUE(splitter.Push(101, "").empty());
  EXPECT_TRUE(splitter.Push(99, "").empty());
  EXPECT_TRUE(splitter.Push(102, "").empty());
  EXPECT_EQ(splitter.ReasoningTokenCount(), 0);
}

TEST(ReasoningStreamSplitterTest, IgnoredVisibleControlTokensAreNotEmittedOrCounted) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102}, {99});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(101, ""));
  Append(segments, splitter.Push(1, "hidden"));
  Append(segments, splitter.Push(99, "<|im_end|>"));
  Append(segments, splitter.Push(102, ""));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, IncompleteTokenMarkersOmitIgnoredControlTokenOutsideReasoning) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {}, {}, {99});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "before "));
  Append(segments, splitter.Push(99, "<|im_end|>"));
  Append(segments, splitter.Push(2, "after"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "before after");
  EXPECT_FALSE(splitter.InsideReasoning());
  EXPECT_EQ(splitter.ReasoningTokenCount(), 0);
}

TEST(ReasoningStreamSplitterTest, IncompleteTokenMarkersOmitIgnoredControlTokenInsideReasoning) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {}, {}, {99});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "<think>"));
  Append(segments, splitter.Push(2, "hidden"));
  Append(segments, splitter.Push(99, "<|im_end|>"));
  Append(segments, splitter.Push(3, " more"));
  Append(segments, splitter.Push(4, "</think>"));
  Append(segments, splitter.Push(5, "after"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden more");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "after");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 2);
}

TEST(ReasoningStreamSplitterTest, IgnoredTokenWhoseDecodedTextIsCompleteMarkerStillToggles) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {}, {}, {99});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(99, "<think>"));
  EXPECT_TRUE(splitter.InsideReasoning());

  Append(segments, splitter.Push(1, "hidden"));
  Append(segments, splitter.Push(99, "</think>"));
  EXPECT_FALSE(splitter.InsideReasoning());

  Append(segments, splitter.Push(2, "after"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "after");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, IgnoredTokenCompletesSplitMarkerAsSuffix) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {}, {}, {99});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "<th"));
  Append(segments, splitter.Push(99, "ink>"));
  EXPECT_TRUE(splitter.InsideReasoning());

  Append(segments, splitter.Push(2, "hidden"));
  Append(segments, splitter.Push(3, "</think>"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden");
}

TEST(ReasoningStreamSplitterTest, IgnoredTokenContributesSplitMarkerAsPrefix) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {}, {}, {99});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(99, "<th"));
  Append(segments, splitter.Push(1, "ink>"));
  EXPECT_TRUE(splitter.InsideReasoning());

  Append(segments, splitter.Push(2, "hidden"));
  Append(segments, splitter.Push(3, "</think>"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden");
}

TEST(ReasoningStreamSplitterTest, IgnoredBoundaryTokenSuppressesNonMarkerText) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {}, {}, {99});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "<th"));
  Append(segments, splitter.Push(99, "control<think>ignored"));
  EXPECT_TRUE(splitter.InsideReasoning());

  Append(segments, splitter.Push(2, "hidden"));
  Append(segments, splitter.Push(99, "control</think>ignored"));
  EXPECT_FALSE(splitter.InsideReasoning());

  Append(segments, splitter.Push(3, "after"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "<thafter");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, IgnoredTokenAfterClosingMarkerDoesNotConsumeNewlineTrim) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {}, {}, {99});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "<think>hidden</think>"));
  Append(segments, splitter.Push(99, "<|im_end|>"));
  Append(segments, splitter.Push(2, "\n\nafter"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "\nafter");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, TokenIdMarkerPathTakesPrecedenceOverOverlappingIgnoredIds) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102}, {101, 102, 99});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(101, ""));
  Append(segments, splitter.Push(1, "hidden"));
  Append(segments, splitter.Push(99, "<|im_end|>"));
  Append(segments, splitter.Push(102, ""));
  Append(segments, splitter.Push(2, "after"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "after");
  EXPECT_FALSE(splitter.InsideReasoning());
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, ClosingReasoningRemovesOnlyOneImmediateNewline) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(101, ""));
  Append(segments, splitter.Push(1, "hidden"));
  Append(segments, splitter.Push(102, ""));
  Append(segments, splitter.Push(2, "\n\n  indented"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "\n  indented");
}

TEST(ReasoningStreamSplitterTest, DecodedClosingMarkerRemovesOnlyOneImmediateNewline) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "<think>hidden</think>\n\n  indented"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "\n  indented");
}

TEST(ReasoningStreamSplitterTest, ClosingReasoningPreservesLeadingSpaces) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(101, ""));
  Append(segments, splitter.Push(1, "hidden"));
  Append(segments, splitter.Push(102, ""));
  Append(segments, splitter.Push(2, "  code"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "  code");
}

TEST(ReasoningStreamSplitterTest, MultiTokenMarkersAreBufferedAndNeverExposed) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {10, 11, 12}, {20, 21});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "preface "));
  Append(segments, splitter.Push(10, "<"));
  Append(segments, splitter.Push(11, "think"));
  Append(segments, splitter.Push(12, ">"));
  Append(segments, splitter.Push(30, "step"));
  Append(segments, splitter.Push(31, " one"));
  Append(segments, splitter.Push(20, "</think"));
  Append(segments, splitter.Push(21, ">"));
  Append(segments, splitter.Push(40, "\nanswer"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "preface answer");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "step one");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 2);
}

TEST(ReasoningStreamSplitterTest, FlushEmitsPartialMultiTokenMarkerAsContent) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {10, 11, 12}, {20, 21});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "before "));
  Append(segments, splitter.Push(10, "<"));
  Append(segments, splitter.Push(11, "think"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "before <think");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 0);
}

TEST(ReasoningStreamSplitterTest, DecodedOrdinaryMarkersRemainAClassificationFallback) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {100}, {200});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(1, "visible "));
  Append(segments, splitter.Push(2, "<thi"));
  Append(segments, splitter.Push(3, "nk>"));
  Append(segments, splitter.Push(4, "reasoning"));
  Append(segments, splitter.Push(5, "</"));
  Append(segments, splitter.Push(6, "think"));
  Append(segments, splitter.Push(7, ">"));
  Append(segments, splitter.Push(8, "answer"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "visible answer");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "reasoning");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, TextOnlyFallbackCountsReasoningTokens) {
  ReasoningStreamSplitter splitter("<think>", "</think>");
  std::vector<Segment> segments;

  Append(segments, splitter.Push("visible "));
  Append(segments, splitter.Push("<think>"));
  Append(segments, splitter.Push("reasoning"));
  Append(segments, splitter.Push("</think>"));
  Append(segments, splitter.Push("answer"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "visible answer");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "reasoning");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, TruncatedReasoningRemainsReasoningThroughEndOfGeneration) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {10}, {20});
  std::vector<Segment> segments;

  Append(segments, splitter.Push(10, ""));
  Append(segments, splitter.Push(30, "unfinished "));
  Append(segments, splitter.Push(31, "thought"));
  Append(segments, splitter.Flush());

  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  EXPECT_EQ(segments[0].text, "unfinished thought");
  EXPECT_TRUE(splitter.InsideReasoning());
  EXPECT_EQ(splitter.ReasoningTokenCount(), 2);
}

TEST(ReasoningStreamSplitterTest, ToolCallShapedReasoningNeverReachesToolAccumulator) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {10}, {20});
  ToolCallStreamAccumulator accumulator("<tool_call>", "</tool_call>");
  std::vector<Segment> segments;
  std::vector<ParsedToolCall> calls;
  std::string visible;

  const auto route = [&](const std::vector<Segment>& emitted) {
    Append(segments, emitted);
    for (const auto& segment : emitted) {
      if (segment.type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
        continue;
      }

      auto output = accumulator.Push(segment.text);
      for (auto& event : output.events) {
        if (auto* text = std::get_if<std::string>(&event)) {
          visible += *text;
        } else {
          calls.push_back(std::move(std::get<ParsedToolCall>(event)));
        }
      }
    }
  };

  route(splitter.Push(10, ""));
  route(splitter.Push(30, R"(<tool_call>{"name":"unsafe","arguments":{}}</tool_call>)"));
  route(splitter.Push(20, ""));
  route(splitter.Push(40, "safe answer"));
  route(splitter.Flush());

  auto final_output = accumulator.Flush();
  for (auto& event : final_output.events) {
    if (auto* text = std::get_if<std::string>(&event)) {
      visible += *text;
    } else {
      calls.push_back(std::move(std::get<ParsedToolCall>(event)));
    }
  }

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING),
            R"(<tool_call>{"name":"unsafe","arguments":{}}</tool_call>)");
  EXPECT_EQ(visible, "safe answer");
  EXPECT_TRUE(calls.empty());
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}
