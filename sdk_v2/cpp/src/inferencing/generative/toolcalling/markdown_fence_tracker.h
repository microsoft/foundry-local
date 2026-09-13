// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <string_view>

namespace fl {

/// Tracks CommonMark-style fenced code blocks in an incrementally delivered text stream.
class MarkdownFenceTracker {
 public:
  void Push(std::string_view text) {
    for (const auto byte : text) {
      PushByte(byte);
    }
  }

  bool InsideFence() const noexcept { return fence_marker_ != '\0'; }

 private:
  void PushByte(char byte) {
    if (byte == '\n') {
      FinishLine();
      return;
    }

    if (line_disqualified_) {
      return;
    }

    if (line_indentation_ < 3 && marker_ == '\0' && byte == ' ') {
      ++line_indentation_;
      return;
    }

    if (marker_ == '\0') {
      if (byte != '`' && byte != '~') {
        line_disqualified_ = true;
        return;
      }

      marker_ = byte;
      marker_count_ = 1;
      return;
    }

    if (!marker_ended_ && byte == marker_) {
      ++marker_count_;
      if (!InsideFence() && marker_count_ == 3) {
        OpenFence();
      } else if (opening_line_ && marker_ == fence_marker_) {
        fence_length_ = marker_count_;
      }

      return;
    }

    marker_ended_ = true;
    suffix_is_whitespace_ = suffix_is_whitespace_ && (byte == ' ' || byte == '\t' || byte == '\r');

    if (!InsideFence() && marker_count_ >= 3) {
      OpenFence();
    }
  }

  void FinishLine() {
    if (!line_disqualified_ && marker_count_ >= 3) {
      if (!InsideFence()) {
        OpenFence();
      } else if (!opening_line_ && marker_ == fence_marker_ &&
                 marker_count_ >= fence_length_ && suffix_is_whitespace_) {
        fence_marker_ = '\0';
        fence_length_ = 0;
      }
    }

    opening_line_ = false;
    marker_ = '\0';
    marker_count_ = 0;
    line_indentation_ = 0;
    marker_ended_ = false;
    suffix_is_whitespace_ = true;
    line_disqualified_ = false;
  }

  void OpenFence() {
    fence_marker_ = marker_;
    fence_length_ = marker_count_;
    opening_line_ = true;
  }

  char fence_marker_ = '\0';
  size_t fence_length_ = 0;
  char marker_ = '\0';
  size_t marker_count_ = 0;
  size_t line_indentation_ = 0;
  bool opening_line_ = false;
  bool marker_ended_ = false;
  bool suffix_is_whitespace_ = true;
  bool line_disqualified_ = false;
};

}  // namespace fl
