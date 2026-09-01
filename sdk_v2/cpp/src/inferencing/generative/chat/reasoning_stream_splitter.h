// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "foundry_local/foundry_local_c.h"

#include <string>
#include <utility>
#include <vector>

namespace fl {

/// Token-level state machine that splits a stream of generated text chunks around reasoning markers
/// (e.g. `<think>` / `</think>`) into typed segments.
///
/// The streaming code calls `Push(token)` for every decoded token and forwards the returned segments to the
/// caller as typed `TextItem`s. At end-of-generation, `Flush()` drains any buffered bytes (e.g. a trailing
/// partial marker that turned out not to be one).
///
/// Why a state machine: the marker can straddle multiple tokens (a tokenizer might split `</think>` into
/// `</`, `think`, `>`). We must not emit the partial prefix as visible text and then realize on the next
/// token that it was actually a marker. The buffer holds the suffix that could still grow into the marker.
///
/// When `start_marker` is empty, the splitter degrades to a passthrough that always emits DEFAULT segments —
/// non-reasoning models share this code without a behavior change.
class ReasoningStreamSplitter {
 public:
  struct Segment {
    std::string text;
    flTextItemType type;
  };

  /// @param start_inside_reasoning True when the chat template pre-fills the reasoning
  ///        open marker (e.g. `<think>\n`) before the model's first token. In that case
  ///        the model's first byte is reasoning content and the splitter must start in
  ///        the reasoning state instead of waiting for a start marker that will never
  ///        appear.
  ReasoningStreamSplitter(std::string start_marker, std::string end_marker,
                          bool start_inside_reasoning = false)
      : start_marker_(std::move(start_marker)),
        end_marker_(std::move(end_marker)),
        inside_reasoning_(start_inside_reasoning) {}

  /// Feed a token into the splitter. Returns zero or more segments to emit.
  std::vector<Segment> Push(const std::string& token) {
    std::vector<Segment> out;

    if (token.empty()) {
      return out;
    }

    if (start_marker_.empty()) {
      out.push_back({token, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT});
      return out;
    }

    buffer_ += token;
    Drain(out, /*flushing=*/false);

    return out;
  }

  /// Drain any remaining buffered bytes at end-of-stream. Buffered bytes that looked like a partial marker
  /// turn out not to be — emit them with the current type.
  std::vector<Segment> Flush() {
    std::vector<Segment> out;

    if (start_marker_.empty()) {
      return out;
    }

    Drain(out, /*flushing=*/true);

    return out;
  }

  /// Whether the splitter is currently inside a reasoning block. Used by callers that want to make
  /// downstream decisions (e.g. suppressing chunks) without inspecting segment types.
  bool InsideReasoning() const noexcept { return inside_reasoning_; }

 private:
  void Drain(std::vector<Segment>& out, bool flushing) {
    while (true) {
      const std::string& marker = inside_reasoning_ ? end_marker_ : start_marker_;
      flTextItemType current_type = inside_reasoning_ ? FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING
                                                      : FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT;

      // Marker may be empty (e.g. end_marker not configured). With no end marker we can never close a
      // reasoning block — drain the buffer with the current type and stop.
      if (marker.empty()) {
        EmitSegment(out, std::move(buffer_), current_type);
        buffer_.clear();
        return;
      }

      size_t found = buffer_.find(marker);

      if (found != std::string::npos) {
        // Emit prefix with current type, consume marker, flip state.
        EmitSegment(out, buffer_.substr(0, found), current_type);

        size_t after = found + marker.size();

        // Drop a single trailing newline immediately after the closing marker — matches the non-streaming
        // SplitReasoningContent behavior so callers see the same visible text either way.
        if (inside_reasoning_ && after < buffer_.size() && buffer_[after] == '\n') {
          ++after;
        }

        buffer_.erase(0, after);
        inside_reasoning_ = !inside_reasoning_;

        continue;  // re-scan the remaining buffer for the next marker
      }

      // No full marker. If we're flushing, emit everything and stop. Otherwise hold back the longest suffix
      // of buffer_ that could still grow into the marker.
      if (flushing) {
        EmitSegment(out, std::move(buffer_), current_type);
        buffer_.clear();
        return;
      }

      size_t hold = LongestSuffixThatIsPrefixOf(buffer_, marker);
      size_t safe = buffer_.size() - hold;

      if (safe > 0) {
        EmitSegment(out, buffer_.substr(0, safe), current_type);
        buffer_.erase(0, safe);
      }

      return;
    }
  }

  static void EmitSegment(std::vector<Segment>& out, std::string text, flTextItemType type) {
    if (text.empty()) {
      return;
    }

    out.push_back({std::move(text), type});
  }

  // Length of the longest suffix of `s` that is also a prefix of `m`. O(min(|s|, |m|)).
  static size_t LongestSuffixThatIsPrefixOf(const std::string& s, const std::string& m) {
    size_t max_len = std::min(s.size(), m.size());

    for (size_t k = max_len; k > 0; --k) {
      if (s.compare(s.size() - k, k, m, 0, k) == 0) {
        return k;
      }
    }

    return 0;
  }

  std::string start_marker_;
  std::string end_marker_;
  std::string buffer_;
  bool inside_reasoning_ = false;
};

}  // namespace fl
