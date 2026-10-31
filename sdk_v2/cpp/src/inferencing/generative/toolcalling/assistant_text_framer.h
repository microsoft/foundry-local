// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "exception.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"

#include <string>
#include <string_view>

namespace fl {

class AssistantTextFramer {
 public:
  explicit AssistantTextFramer(const ToolCallContext& context)
      : enabled_(context.UsesAssistantTextFraming()),
        reasoning_separator_pending_(enabled_ && context.supports_reasoning) {}

  std::string Push(std::string_view text) {
    if (!enabled_ || content_started_) {
      content_seen_ |= !text.empty();
      return std::string(text);
    }
    if (text.empty()) {
      return {};
    }

    std::string visible;
    if (reasoning_separator_pending_) {
      reasoning_separator_pending_ = false;
      if (text.front() == '\n') {
        visible += '\n';
        text.remove_prefix(1);
      }
    }

    while (!text.empty() && prefix_offset_ < ToolCallContext::kAssistantTextPrefix.size()) {
      if (text.front() != ToolCallContext::kAssistantTextPrefix[prefix_offset_]) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Model emitted unframed assistant text in a structured tool turn");
      }

      ++prefix_offset_;
      text.remove_prefix(1);
    }

    content_started_ = prefix_offset_ == ToolCallContext::kAssistantTextPrefix.size();
    content_seen_ |= !text.empty();
    visible.append(text);
    return visible;
  }

  void ValidateBoundary() const {
    if (enabled_ && prefix_offset_ != 0 && (!content_started_ || !content_seen_)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Model interrupted or left empty an assistant text frame");
    }
  }

  void ValidateCompletion(bool has_tool_calls) const {
    ValidateBoundary();
    if (enabled_ && !has_tool_calls && !content_started_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Model completed a structured tool turn without choosing a response mode");
    }
  }

 private:
  bool enabled_;
  bool reasoning_separator_pending_;
  bool content_started_ = false;
  bool content_seen_ = false;
  size_t prefix_offset_ = 0;
};

}  // namespace fl
