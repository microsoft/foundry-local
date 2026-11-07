// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/harmony_protocol.h"
#include "inferencing/generative/chat/prepared_chat_prompt.h"
#include "inferencing/generative/chat/reasoning_stream_splitter.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace fl {

/// Decodes native message structure above payload admission. No session, retry policy, or publication ownership.
class HarmonyDecoder {
 public:
  using Event = std::variant<ReasoningStreamSplitter::Segment, ParsedToolCall>;
  struct Output {
    std::vector<Event> events;
    bool malformed = false;
    std::string error;
  };

  HarmonyDecoder(HarmonyProtocol protocol, std::shared_ptr<const ToolCallPlan> plan,
                 const PreparedChatPrompt& prompt,
                 std::unordered_set<std::string> raw_tool_names = {});
  Output Push(std::optional<int32_t> token, const std::string& text);
  Output Finish(std::optional<int32_t> terminal_token, bool natural_end);
  bool HasRecoveryCheckpoint() const noexcept { return checkpoint_captured_; }
  PreparedChatPrompt TakeRecoveryPrompt();
  int ReasoningTokenCount() const noexcept { return reasoning_tokens_; }

 private:
  enum class Phase { kHeader,
                     kBody,
                     kBetween,
                     kDone };
  void ParseHeader();
  void RecordToken(int32_t token, const std::string& text);
  void CaptureCheckpoint();
  Output Fail(std::string error);

  const HarmonyProtocol protocol_;
  const std::shared_ptr<const ToolCallPlan> plan_;
  const std::unordered_set<std::string> raw_tool_names_;
  Phase phase_ = Phase::kHeader;
  std::string header_;
  std::string channel_;
  std::string recipient_;
  std::string arguments_;
  bool visible_started_ = false;
  bool failed_ = false;
  bool checkpoint_captured_ = false;
  size_t message_token_offset_ = 0;
  size_t message_byte_offset_ = 0;
  PreparedChatPrompt checkpoint_;
  std::optional<int32_t> ending_;
  int reasoning_tokens_ = 0;
};

template <typename SegmentSink, typename ToolSink>
void DeliverHarmonyOutput(HarmonyDecoder::Output output, SegmentSink&& segments, ToolSink&& tools) {
  for (auto& event : output.events) {
    if (auto* segment = std::get_if<ReasoningStreamSplitter::Segment>(&event)) {
      segments(std::vector<ReasoningStreamSplitter::Segment>{std::move(*segment)});
    } else {
      ToolCallStreamAccumulator::Output admitted;
      admitted.events.emplace_back(std::move(std::get<ParsedToolCall>(event)));
      tools(std::move(admitted));
    }
  }
  if (output.malformed) {
    ToolCallStreamAccumulator::Output rejected;
    rejected.malformed = true;
    rejected.error = std::move(output.error);
    tools(std::move(rejected));
  }
}

}  // namespace fl
