// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/toolcalling/tool_call_recovery_checkpoint.h"

#include <gtest/gtest.h>

namespace fl {
namespace {

PreparedChatPrompt InitialPrompt() {
  PreparedChatPrompt prompt;
  prompt.prompt = "original prompt";
  prompt.token_ids = {1, 2, 3};
  prompt.prompt_token_count = 3;
  return prompt;
}

TEST(ToolCallRecoveryCheckpointTest, PreservesExactPublishedPrefixTokensAndExcludesRejectedSuffix) {
  auto prompt = InitialPrompt();
  ToolCallRecoveryCheckpoint checkpoint(prompt, 99, true);
  checkpoint.Observe(41, "thinking", true);
  checkpoint.Observe(42, "</think>", true);
  checkpoint.Observe(43, "visible", false);
  checkpoint.Observe(99, "<tool_call>", false);
  checkpoint.Observe(44, "rejected arguments", false);
  ASSERT_TRUE(checkpoint.HasBoundary());

  const auto retry = checkpoint.TakePrompt();
  EXPECT_EQ(retry.token_ids, (std::vector<int32_t>{1, 2, 3, 41, 42, 43}));
  EXPECT_EQ(retry.prompt, "original promptthinking</think>visible");
  EXPECT_EQ(retry.prompt_token_count, 6);
}

TEST(ToolCallRecoveryCheckpointTest, ToolMarkerInsideReasoningIsPartOfRetainedPrefix) {
  auto prompt = InitialPrompt();
  ToolCallRecoveryCheckpoint checkpoint(prompt, 99, true);
  checkpoint.Observe(99, "<tool_call>", true);
  EXPECT_FALSE(checkpoint.HasBoundary());
  checkpoint.Observe(42, "</think>", true);
  checkpoint.Observe(99, "<tool_call>", false);
  ASSERT_TRUE(checkpoint.HasBoundary());
  EXPECT_EQ(checkpoint.TakePrompt().token_ids, (std::vector<int32_t>{1, 2, 3, 99, 42}));
}

TEST(ToolCallRecoveryCheckpointTest, MissingTokenIdentityDisablesRecoveryRatherThanRetokenizingStreamedText) {
  auto prompt = InitialPrompt();
  ToolCallRecoveryCheckpoint checkpoint(prompt, 99, true);
  checkpoint.Observe(std::nullopt, "already streamed text", false);
  checkpoint.Observe(99, "<tool_call>", false);
  EXPECT_FALSE(checkpoint.HasBoundary());
}

TEST(ToolCallRecoveryCheckpointTest, MissingBoundaryAndDisabledRequestsCannotProduceARecoveryCheckpoint) {
  auto prompt = InitialPrompt();
  ToolCallRecoveryCheckpoint disabled(prompt, 99, false);
  disabled.Observe(99, "<tool_call>", false);
  EXPECT_FALSE(disabled.HasBoundary());

  ToolCallRecoveryCheckpoint unknown_marker(prompt, std::nullopt, true);
  unknown_marker.Observe(99, "<tool_call>", false);
  EXPECT_FALSE(unknown_marker.HasBoundary());

  ToolCallRecoveryCheckpoint text_only(prompt, 99, true);
  text_only.Observe(43, "ordinary final answer", false);
  EXPECT_FALSE(text_only.HasBoundary());
}

}  // namespace
}  // namespace fl
