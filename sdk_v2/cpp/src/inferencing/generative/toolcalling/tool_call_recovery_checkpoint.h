// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/prepared_chat_prompt.h"

#include <optional>
#include <string_view>
#include <utility>

namespace fl {

/// Recovery replaces only the uncommitted tool suffix, retaining the exact tokens behind already streamed content.
class ToolCallRecoveryCheckpoint {
 public:
  ToolCallRecoveryCheckpoint(const PreparedChatPrompt& prompt, std::optional<int32_t> opening_token, bool enabled)
      : opening_token_(opening_token), enabled_(enabled && opening_token.has_value()) {
    if (enabled_) {
      prompt_.prompt = prompt.prompt;
      prompt_.token_ids = prompt.token_ids;
    }
  }

  void Observe(std::optional<int32_t> token, std::string_view decoded, bool inside_reasoning) {
    if (!enabled_ || captured_) {
      return;
    }
    if (!token) {
      enabled_ = false;
      return;
    }
    if (!inside_reasoning && token == opening_token_) {
      captured_ = true;
      return;
    }

    prompt_.token_ids.push_back(*token);
    prompt_.prompt.append(decoded);
  }

  bool HasBoundary() const noexcept {
    return enabled_ && captured_;
  }

  PreparedChatPrompt TakePrompt() {
    prompt_.prompt_token_count = static_cast<int64_t>(prompt_.token_ids.size());
    return std::move(prompt_);
  }

 private:
  PreparedChatPrompt prompt_;
  std::optional<int32_t> opening_token_;
  bool enabled_ = false;
  bool captured_ = false;
};

}  // namespace fl
