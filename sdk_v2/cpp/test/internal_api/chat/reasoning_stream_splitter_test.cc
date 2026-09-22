// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/reasoning_stream_splitter.h"
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
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

// ===========================================================================
// Prompts that open reasoning themselves
//
// Some package templates end the rendered assistant prompt with the configured beginning-of-reasoning marker. That
// marker is prompt input, so generation starts inside reasoning and the model only ever emits the closing marker.
// ===========================================================================

namespace {

// A prompt whose template opened the assistant turn's reasoning block, and its encoded form. Token 101 is the
// beginning-of-reasoning marker, 102 the closing marker.
constexpr const char* kPromptOpenedByTemplate = "<|im_start|>assistant\n<think>\n";

ReasoningMarkers MakeMarkers(std::vector<int32_t> start_ids = {101}, std::vector<int32_t> end_ids = {102}) {
  return {"<think>", "</think>", std::move(start_ids), std::move(end_ids), true};
}

}  // namespace

TEST(PromptOpensReasoningTest, TemplateEmittedOpenerAtPromptTailOpensReasoning) {
  const std::vector<int32_t> prompt_ids = {5, 6, 7, 101, 8};

  EXPECT_TRUE(PromptOpensReasoning(prompt_ids, MakeMarkers(), kPromptOpenedByTemplate));
}

TEST(PromptOpensReasoningTest, OpenerFollowedOnlyByWhitespaceStillOpensReasoning) {
  const std::vector<int32_t> prompt_ids = {5, 101, 8};

  EXPECT_TRUE(PromptOpensReasoning(prompt_ids, MakeMarkers(), "<|im_start|>assistant\n<think>"));
  EXPECT_TRUE(PromptOpensReasoning(prompt_ids, MakeMarkers(), "<|im_start|>assistant\n<think> \t\r\n"));
}

TEST(PromptOpensReasoningTest, MarkerInsideMessageContentDoesNotOpenReasoning) {
  // The decisive case: a user (or tool result) mentioning an unbalanced marker must never seed the splitter, or the
  // whole assistant answer would be reclassified as hidden reasoning. The encoded prompt carries the opener ID and
  // no closing ID, so only the positional rule rejects this.
  const std::vector<int32_t> prompt_ids = {5, 101, 6, 7};

  EXPECT_FALSE(PromptOpensReasoning(prompt_ids, MakeMarkers(),
                                    "<|im_start|>user\nwhat does the <think> tag do?<|im_end|>\n"
                                    "<|im_start|>assistant\n"));
}

TEST(PromptOpensReasoningTest, ToolResultQuotingAnOpenerDoesNotOpenReasoning) {
  const std::vector<int32_t> prompt_ids = {5, 101, 6};

  EXPECT_FALSE(PromptOpensReasoning(prompt_ids, MakeMarkers(),
                                    "<|im_start|>tool\nthe page says <think> is a tag<|im_end|>\n"
                                    "<|im_start|>assistant\n"));
}

TEST(PromptOpensReasoningTest, ClosingMarkerAfterOpenerInTokenIdsDoesNotOpenReasoning) {
  // Token evidence disagrees with the tail: the encoded prompt closed the block, so reasoning is not open.
  const std::vector<int32_t> prompt_ids = {101, 9, 102};

  EXPECT_FALSE(PromptOpensReasoning(prompt_ids, MakeMarkers(), kPromptOpenedByTemplate));
}

TEST(PromptOpensReasoningTest, MissingOpenerInTokenIdsDoesNotOpenReasoning) {
  const std::vector<int32_t> prompt_ids = {5, 6, 7};

  EXPECT_FALSE(PromptOpensReasoning(prompt_ids, MakeMarkers(), kPromptOpenedByTemplate));
}

TEST(PromptOpensReasoningTest, BalancedEarlierBlockDoesNotOpenReasoning) {
  // A previous turn opened and closed a reasoning block; the prompt ends outside reasoning.
  const std::vector<int32_t> prompt_ids = {101, 9, 102, 5, 6};

  EXPECT_FALSE(PromptOpensReasoning(prompt_ids, MakeMarkers(),
                                    "<think>prior</think>\n<|im_start|>assistant\n"));
}

TEST(PromptOpensReasoningTest, EarlierBalancedBlockThenTemplateOpenerOpensReasoning) {
  // Rebuilt history can render a prior reasoning block before the template opens a new one for this turn.
  const std::vector<int32_t> prompt_ids = {101, 9, 102, 5, 101, 8};

  EXPECT_TRUE(PromptOpensReasoning(prompt_ids, MakeMarkers(),
                                   "<think>prior</think>\n<|im_start|>assistant\n<think>\n"));
}

TEST(PromptOpensReasoningTest, PromptWithoutAnyOpenerDoesNotOpenReasoning) {
  const std::vector<int32_t> prompt_ids = {5, 6, 7};

  EXPECT_FALSE(PromptOpensReasoning(prompt_ids, MakeMarkers(), "<|im_start|>assistant\n"));
}

TEST(PromptOpensReasoningTest, FallsBackToMarkerTextWhenModelPublishesNoMarkerIds) {
  const auto markers = MakeMarkers({}, {});

  EXPECT_TRUE(PromptOpensReasoning({}, markers, kPromptOpenedByTemplate));
  EXPECT_FALSE(PromptOpensReasoning({}, markers, "<think>prior</think>\n<|im_start|>assistant\n"));
  EXPECT_FALSE(PromptOpensReasoning({}, markers, "<|im_start|>assistant\n"));
  EXPECT_FALSE(PromptOpensReasoning({}, markers,
                                    "<|im_start|>user\nwhat does the <think> tag do?<|im_end|>\n"
                                    "<|im_start|>assistant\n"));
}

TEST(PromptOpensReasoningTest, EmptyPromptTokenIdsFallBackToTheMarkerText) {
  // The media path has no encoded sequence of its own.
  EXPECT_TRUE(PromptOpensReasoning({}, MakeMarkers(), kPromptOpenedByTemplate));
}

TEST(PromptOpensReasoningTest, UnconfiguredMarkersNeverOpenReasoning) {
  ReasoningMarkers markers;

  EXPECT_FALSE(PromptOpensReasoning(std::vector<int32_t>{101}, markers, "<think>"));
}

TEST(ReasoningStreamSplitterTest, PromptOpenedReasoningIsClassifiedUntilClosingMarker) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102}, {},
                                   /*starts_inside_reasoning=*/true);
  std::vector<Segment> segments;

  EXPECT_TRUE(splitter.InsideReasoning());

  // The opener came from the prompt, so the model emits only scratchpad, the closing marker, then the answer.
  Append(segments, splitter.Push(1, "let me work this out"));
  Append(segments, splitter.Push(102, ""));
  Append(segments, splitter.Push(2, "the answer is 4"));
  Append(segments, splitter.Flush());

  ASSERT_EQ(segments.size(), 2u);
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  EXPECT_EQ(segments[0].text, "let me work this out");
  EXPECT_EQ(segments[1].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(segments[1].text, "the answer is 4");

  // The seeded opener is not a generated token, and the generated closing marker is suppressed rather than counted.
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
  EXPECT_FALSE(splitter.InsideReasoning());
}

TEST(ReasoningStreamSplitterTest, PromptOpenedReasoningSuppressesDecodedClosingMarker) {
  // Same flow for a model whose closing marker decodes to visible text rather than an empty special token.
  ReasoningStreamSplitter splitter("<think>", "</think>", {}, {}, {},
                                   /*starts_inside_reasoning=*/true);
  std::vector<Segment> segments;

  Append(segments, splitter.Push("scratch"));
  Append(segments, splitter.Push("</think>"));
  Append(segments, splitter.Push("visible"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "scratch");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "visible");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, TruncatedPromptOpenedReasoningStaysReasoning) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102}, {},
                                   /*starts_inside_reasoning=*/true);
  std::vector<Segment> segments;

  // Generation stops (max_output_tokens) before the model ever emits the closing marker.
  Append(segments, splitter.Push(1, "still "));
  Append(segments, splitter.Push(2, "thinking"));
  Append(segments, splitter.Flush());

  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  EXPECT_EQ(segments[0].text, "still thinking");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "");
  EXPECT_EQ(splitter.ReasoningTokenCount(), 2);
  EXPECT_TRUE(splitter.InsideReasoning());
}

TEST(ReasoningStreamSplitterTest, ToolShapedTextInsidePromptOpenedReasoningIsNotAToolCall) {
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102}, {},
                                   /*starts_inside_reasoning=*/true);
  ToolCallStreamAccumulator accumulator("<tool_call>", "</tool_call>");

  std::vector<Segment> segments;
  std::string visible;
  std::vector<ParsedToolCall> calls;

  // Mirrors ChatSession: REASONING segments bypass the tool-call accumulator, DEFAULT segments feed it.
  auto route = [&](const std::vector<Segment>& produced) {
    Append(segments, produced);
    for (const auto& segment : produced) {
      if (segment.type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
        continue;
      }

      for (auto& event : accumulator.Push(segment.text).events) {
        if (auto* text = std::get_if<std::string>(&event)) {
          visible += *text;
        } else {
          calls.push_back(std::move(std::get<ParsedToolCall>(event)));
        }
      }
    }
  };

  route(splitter.Push(1, R"(<tool_call>{"name":"unsafe","arguments":{}}</tool_call>)"));
  route(splitter.Push(102, ""));
  route(splitter.Push(2, "safe answer"));
  route(splitter.Flush());

  for (auto& event : accumulator.Flush().events) {
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

TEST(ReasoningStreamSplitterTest, ModelGeneratedOpenerIsUnchangedWhenPromptDoesNotOpenReasoning) {
  // The default (unseeded) path must behave exactly as before: the model emits both markers itself.
  ReasoningStreamSplitter splitter("<think>", "</think>", {101}, {102}, {},
                                   /*starts_inside_reasoning=*/false);
  std::vector<Segment> segments;

  EXPECT_FALSE(splitter.InsideReasoning());

  Append(segments, splitter.Push(1, "before "));
  Append(segments, splitter.Push(101, ""));
  Append(segments, splitter.Push(2, "hidden"));
  Append(segments, splitter.Push(102, ""));
  Append(segments, splitter.Push(3, "after"));
  Append(segments, splitter.Flush());

  ASSERT_EQ(segments.size(), 3u);
  EXPECT_EQ(segments[0].text, "before ");
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(segments[1].text, "hidden");
  EXPECT_EQ(segments[1].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  EXPECT_EQ(segments[2].text, "after");
  EXPECT_EQ(segments[2].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(splitter.ReasoningTokenCount(), 1);
}

TEST(ReasoningStreamSplitterTest, SeedingIsIgnoredWithoutConfiguredMarkers) {
  // A non-reasoning model must stay a DEFAULT passthrough even if the flag is set.
  ReasoningStreamSplitter splitter("", "", {}, {}, {}, /*starts_inside_reasoning=*/true);

  EXPECT_FALSE(splitter.InsideReasoning());

  auto segments = splitter.Push(1, "visible");
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(splitter.ReasoningTokenCount(), 0);
}

// ========================================================================
// ResolveMarkerTokenIds — the IDs must describe the marker actually in effect.
//
// The marker *string* can be overridden per request while the ID a model publishes always describes the model's own
// marker. Pairing the two would make the splitter flip its reasoning state on a token that is not the boundary.
// ========================================================================

namespace {

/// A stand-in tokenizer: every marker it knows maps to a token ID sequence, everything else encodes to nothing.
struct FakeEncoder {
  std::vector<std::pair<std::string, std::vector<int32_t>>> vocabulary;

  std::vector<int32_t> operator()(const std::string& text) const {
    for (const auto& [marker, ids] : vocabulary) {
      if (marker == text) {
        return ids;
      }
    }

    return {};
  }
};

}  // namespace

TEST(ResolveMarkerTokenIdsTest, PublishedIdIsUsedWhenItDecodesToTheEffectiveMarker) {
  FakeEncoder encoder{{{"<think>", {999}}}};

  const auto ids = ResolveMarkerTokenIds("<think>", {151667, "<think>"}, encoder);

  // The published ID is proven to be this marker, so it is preferred over re-encoding.
  EXPECT_EQ(ids, (std::vector<int32_t>{151667}));
}

TEST(ResolveMarkerTokenIdsTest, OverrideThatDiffersFromTheModelMarkerIsEncodedInstead) {
  FakeEncoder encoder{{{"<reasoning>", {42}}}};

  // The model publishes ID 151667 for "<think>"; the request asked for "<reasoning>". Using 151667 would flip the
  // reasoning state on the wrong token and leak the scratchpad.
  const auto ids = ResolveMarkerTokenIds("<reasoning>", {151667, "<think>"}, encoder);

  EXPECT_EQ(ids, (std::vector<int32_t>{42}));
}

TEST(ResolveMarkerTokenIdsTest, MultiTokenOverrideKeepsItsWholeSequence) {
  FakeEncoder encoder{{{"BEGIN REASONING", {10, 11, 12}}}};

  const auto ids = ResolveMarkerTokenIds("BEGIN REASONING", {151667, "<think>"}, encoder);

  EXPECT_EQ(ids, (std::vector<int32_t>{10, 11, 12}));
}

TEST(ResolveMarkerTokenIdsTest, PublishedIdThatDecodesToNothingIsNotAssumedToBeTheMarker) {
  // A tokenizer that skips special tokens decodes a valid ID to an empty string. That is not proof of identity, so
  // the marker in effect is encoded rather than paired with the unproven ID.
  FakeEncoder encoder{{{"<think>", {7}}}};

  const auto ids = ResolveMarkerTokenIds("<think>", {151667, ""}, encoder);

  EXPECT_EQ(ids, (std::vector<int32_t>{7}));
}

TEST(ResolveMarkerTokenIdsTest, UnencodableMarkerFallsBackToTextMatching) {
  FakeEncoder encoder{};

  const auto ids = ResolveMarkerTokenIds("<reasoning>", {151667, "<think>"}, encoder);

  // Empty IDs are safe: the splitter and the prompt probe both fall back to matching decoded text.
  EXPECT_TRUE(ids.empty());
}

TEST(ResolveMarkerTokenIdsTest, AnEmptyMarkerResolvesToNoIds) {
  FakeEncoder encoder{{{"", {5}}}};

  EXPECT_TRUE(ResolveMarkerTokenIds("", {151667, "<think>"}, encoder).empty());
}

TEST(ResolveMarkerTokenIdsTest, ModelWithoutPublishedIdsEncodesTheMarker) {
  FakeEncoder encoder{{{"<think>", {3, 4}}}};

  const auto ids = ResolveMarkerTokenIds("<think>", {std::nullopt, ""}, encoder);

  EXPECT_EQ(ids, (std::vector<int32_t>{3, 4}));
}

TEST(PromptOpensReasoningTest, OverriddenMarkerIdsDecideThePromptProbe) {
  // End-to-end for the override: the encoded IDs of the *overridden* markers are what the prompt is checked against.
  FakeEncoder encoder{{{"<reasoning>", {70, 71}}, {"</reasoning>", {80, 81}}}};

  ReasoningMarkers markers;
  markers.start = "<reasoning>";
  markers.end = "</reasoning>";
  markers.start_token_ids = ResolveMarkerTokenIds(markers.start, {151667, "<think>"}, encoder);
  markers.end_token_ids = ResolveMarkerTokenIds(markers.end, {151668, "</think>"}, encoder);

  const std::vector<int32_t> open_prompt{1, 2, 70, 71};
  EXPECT_TRUE(PromptOpensReasoning(open_prompt, markers, "user turn<reasoning>"));

  const std::vector<int32_t> closed_prompt{1, 2, 70, 71, 5, 80, 81};
  // The model's own IDs appear nowhere in this prompt; only the overridden sequences decide.
  EXPECT_FALSE(PromptOpensReasoning(closed_prompt, markers, "user turn<reasoning>hidden</reasoning>"));
}

TEST(ReasoningStreamSplitterTest, OverriddenMarkerIdsClassifyReasoningAndLeakNothing) {
  FakeEncoder encoder{{{"<reasoning>", {70}}, {"</reasoning>", {80}}}};

  const auto start_ids = ResolveMarkerTokenIds("<reasoning>", {151667, "<think>"}, encoder);
  const auto end_ids = ResolveMarkerTokenIds("</reasoning>", {151668, "</think>"}, encoder);

  ReasoningStreamSplitter splitter("<reasoning>", "</reasoning>", start_ids, end_ids);

  std::vector<Segment> segments;
  Append(segments, splitter.Push(1, "visible "));
  // 151667 is the model's published opener ID. It is not the marker in effect and must be ordinary content.
  Append(segments, splitter.Push(151667, "<think>"));
  Append(segments, splitter.Push(70, "<reasoning>"));
  Append(segments, splitter.Push(2, "hidden"));
  Append(segments, splitter.Push(80, "</reasoning>"));
  Append(segments, splitter.Push(3, "answer"));
  Append(segments, splitter.Flush());

  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING), "hidden");
  EXPECT_EQ(Collect(segments, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT), "visible <think>answer");
  EXPECT_FALSE(splitter.InsideReasoning());
}

TEST(PromptOpensReasoningTest, AbsentDedicatedOpenerTokenRefutesTheTextMatch) {
  // A single-ID marker is a dedicated token. If the encoding does not contain it, the trailing text only looked like
  // the marker, and seeding reasoning would hide the whole answer.
  ReasoningMarkers markers;
  markers.start = "<think>";
  markers.end = "</think>";
  markers.start_token_ids = {151667};
  markers.end_token_ids = {151668};
  markers.start_token_is_published = true;

  const std::vector<int32_t> prompt{1, 2, 3};
  EXPECT_FALSE(PromptOpensReasoning(prompt, markers, "chat<think>"));
}

TEST(PromptOpensReasoningTest, AbsentMultiTokenOpenerLeavesThePositionalRuleStanding) {
  // An overridden marker usually encodes to several ordinary tokens, and a tokenizer may merge its leading bytes
  // with whatever precedes them — so the sequence can be missing from the prompt's encoding even though the marker
  // really is its last text. Refusing to seed there would leak the scratchpad of every prompt-opened turn.
  ReasoningMarkers markers;
  markers.start = "BEGIN REASONING";
  markers.end = "END REASONING";
  markers.start_token_ids = {10, 11, 12};
  markers.end_token_ids = {20, 21};

  const std::vector<int32_t> prompt{1, 2, 3};
  EXPECT_TRUE(PromptOpensReasoning(prompt, markers, "chat\nBEGIN REASONING"));

  // The positional rule is still the gate: marker text in the middle of the prompt does not seed anything.
  EXPECT_FALSE(PromptOpensReasoning(prompt, markers, "BEGIN REASONING quoted by a tool result\nassistant:"));
}

TEST(PromptOpensReasoningTest, AbsentEncodeDerivedSingleTokenLeavesThePositionalRuleStanding) {
  ReasoningMarkers markers;
  markers.start = "<custom-think>";
  markers.end = "</custom-think>";
  markers.start_token_ids = {42};
  markers.end_token_ids = {43};
  markers.start_token_is_published = false;

  EXPECT_TRUE(PromptOpensReasoning(std::vector<int32_t>{1, 2}, markers, "assistant\n<custom-think>\n"));
}

TEST(PromptOpensReasoningTest, AMultiTokenOpenerFoundInTheEncodingStillObeysItsCloser) {
  // When the sequence *is* locatable, the balance rule applies exactly as it does for dedicated tokens.
  ReasoningMarkers markers;
  markers.start = "BEGIN REASONING";
  markers.end = "END REASONING";
  markers.start_token_ids = {10, 11, 12};
  markers.end_token_ids = {20, 21};

  const std::vector<int32_t> open_prompt{1, 10, 11, 12};
  EXPECT_TRUE(PromptOpensReasoning(open_prompt, markers, "chat\nBEGIN REASONING"));

  const std::vector<int32_t> closed_prompt{10, 11, 12, 5, 20, 21, 10, 11, 12};
  EXPECT_TRUE(PromptOpensReasoning(closed_prompt, markers, "chat\nBEGIN REASONING"));

  // Closer after the last opener: the block the prompt left behind is finished.
  const std::vector<int32_t> balanced_prompt{10, 11, 12, 5, 20, 21};
  EXPECT_FALSE(PromptOpensReasoning(balanced_prompt, markers, "chat\nBEGIN REASONING"));
}
