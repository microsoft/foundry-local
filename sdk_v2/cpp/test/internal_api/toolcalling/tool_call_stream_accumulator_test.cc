// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for ToolCallStreamAccumulator — the streaming state machine that separates visible assistant text from
// buffered tool-call blocks. These tests pin down the cross-token marker buffering, parse-on-close semantics, and
// EOS draining that the chat streaming paths rely on.
//
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace fl;

namespace {

// Concatenate text events from a sequence of Push results.
std::string CollectVisible(const std::vector<ToolCallStreamAccumulator::Output>& outs) {
  std::string s;
  for (const auto& o : outs) {
    for (const auto& event : o.events) {
      if (const auto* text = std::get_if<std::string>(&event)) {
        s += *text;
      }
    }
  }
  return s;
}

std::vector<ParsedToolCall> CollectCalls(std::vector<ToolCallStreamAccumulator::Output>& outs) {
  std::vector<ParsedToolCall> calls;
  for (auto& output : outs) {
    for (auto& event : output.events) {
      if (auto* call = std::get_if<ParsedToolCall>(&event)) {
        calls.push_back(std::move(*call));
      }
    }
  }
  return calls;
}

// Run a sequence of chunks through the accumulator, calling Flush at the end. Returns one Output per chunk plus
// the Flush Output appended last.
std::vector<ToolCallStreamAccumulator::Output> RunChunks(ToolCallStreamAccumulator& acc,
                                                         const std::vector<std::string>& chunks) {
  std::vector<ToolCallStreamAccumulator::Output> outs;
  outs.reserve(chunks.size() + 1);
  for (const auto& c : chunks) {
    outs.push_back(acc.Push(c));
  }
  outs.push_back(acc.Flush());
  return outs;
}

}  // namespace

// ========================================================================
// Passthrough mode — no markers configured.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, EmptyMarkersIsPassthrough) {
  ToolCallStreamAccumulator acc("", "");
  auto out = acc.Push("any text including <tool_call> markers");
  ASSERT_EQ(out.events.size(), 1u);
  EXPECT_EQ(std::get<std::string>(out.events[0]), "any text including <tool_call> markers");
  EXPECT_FALSE(acc.InsideToolCall());
}

TEST(ToolCallStreamAccumulatorTest, EmptyChunkProducesNothing) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  auto out = acc.Push("");
  EXPECT_TRUE(out.events.empty());
}

// ========================================================================
// No tool calls — text-only path.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, PlainTextPassesThroughVerbatim) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  auto outs = RunChunks(acc, {"Hello", ", ", "world!"});
  EXPECT_EQ(CollectVisible(outs), "Hello, world!");
  for (const auto& o : outs) {
    EXPECT_TRUE(std::none_of(o.events.begin(), o.events.end(), [](const auto& event) {
      return std::holds_alternative<ParsedToolCall>(event);
    }));
  }
}

// ========================================================================
// Single tool call, varying chunking strategies.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, SingleToolCallInOneChunk) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  std::string chunk =
      R"(prefix <tool_call>[{"name":"add","arguments":{"a":1,"b":2}}]</tool_call> suffix)";
  auto outs = RunChunks(acc, {chunk});

  EXPECT_EQ(CollectVisible(outs), "prefix  suffix");
  ASSERT_EQ(outs[0].events.size(), 3u);
  EXPECT_EQ(std::get<std::string>(outs[0].events[0]), "prefix ");
  EXPECT_EQ(std::get<ParsedToolCall>(outs[0].events[1]).name, "add");
  EXPECT_EQ(std::get<std::string>(outs[0].events[2]), " suffix");
}

TEST(ToolCallStreamAccumulatorTest, SingleToolCallSplitAcrossManyChunks) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");

  // Deliberately split markers and JSON across tiny pieces — exactly what real tokenizers do.
  std::vector<std::string> chunks = {
      "before ",
      "<tool",
      "_call",
      ">",
      "[{\"name\":\"mul\",\"arg",
      "uments\":{\"x\":7,\"y\":6}}]",
      "</tool",
      "_call>",
      " after",
  };
  auto outs = RunChunks(acc, chunks);

  EXPECT_EQ(CollectVisible(outs), "before  after")
      << "Marker and JSON bytes must not leak into visible text";

  auto all = CollectCalls(outs);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].name, "mul");
  EXPECT_NE(all[0].arguments.find("7"), std::string::npos);
  EXPECT_NE(all[0].arguments.find("6"), std::string::npos);
}

TEST(ToolCallStreamAccumulatorTest, MarkerByteByByte) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");

  std::string full =
      R"(<tool_call>{"name":"f","arguments":{}}</tool_call>)";
  std::vector<std::string> chunks;
  chunks.reserve(full.size());
  for (char c : full) {
    chunks.emplace_back(1, c);
  }
  auto outs = RunChunks(acc, chunks);

  EXPECT_TRUE(CollectVisible(outs).empty()) << "Single tool-call block with no surrounding text produces no visible";

  auto all = CollectCalls(outs);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].name, "f");
}

// ========================================================================
// Multiple tool calls — sequential blocks, with interspersed visible text.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, TwoSequentialToolCalls) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  std::string chunk =
      R"(<tool_call>{"name":"a","arguments":{}}</tool_call> middle )"
      R"(<tool_call>{"name":"b","arguments":{}}</tool_call>)";
  auto outs = RunChunks(acc, {chunk});

  EXPECT_EQ(CollectVisible(outs), " middle ");

  auto all = CollectCalls(outs);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].name, "a");
  EXPECT_EQ(all[1].name, "b");
}

// ========================================================================
// EOS draining behavior.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, FlushDrainsPendingSuffixWhenOutside) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  // The trailing "<too" looks like a potential start-marker prefix and gets held back across Push calls; Flush
  // must surface it as visible text since no real marker arrives.
  auto outs = RunChunks(acc, {"hello <too"});
  EXPECT_EQ(CollectVisible(outs), "hello <too");
}

TEST(ToolCallStreamAccumulatorTest, UnterminatedToolCallBecomesVisibleOnFlush) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  // Opens a tool-call block but stream ends before the closing marker. Buffered bytes should surface as visible
  // text on Flush so the caller still sees what the model produced.
  auto outs = RunChunks(acc, {"prefix <tool_call>{\"name\":\"truncated"});

  std::string visible = CollectVisible(outs);
  EXPECT_NE(visible.find("prefix "), std::string::npos);
  EXPECT_NE(visible.find("<tool_call>"), std::string::npos);
  EXPECT_NE(visible.find("truncated"), std::string::npos);

  // No tool call should have been emitted — the block never closed.
  for (const auto& o : outs) {
    EXPECT_TRUE(std::none_of(o.events.begin(), o.events.end(), [](const auto& event) {
      return std::holds_alternative<ParsedToolCall>(event);
    }));
  }

  EXPECT_FALSE(acc.InsideToolCall()) << "Flush should leave accumulator in outside state";
}

TEST(ToolCallStreamAccumulatorTest, CompletedMalformedToolCallBecomesVisible) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  auto outs = RunChunks(acc, {"before <tool_call>not json</tool_call> after"});

  EXPECT_EQ(CollectVisible(outs), "before <tool_call>not json</tool_call> after");
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, CompletedToolCallWithoutNameBecomesVisible) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  const std::string generated = R"(<tool_call>{"arguments":{"value":1}}</tool_call>)";
  auto outs = RunChunks(acc, {generated});

  EXPECT_EQ(CollectVisible(outs), generated);
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, CompletedToolCallWithNonStringNameBecomesVisible) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  const std::string generated = R"(<tool_call>{"name":123,"arguments":{}}</tool_call>)";
  auto outs = RunChunks(acc, {generated});

  EXPECT_EQ(CollectVisible(outs), generated);
  EXPECT_TRUE(CollectCalls(outs).empty());
}

TEST(ToolCallStreamAccumulatorTest, CompletedMixedValidAndInvalidArrayBecomesVisible) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");
  const std::string generated =
      R"(<tool_call>[{"name":"fn1","arguments":{}},{"name":123}]</tool_call>)";
  auto outs = RunChunks(acc, {"before ", generated, " after"});

  EXPECT_EQ(CollectVisible(outs), "before " + generated + " after")
      << "A partially-invalid block must be preserved whole, not partially parsed";
  EXPECT_TRUE(CollectCalls(outs).empty());
}

// ========================================================================
// InsideToolCall state machine.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, InsideToolCallTransitions) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");

  EXPECT_FALSE(acc.InsideToolCall());

  acc.Push("text ");
  EXPECT_FALSE(acc.InsideToolCall());

  acc.Push("<tool_call>{\"name\":\"f\"");
  EXPECT_TRUE(acc.InsideToolCall());

  acc.Push(",\"arguments\":{}}</tool_call>");
  EXPECT_FALSE(acc.InsideToolCall());
}

// ========================================================================
// Pathological prefix that does not become a marker — must not eat user text.
// ========================================================================

TEST(ToolCallStreamAccumulatorTest, FalseStartPrefixReleasesAfterDisambiguation) {
  ToolCallStreamAccumulator acc("<tool_call>", "</tool_call>");

  // First chunk ends with "<tool" — a real prefix of the start marker. The accumulator must hold it back.
  auto out1 = acc.Push("hello <tool");
  ASSERT_EQ(out1.events.size(), 1u);
  EXPECT_EQ(std::get<std::string>(out1.events[0]), "hello ");

  // Next chunk reveals the prefix was actually part of unrelated XML-ish text. The held-back "<tool" plus the new
  // bytes must flow out as visible.
  auto out2 = acc.Push("box>");
  ASSERT_EQ(out2.events.size(), 1u);
  EXPECT_EQ(std::get<std::string>(out2.events[0]), "<toolbox>");
}
