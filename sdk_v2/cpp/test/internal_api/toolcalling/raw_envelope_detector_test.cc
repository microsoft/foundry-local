// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/toolcalling/raw_envelope_detector.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/chat/search_options.h"

#include <gtest/gtest.h>

#include <iomanip>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace fl;

namespace {

RawEnvelopeDescriptor Descriptor() {
  return {"apply_patch", "*** Begin Patch", "*** End Patch"};
}

struct Result {
  std::string text;
  std::string rejected_text;
  std::vector<ParsedToolCall> calls;
};

void Append(Result& result, RawEnvelopeDetector::Output output) {
  for (auto& event : output.events) {
    if (auto* text = std::get_if<std::string>(&event)) {
      result.text += *text;
      continue;
    }

    if (auto* rejected = std::get_if<RawEnvelopeDetector::RejectedCandidate>(&event)) {
      result.text += rejected->text;
      result.rejected_text += rejected->text;
      continue;
    }

    result.calls.push_back(std::move(std::get<ParsedToolCall>(event)));
  }
}

Result Read(const std::vector<std::string>& chunks) {
  RawEnvelopeDetector detector(Descriptor());
  Result result;
  for (const auto& chunk : chunks) {
    Append(result, detector.Push(chunk));
  }
  Append(result, detector.FinalizeNatural());
  return result;
}

const std::string kEnvelope =
    "*** Begin Patch\n*** Update File: a.txt\n@@\n-old\n+new\n*** End Patch";

}  // namespace

TEST(RawEnvelopeDetectorTest, RecognizesExactMarkerInclusiveBytes) {
  const auto result = Read({kEnvelope});
  ASSERT_EQ(result.calls.size(), 1u);
  EXPECT_EQ(result.calls[0].name, "apply_patch");
  EXPECT_EQ(result.calls[0].arguments, kEnvelope);
  EXPECT_EQ(result.calls[0].argument_source, kEnvelope);
  EXPECT_TRUE(result.calls[0].raw_envelope);
  EXPECT_TRUE(result.text.empty());
}

TEST(RawEnvelopeDetectorTest, RecognitionIsIndependentOfEveryByteSplit) {
  for (size_t split = 0; split <= kEnvelope.size(); ++split) {
    const auto result = Read({kEnvelope.substr(0, split), kEnvelope.substr(split)});
    ASSERT_EQ(result.calls.size(), 1u) << split;
    EXPECT_EQ(result.calls[0].arguments, kEnvelope) << split;
  }
}

TEST(RawEnvelopeDetectorTest, ReleasesOrdinarySingleLineTextButRetainsASplitStartMarker) {
  RawEnvelopeDetector detector(Descriptor());
  Result result;

  Append(result, detector.Push("ordinary prose"));
  EXPECT_EQ(result.text, "ordinary prose");
  EXPECT_TRUE(result.calls.empty());

  Append(result, detector.Push(" continues"));
  EXPECT_EQ(result.text, "ordinary prose continues");
  EXPECT_TRUE(result.calls.empty());

  Append(result, detector.Push("\n*** Beg"));
  EXPECT_EQ(result.text, "ordinary prose continues\n");
  EXPECT_TRUE(result.calls.empty());

  Append(result, detector.Push("in Patch\npayload\n*** End Patch"));
  Append(result, detector.FinalizeNatural());
  ASSERT_EQ(result.calls.size(), 1u);
  EXPECT_EQ(result.calls.front().arguments,
            "*** Begin Patch\npayload\n*** End Patch");
}

TEST(RawEnvelopeDetectorTest, PreservesPrefixAndStopsAfterFirstCompletedCall) {
  const auto result = Read({"before\n" + kEnvelope + "\nafter\n" + kEnvelope});
  ASSERT_EQ(result.calls.size(), 1u);
  EXPECT_EQ(result.text, "before\n\nafter\n" + kEnvelope);
}

TEST(RawEnvelopeDetectorTest, FencedEnvelopeStaysText) {
  for (const auto& fence : {"```patch", "~~~"}) {
    const auto input = std::string(fence) + "\n" + kEnvelope + "\n" +
                       std::string(3, fence[0]) + "\n";
    const auto result = Read({input});
    EXPECT_TRUE(result.calls.empty());
    EXPECT_EQ(result.text, input);
  }
}

TEST(RawEnvelopeDetectorTest, SuffixedFenceCloserStaysOpenAcrossEveryByteSplit) {
  const auto input = std::string("```patch\n```suffix\n") + kEnvelope + "\n```\n";
  for (size_t split = 0; split <= input.size(); ++split) {
    const auto result = Read({input.substr(0, split), input.substr(split)});
    EXPECT_TRUE(result.calls.empty()) << split;
    EXPECT_EQ(result.text, input) << split;
  }
}

TEST(RawEnvelopeDetectorTest, SpaceAndTabFenceCloserSuffixAllowsLaterEnvelope) {
  const auto prefix = std::string("```patch\nnot an envelope\n``` \t\n");
  const auto result = Read({prefix + kEnvelope});

  ASSERT_EQ(result.calls.size(), 1u);
  EXPECT_EQ(result.calls.front().arguments, kEnvelope);
  EXPECT_EQ(result.text, prefix);
}

TEST(RawEnvelopeDetectorTest, IndentedSuffixedMisspelledAndReversedMarkersStayText) {
  const std::vector<std::string> inputs{
      " *** Begin Patch\nx\n*** End Patch",
      "*** Begin Patch suffix\nx\n*** End Patch",
      "*** Begin patch\nx\n*** End Patch",
      "*** End Patch\nx\n*** Begin Patch\n",
  };

  for (const auto& input : inputs) {
    const auto result = Read({input});
    EXPECT_TRUE(result.calls.empty()) << input;
    EXPECT_EQ(result.text, input);
  }
}

TEST(RawEnvelopeDetectorTest, IncompleteEnvelopeStaysText) {
  const auto input = "*** Begin Patch\n*** Update File: a.txt\n";
  const auto result = Read({input});
  EXPECT_TRUE(result.calls.empty());
  EXPECT_EQ(result.text, input);
  EXPECT_EQ(result.rejected_text, input);
}

TEST(RawEnvelopeDetectorTest, OverLimitEnvelopeStaysTextAndNeverPromotesLaterEnd) {
  const auto input = std::string("*** Begin Patch\n") +
                     std::string(RawEnvelopeDetector::kMaxBufferedBytes, 'x') +
                     "\n*** End Patch";
  const auto result = Read({input});
  EXPECT_TRUE(result.calls.empty());
  EXPECT_EQ(result.text, input);
  EXPECT_EQ(result.rejected_text,
            std::string("*** Begin Patch\n") +
                std::string(RawEnvelopeDetector::kMaxBufferedBytes, 'x') + "\n");
}

TEST(RawEnvelopeDetectorTest, IndentedAndSuffixedEndMarkersRemainPayloadUntilExactEnd) {
  const auto input = std::string("*** Begin Patch\n *** End Patch\n*** End Patch suffix\n") +
                     "*** End Patch";
  const auto result = Read({input});
  ASSERT_EQ(result.calls.size(), 1u);
  EXPECT_EQ(result.calls[0].arguments, input);
}

TEST(RawEnvelopeDetectorTest, CarriageReturnLineEndingsPreserveExactPayload) {
  const auto input = std::string("*** Begin Patch\r\n+x\r\n*** End Patch");
  const auto result = Read({input});
  ASSERT_EQ(result.calls.size(), 1u);
  EXPECT_EQ(result.calls[0].arguments, input);
}

TEST(RawEnvelopeDetectorTest, LoneCarriageReturnAfterEofMarkerRejectsCandidateVerbatim) {
  const auto input = kEnvelope + "\r";
  const auto result = Read({input});

  EXPECT_TRUE(result.calls.empty());
  EXPECT_EQ(result.text, input);
  EXPECT_EQ(result.rejected_text, input);
}

TEST(RawEnvelopeDetectorTest, ReasoningRejectionPreservesBacktickAndTildeFenceState) {
  for (const auto fence : {"```", "~~~"}) {
    RawEnvelopeDetector detector(Descriptor());
    Result result;
    const auto partial = std::string(fence) + "\n*** Begin Pa";
    const auto remainder =
        std::string("tch\npayload\n*** End Patch\n") + fence + "\n";

    Append(result, detector.Push(partial));
    Append(result, detector.RejectCandidateForReasoning());
    Append(result, detector.Push(remainder));
    Append(result, detector.FinalizeNatural());

    EXPECT_TRUE(result.calls.empty()) << fence;
    EXPECT_EQ(result.text, partial + remainder) << fence;
  }
}

TEST(RawEnvelopeDetectorTest, ReasoningRejectionPreservesOutsideLineContinuation) {
  RawEnvelopeDetector detector(Descriptor());
  Result result;

  Append(result, detector.Push("*** Begin Pa"));
  Append(result, detector.RejectCandidateForReasoning());
  Append(result, detector.Push("tch\n"));
  EXPECT_TRUE(result.calls.empty());

  Append(result, detector.Push(kEnvelope));
  Append(result, detector.FinalizeNatural());

  ASSERT_EQ(result.calls.size(), 1u);
  EXPECT_EQ(result.text, "*** Begin Patch\n");
  EXPECT_EQ(result.calls.front().arguments, kEnvelope);
}

TEST(RawEnvelopeActivationTest, ExplicitDescriptorSupportsAutoAndMatchingForcedCustomTool) {
  ToolCallContext context;
  context.tool_output = true;
  context.text_output = true;
  context.tool_kinds = {{"edit", ToolKind::kCustom}};
  context.raw_envelope = RawEnvelopeDescriptor{"edit", "BEGIN", "END"};

  ASSERT_NE(context.ActiveRawEnvelope(), nullptr);
  EXPECT_EQ(context.ActiveRawEnvelope()->tool_name, "edit");

  context.forced_tool = ForcedToolChoice{"other", ToolKind::kCustom};
  EXPECT_EQ(context.ActiveRawEnvelope(), nullptr);

  context.forced_tool = ForcedToolChoice{"edit", ToolKind::kFunction};
  EXPECT_EQ(context.ActiveRawEnvelope(), nullptr);

  context.forced_tool = ForcedToolChoice{"edit", ToolKind::kCustom};
  ASSERT_NE(context.ActiveRawEnvelope(), nullptr);
  EXPECT_EQ(context.ActiveRawEnvelope()->start_marker, "BEGIN");
}

TEST(RawEnvelopeActivationTest, NoneRequiredAndFilteredToolStayInactive) {
  ToolCallContext context;
  context.raw_envelope = RawEnvelopeDescriptor{"edit", "BEGIN", "END"};
  context.forced_tool = ForcedToolChoice{"edit", ToolKind::kCustom};

  context.tool_output = false;
  context.tool_kinds = {{"edit", ToolKind::kCustom}};
  EXPECT_EQ(context.ActiveRawEnvelope(), nullptr);

  context.tool_output = true;
  context.forced_tool.reset();
  context.text_output = false;
  EXPECT_EQ(context.ActiveRawEnvelope(), nullptr);

  context.forced_tool = ForcedToolChoice{"edit", ToolKind::kCustom};
  context.tool_kinds.clear();
  EXPECT_EQ(context.ActiveRawEnvelope(), nullptr);
}

TEST(RawEnvelopeActivationTest, BuiltInApplyPatchIsActiveForAutoButNotUnnamedRequired) {
  ToolCallContext context;
  context.tool_output = true;
  context.text_output = true;
  context.tool_kinds = {{"apply_patch", ToolKind::kCustom}};
  context.raw_envelope = Descriptor();
  context.built_in_raw_envelope = true;

  ASSERT_NE(context.ActiveRawEnvelope(), nullptr);

  context.text_output = false;
  EXPECT_EQ(context.ActiveRawEnvelope(), nullptr);
}

TEST(RawEnvelopeActivationTest, GenericExplicitDescriptorFlowsThroughGuidanceAndRecognition) {
  ToolCallContext context;
  context.tool_output = true;
  context.text_output = true;
  context.tool_kinds = {{"edit", ToolKind::kCustom}};
  context.raw_envelope = RawEnvelopeDescriptor{"edit", "BEGIN", "END"};

  EXPECT_FALSE(ResolveTurnGuidanceOptions(context, false).has_value());
  ASSERT_NE(context.ActiveRawEnvelope(), nullptr);

  RawEnvelopeDetector detector(*context.ActiveRawEnvelope());
  Result result;
  Append(result, detector.Push("ordinary text\nBEGIN\npayload\nEND"));
  Append(result, detector.FinalizeNatural());

  ASSERT_EQ(result.calls.size(), 1u);
  EXPECT_EQ(result.text, "ordinary text\n");
  EXPECT_EQ(result.calls.front().name, "edit");
  EXPECT_EQ(result.calls.front().arguments, "BEGIN\npayload\nEND");
}

TEST(RawEnvelopeDetectorTest, AbortRejectsCompleteMarkerAtEof) {
  RawEnvelopeDetector detector(Descriptor());
  Result result;

  Append(result, detector.Push(kEnvelope));
  Append(result, detector.Abort());

  EXPECT_TRUE(result.calls.empty());
  EXPECT_EQ(result.text, kEnvelope);
  EXPECT_EQ(result.rejected_text, kEnvelope);
}

TEST(RawEnvelopeDetectorTest, ExactLimitClosingMarkerIsIndependentOfEofLfCrLfAndMarkerSplit) {
  const std::string start = "*** Begin Patch\n";
  const std::string end = "*** End Patch";
  const auto payload_size =
      RawEnvelopeDetector::kMaxBufferedBytes - start.size() - end.size();
  const std::string prefix = start + std::string(payload_size - 1, 'x') + "\n";

  for (const auto& terminator : {"", "\n", "\r\n"}) {
    const std::string input = prefix + end + terminator;
    for (size_t split = 0; split <= end.size() + std::string_view(terminator).size(); ++split) {
      const auto boundary = prefix.size() + split;
      const auto result = Read({input.substr(0, boundary), input.substr(boundary)});
      ASSERT_EQ(result.calls.size(), 1u) << "terminator=" << std::quoted(terminator)
                                         << " split=" << split;
      EXPECT_EQ(result.calls.front().arguments, prefix + end);
      EXPECT_EQ(result.text, terminator);
    }
  }
}
