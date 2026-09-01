// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_utils.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace fl {

/// Streaming state machine that separates visible assistant text from buffered tool-call blocks.
///
/// Generative chat models that support tool calling emit tool requests inline in the token stream, wrapped in
/// model-specific marker tokens (e.g. `<tool_call>...</tool_call>`). The streaming code must not forward those
/// markers — or the JSON payload between them — to callers as visible text; instead it must accumulate the payload
/// across tokens, parse it once the closing marker arrives, and surface the structured `ParsedToolCall`s.
///
/// `Push(chunk)` accepts any text chunk (a single decoded token, or a multi-token segment produced by the upstream
/// `ReasoningStreamSplitter`) and returns:
///   - `visible_text`: text that is safe to emit to the caller (everything outside a tool-call block, minus any
///     pending suffix that could still grow into the start marker).
///   - `ready_calls`: zero or more fully parsed tool calls whose closing marker arrived in this chunk.
///
/// Marker matching is buffered, mirroring `ReasoningStreamSplitter`: a marker can straddle multiple tokens, so the
/// accumulator holds back the longest suffix of its scan buffer that could still extend into the marker rather than
/// flushing it as visible text prematurely.
///
/// `Flush()` drains end-of-stream. If a `<tool_call>` block was opened but never closed, the buffered bytes are
/// returned as visible text — they turned out not to be a tool call, so the caller still sees what the model
/// produced. Matches `ReasoningStreamSplitter::Flush()`.
///
/// When either marker is empty, the accumulator degrades to a passthrough: `Push` returns its input verbatim as
/// `visible_text` with no `ready_calls`. This keeps the call site uniform for non-tool-calling models.
///
/// Callers must not feed REASONING-tagged content into `Push` — reasoning is the model's scratchpad and any
/// tool-call-shaped text inside `<think>...</think>` is not a real tool call. The upstream `ReasoningStreamSplitter`
/// already routes REASONING segments through a separate path; this accumulator sits below the DEFAULT-segment branch.
class ToolCallStreamAccumulator {
 public:
  struct Output {
    std::string visible_text;
    std::vector<ParsedToolCall> ready_calls;
  };

  ToolCallStreamAccumulator(std::string start_marker, std::string end_marker, std::string tools_json = {})
      : start_marker_(std::move(start_marker)),
        end_marker_(std::move(end_marker)),
        tools_json_(std::move(tools_json)) {}

  /// Feed a chunk into the accumulator. Returns visible text and any tool calls completed by this chunk.
  Output Push(const std::string& chunk) {
    Output out;

    if (chunk.empty()) {
      return out;
    }

    if (start_marker_.empty() || end_marker_.empty()) {
      // Passthrough mode — no tool-call detection.
      out.visible_text = chunk;
      return out;
    }

    buffer_ += chunk;
    Drain(out, /*flushing=*/false);

    return out;
  }

  /// Drain at end-of-stream. A complete or narrowly repairable JSON call is recovered even if the model omitted the
  /// closing marker; genuinely truncated blocks become visible text.
  Output Flush() {
    Output out;

    if (start_marker_.empty() || end_marker_.empty()) {
      return out;
    }

    Drain(out, /*flushing=*/true);

    return out;
  }

  /// Whether the accumulator is currently inside a `<tool_call>...</tool_call>` block (between start and end markers).
  bool InsideToolCall() const noexcept { return inside_tool_call_; }

 private:
  void Drain(Output& out, bool flushing) {
    while (true) {
      const std::string& marker = inside_tool_call_ ? end_marker_ : start_marker_;

      size_t found = buffer_.find(marker);

      if (found != std::string::npos) {
        if (inside_tool_call_) {
          // Closing marker: take everything up to and including the marker, parse it as one tool-call block,
          // and emit any tool calls it contained.
          tool_call_buffer_ += buffer_.substr(0, found + marker.size());
          buffer_.erase(0, found + marker.size());

          auto parsed = ParseToolCalls(tool_call_buffer_, start_marker_, end_marker_, tools_json_);
          for (auto& pc : parsed) {
            out.ready_calls.push_back(std::move(pc));
          }

          tool_call_buffer_.clear();
          inside_tool_call_ = false;
        } else {
          // Opening marker: emit prefix as visible text, then start buffering the tool-call block (including the
          // marker — ParseToolCalls expects the full `<tool_call>...</tool_call>` substring).
          if (found > 0) {
            out.visible_text.append(buffer_, 0, found);
          }
          tool_call_buffer_ = buffer_.substr(found, marker.size());
          buffer_.erase(0, found + marker.size());
          inside_tool_call_ = true;
        }

        continue;  // re-scan the remaining buffer for the next marker
      }

      // No full marker.
      if (flushing) {
        if (inside_tool_call_) {
          std::string incomplete = tool_call_buffer_ + buffer_;
          std::string candidate = incomplete;
          if (const size_t stray_reasoning_end = candidate.find("</think>");
              stray_reasoning_end != std::string::npos) {
            candidate.erase(stray_reasoning_end);
          }
          candidate += end_marker_;
          auto parsed = ParseToolCalls(candidate, start_marker_, end_marker_, tools_json_);
          if (parsed.empty()) {
            out.visible_text += incomplete;
          } else {
            for (auto& pc : parsed) {
              out.ready_calls.push_back(std::move(pc));
            }
          }
          tool_call_buffer_.clear();
          inside_tool_call_ = false;
        } else {
          out.visible_text.append(buffer_);
        }
        buffer_.clear();
        return;
      }

      if (inside_tool_call_) {
        // Inside a tool-call block: every byte belongs to the block. Append to the tool-call buffer and hold back
        // only the longest suffix that could still grow into the end marker.
        size_t hold = LongestSuffixThatIsPrefixOf(buffer_, marker);
        size_t safe = buffer_.size() - hold;

        if (safe > 0) {
          tool_call_buffer_.append(buffer_, 0, safe);
          buffer_.erase(0, safe);
        }
      } else {
        // Outside: emit visible text, but hold back the longest suffix that could still grow into the start marker.
        size_t hold = LongestSuffixThatIsPrefixOf(buffer_, marker);
        size_t safe = buffer_.size() - hold;

        if (safe > 0) {
          out.visible_text.append(buffer_, 0, safe);
          buffer_.erase(0, safe);
        }
      }

      return;
    }
  }

  // Length of the longest suffix of `s` that is also a prefix of `m`. O(min(|s|, |m|)).
  // Identical to ReasoningStreamSplitter's helper — kept private so each splitter stays self-contained.
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
  std::string tools_json_;
  std::string buffer_;            // pending bytes from Push() that haven't yet been routed
  std::string tool_call_buffer_;  // accumulated bytes of the in-progress tool-call block (incl. start marker)
  bool inside_tool_call_ = false;
};

}  // namespace fl
