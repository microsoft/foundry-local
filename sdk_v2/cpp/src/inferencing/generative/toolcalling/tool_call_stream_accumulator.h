// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/markdown_fence_tracker.h"
#include "inferencing/generative/toolcalling/tool_call_payload_parser.h"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
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
/// `ReasoningStreamSplitter`) and returns ordered events containing:
///   - text that is safe to emit to the caller (everything outside a tool-call block, minus any pending suffix that
///     could still grow into the start marker);
///   - fully parsed tool calls whose closing marker arrived in this chunk.
///
/// Marker matching is buffered, mirroring `ReasoningStreamSplitter`: a marker can straddle multiple tokens, so the
/// accumulator holds back the longest suffix of its scan buffer that could still extend into the marker rather than
/// flushing it as visible text prematurely.
///
/// `Flush()` drains end-of-stream using the selected parser's normal finalization behavior.
///
/// When either marker is empty, the accumulator degrades to a passthrough and returns its input as a text event.
/// This keeps the call site uniform for non-tool-calling models.
///
/// Callers must not feed REASONING-tagged content into `Push` — reasoning is the model's scratchpad and any
/// tool-call-shaped text inside `<think>...</think>` is not a real tool call. The upstream `ReasoningStreamSplitter`
/// already routes REASONING segments through a separate path; this accumulator sits below the DEFAULT-segment branch.
class ToolCallStreamAccumulator {
 public:
  using Event = std::variant<std::string, ParsedToolCall>;

  struct Output {
    std::vector<Event> events;
  };

  ToolCallStreamAccumulator(std::string start_marker, std::string end_marker,
                            std::string tools_json = {},
                            std::string reasoning_end_marker = {},
                            ToolCallPayloadParser payload_parser = {})
      : start_marker_(std::move(start_marker)),
        end_marker_(std::move(end_marker)),
        tools_json_(std::move(tools_json)),
        reasoning_end_marker_(std::move(reasoning_end_marker)),
        payload_parser_(std::move(payload_parser)) {}

  /// Feed a chunk into the accumulator. Returns ordered visible-text and completed-tool-call events.
  Output Push(const std::string& chunk) {
    Output out;

    if (chunk.empty()) {
      return out;
    }

    if (start_marker_.empty() || end_marker_.empty()) {
      // Passthrough mode — no tool-call detection.
      EmitVisible(out, chunk);
      return out;
    }

    if (payload_parser_) {
      size_t offset = 0;
      while (offset < chunk.size()) {
        const auto count = std::min(kSelectedPayloadBufferLimit, chunk.size() - offset);
        buffer_.append(chunk, offset, count);
        offset += count;
        DrainSelectedPayload(out, /*flushing=*/false);
      }
    } else {
      buffer_ += chunk;
      Drain(out, /*flushing=*/false);
    }

    return out;
  }

  /// Drain at end-of-stream.
  Output Flush() {
    Output out;

    if (start_marker_.empty() || end_marker_.empty()) {
      return out;
    }

    if (payload_parser_) {
      DrainSelectedPayload(out, /*flushing=*/true);
    } else {
      Drain(out, /*flushing=*/true);
    }

    return out;
  }

  /// Reject a pending request-selected payload without parsing it. This is intentionally separate from `Flush()`:
  /// default JSON recovery keeps its established terminal semantics, while an interrupted selected batch is ambiguous.
  Output RejectPendingSelectedPayload() {
    Output out;
    if (!payload_parser_) {
      return out;
    }

    EmitVisible(out, std::move(tool_call_buffer_));
    EmitVisible(out, std::move(buffer_));
    tool_call_buffer_.clear();
    buffer_.clear();
    inside_tool_call_ = false;
    rejected_batch_state_ = RejectedBatchState::kNone;
    ResetPayloadScan();
    return out;
  }

  bool HasPayloadParser() const noexcept { return static_cast<bool>(payload_parser_); }

  /// Whether the accumulator is currently inside a `<tool_call>...</tool_call>` block (between start and end markers).
  bool InsideToolCall() const noexcept { return inside_tool_call_; }

 private:
  static constexpr size_t kSelectedPayloadBufferLimit = 64 * 1024;

  enum class MarkerKind { kNone,
                          kNestedStart,
                          kEnd };

  enum class RejectedBatchState {
    kNone,
    kCandidate,
    kBetweenCandidates,
  };

  struct MarkerMatch {
    MarkerKind kind = MarkerKind::kNone;
    size_t position = std::string::npos;
  };

  void EmitVisible(Output& out, std::string text) {
    if (text.empty()) {
      return;
    }

    if (payload_parser_) {
      markdown_fence_tracker_.Push(text);
    }

    if (!out.events.empty()) {
      if (auto* previous = std::get_if<std::string>(&out.events.back())) {
        *previous += text;
        return;
      }
    }
    out.events.emplace_back(std::move(text));
  }

  void EmitParsedCalls(Output& out, std::vector<ParsedToolCall> calls) {
    for (auto& call : calls) {
      out.events.emplace_back(std::move(call));
    }
  }

  void DrainRejectedCandidate(Output& out, bool flushing) {
    const auto found = buffer_.find(end_marker_);
    if (found != std::string::npos) {
      const auto end = found + end_marker_.size();
      EmitVisible(out, buffer_.substr(0, end));
      buffer_.erase(0, end);
      rejected_batch_state_ = RejectedBatchState::kBetweenCandidates;
      inside_tool_call_ = false;
      return;
    }

    if (flushing) {
      EmitVisible(out, std::move(buffer_));
      buffer_.clear();
      rejected_batch_state_ = RejectedBatchState::kNone;
      inside_tool_call_ = false;
      return;
    }

    const auto hold = LongestSuffixThatIsPrefixOf(buffer_, end_marker_);
    const auto safe = buffer_.size() - hold;
    if (safe > 0) {
      EmitVisible(out, buffer_.substr(0, safe));
      buffer_.erase(0, safe);
    }
  }

  void DrainBetweenRejectedCandidates(Output& out, bool flushing) {
    const auto boundary = buffer_.find_first_not_of(" \t\r\n");
    if (boundary == std::string::npos) {
      EmitVisible(out, std::move(buffer_));
      buffer_.clear();
      if (flushing) {
        rejected_batch_state_ = RejectedBatchState::kNone;
      }

      return;
    }

    if (boundary > 0) {
      EmitVisible(out, buffer_.substr(0, boundary));
      buffer_.erase(0, boundary);
    }

    if (buffer_.starts_with(start_marker_)) {
      EmitVisible(out, buffer_.substr(0, start_marker_.size()));
      buffer_.erase(0, start_marker_.size());
      rejected_batch_state_ = RejectedBatchState::kCandidate;
      inside_tool_call_ = true;
      return;
    }

    if (!flushing && start_marker_.starts_with(buffer_)) {
      return;
    }

    rejected_batch_state_ = RejectedBatchState::kNone;
  }

  void DrainSelectedPayload(Output& out, bool flushing) {
    while (true) {
      if (rejected_batch_state_ == RejectedBatchState::kCandidate) {
        DrainRejectedCandidate(out, flushing);
        if (rejected_batch_state_ == RejectedBatchState::kCandidate || buffer_.empty()) {
          return;
        }

        continue;
      }

      if (rejected_batch_state_ == RejectedBatchState::kBetweenCandidates) {
        DrainBetweenRejectedCandidates(out, flushing);
        if (rejected_batch_state_ == RejectedBatchState::kBetweenCandidates || buffer_.empty()) {
          return;
        }

        continue;
      }

      if (inside_tool_call_) {
        const auto available = kSelectedPayloadBufferLimit - tool_call_buffer_.size();
        const auto appended = std::min(available, buffer_.size());
        tool_call_buffer_.append(buffer_, 0, appended);
        buffer_.erase(0, appended);

        auto result = payload_parser_(tool_call_buffer_, flushing);
        if (result.disposition == ToolCallPayloadDisposition::kNeedMore) {
          if (flushing) {
            EmitVisible(out, std::move(tool_call_buffer_));
            tool_call_buffer_.clear();
            inside_tool_call_ = false;
            return;
          }

          if (tool_call_buffer_.size() == kSelectedPayloadBufferLimit) {
            const auto hold = LongestSuffixThatIsPrefixOf(tool_call_buffer_, end_marker_);
            const auto safe = tool_call_buffer_.size() - hold;
            EmitVisible(out, tool_call_buffer_.substr(0, safe));
            buffer_.insert(0, tool_call_buffer_.substr(safe));
            tool_call_buffer_.clear();
            rejected_batch_state_ = RejectedBatchState::kCandidate;
            DrainRejectedCandidate(out, flushing);
            if (rejected_batch_state_ == RejectedBatchState::kCandidate || buffer_.empty()) {
              return;
            }

            continue;
          }

          return;
        }

        if (result.consumed_size == 0 || result.consumed_size > tool_call_buffer_.size()) {
          EmitVisible(out, std::move(tool_call_buffer_));
          tool_call_buffer_.clear();
          inside_tool_call_ = false;
          return;
        }

        auto consumed = tool_call_buffer_.substr(0, result.consumed_size);
        buffer_.insert(0, tool_call_buffer_.substr(result.consumed_size));
        tool_call_buffer_.clear();
        inside_tool_call_ = false;
        if (result.disposition == ToolCallPayloadDisposition::kParsed) {
          EmitParsedCalls(out, std::move(result.calls));
        } else {
          EmitVisible(out, std::move(consumed));
        }

        continue;
      }

      const auto found = buffer_.find(start_marker_);
      if (found != std::string::npos) {
        if (found > 0) {
          EmitVisible(out, buffer_.substr(0, found));
          buffer_.erase(0, found);
        }

        if (markdown_fence_tracker_.InsideFence()) {
          EmitVisible(out, buffer_.substr(0, start_marker_.size()));
          buffer_.erase(0, start_marker_.size());
          continue;
        }

        tool_call_buffer_ = buffer_.substr(0, start_marker_.size());
        buffer_.erase(0, start_marker_.size());
        inside_tool_call_ = true;
        continue;
      }

      if (flushing) {
        EmitVisible(out, std::move(buffer_));
        buffer_.clear();
        return;
      }

      const auto hold = LongestSuffixThatIsPrefixOf(buffer_, start_marker_);
      const auto safe = buffer_.size() - hold;
      if (safe > 0) {
        EmitVisible(out, buffer_.substr(0, safe));
        buffer_.erase(0, safe);
      }

      return;
    }
  }

  bool EmitParsedBlock(Output& out, const std::string& block) const {
    auto parsed = ParseToolCalls(block, start_marker_, end_marker_, tools_json_);
    if (parsed.empty()) {
      return false;
    }

    for (auto& call : parsed) {
      out.events.emplace_back(std::move(call));
    }
    return true;
  }

  void ResetPayloadScan() {
    scan_position_ = start_marker_.size();
    prefix_probe_position_ = scan_position_;
    prefix_quote_position_ = std::string::npos;
    prefix_quote_search_position_ = std::string::npos;
    prefix_classified_ = false;
    ignored_prefix_quote_ = std::string::npos;
    scan_inside_string_ = false;
    scan_escaped_ = false;
  }

  bool ClassifyPayloadPrefix() {
    if (prefix_classified_) {
      return true;
    }

    while (prefix_probe_position_ < tool_call_buffer_.size() &&
           std::string_view(" \t\r\n").find(tool_call_buffer_[prefix_probe_position_]) !=
               std::string_view::npos) {
      ++prefix_probe_position_;
    }
    if (prefix_probe_position_ == tool_call_buffer_.size()) {
      return false;
    }
    if (tool_call_buffer_[prefix_probe_position_] != '<') {
      prefix_classified_ = true;
      return true;
    }

    if (prefix_quote_search_position_ == std::string::npos) {
      prefix_quote_search_position_ = prefix_probe_position_ + 1;
    }
    if (prefix_quote_position_ == std::string::npos) {
      while (prefix_quote_search_position_ < tool_call_buffer_.size()) {
        if (tool_call_buffer_[prefix_quote_search_position_] == '"') {
          prefix_quote_position_ = prefix_quote_search_position_;
          break;
        }
        ++prefix_quote_search_position_;
      }
      if (prefix_quote_position_ == std::string::npos) {
        return false;
      }
    }
    if (prefix_quote_position_ + 1 >= tool_call_buffer_.size()) {
      return false;
    }

    if (tool_call_buffer_[prefix_quote_position_ + 1] == ',') {
      ignored_prefix_quote_ = prefix_quote_position_;
    }
    prefix_classified_ = true;
    return true;
  }

  MarkerMatch FindNextPayloadMarker(bool flushing) {
    if (!ClassifyPayloadPrefix()) {
      return {};
    }

    const size_t marker_width = std::max(start_marker_.size(), end_marker_.size());
    const size_t scan_end = flushing
                                ? tool_call_buffer_.size()
                                : (tool_call_buffer_.size() >= marker_width
                                       ? tool_call_buffer_.size() - marker_width + 1
                                       : 0);

    while (scan_position_ < scan_end) {
      if (!scan_inside_string_) {
        if (tool_call_buffer_.compare(scan_position_, end_marker_.size(), end_marker_) == 0) {
          return {MarkerKind::kEnd, scan_position_};
        }
        if (tool_call_buffer_.compare(scan_position_, start_marker_.size(), start_marker_) == 0) {
          return {MarkerKind::kNestedStart, scan_position_};
        }
      }

      const char ch = tool_call_buffer_[scan_position_];
      if (scan_position_ == ignored_prefix_quote_) {
        ++scan_position_;
        continue;
      }
      if (scan_inside_string_) {
        if (scan_escaped_) {
          scan_escaped_ = false;
        } else if (ch == '\\') {
          scan_escaped_ = true;
        } else if (ch == '"') {
          scan_inside_string_ = false;
        }
      } else if (ch == '"') {
        scan_inside_string_ = true;
      }
      ++scan_position_;
    }

    return {};
  }

  void Drain(Output& out, bool flushing) {
    while (true) {
      if (inside_tool_call_) {
        tool_call_buffer_ += buffer_;
        buffer_.clear();
        const size_t payload_start = start_marker_.size();
        const MarkerMatch match = FindNextPayloadMarker(flushing);

        if (match.kind == MarkerKind::kNestedStart) {
          const size_t nested_start = match.position;
          std::string remainder = tool_call_buffer_.substr(nested_start + start_marker_.size());
          std::string prefix = tool_call_buffer_.substr(0, nested_start);
          if (!EmitParsedBlock(out, prefix + end_marker_)) {
            EmitVisible(out, std::move(prefix));
          }
          tool_call_buffer_ = start_marker_;
          buffer_ = std::move(remainder);
          ResetPayloadScan();
          continue;
        }

        if (match.kind == MarkerKind::kEnd) {
          const size_t end = match.position;
          const size_t block_end = end + end_marker_.size();
          std::string block = tool_call_buffer_.substr(0, block_end);
          buffer_ = tool_call_buffer_.substr(block_end);

          if (!EmitParsedBlock(out, block)) {
            EmitVisible(out, std::move(block));
          }

          tool_call_buffer_.clear();
          inside_tool_call_ = false;
          ResetPayloadScan();
          continue;
        }

        if (flushing) {
          const size_t wrong_end = reasoning_end_marker_.empty()
                                       ? std::string::npos
                                       : FindMarkerOutsideJsonString(
                                             tool_call_buffer_, reasoning_end_marker_, payload_start);
          if (wrong_end != std::string::npos) {
            std::string suffix =
                tool_call_buffer_.substr(wrong_end + reasoning_end_marker_.size());
            if (suffix.find_first_not_of(" \t\r\n") != std::string::npos) {
              // A transcript cannot represent visible text after a tool call without reordering it. Keep the whole
              // model output visible instead of recovering a call that would force the suffix to be discarded.
              EmitVisible(out, std::move(tool_call_buffer_));
            } else {
              std::string candidate = tool_call_buffer_.substr(0, wrong_end) + end_marker_;
              if (EmitParsedBlock(out, candidate)) {
                EmitVisible(out, std::move(suffix));
              } else {
                EmitVisible(out, std::move(tool_call_buffer_));
              }
            }
          } else {
            std::string candidate = tool_call_buffer_ + end_marker_;
            if (!EmitParsedBlock(out, candidate)) {
              EmitVisible(out, std::move(tool_call_buffer_));
            }
          }

          tool_call_buffer_.clear();
          buffer_.clear();
          inside_tool_call_ = false;
          ResetPayloadScan();
          return;
        }

        return;
      }

      const std::string& marker = start_marker_;

      size_t found = buffer_.find(marker);

      if (found != std::string::npos) {
        // Opening marker: emit prefix as visible text, then start buffering the tool-call block (including the
        // marker — ParseToolCalls expects the full `<tool_call>...</tool_call>` substring).
        if (found > 0) {
          EmitVisible(out, buffer_.substr(0, found));
        }
        tool_call_buffer_ = buffer_.substr(found, marker.size());
        buffer_.erase(0, found + marker.size());
        inside_tool_call_ = true;
        ResetPayloadScan();

        continue;  // re-scan the remaining buffer for the next marker
      }

      // No full marker.
      if (flushing) {
        EmitVisible(out, buffer_);
        buffer_.clear();
        return;
      }

      // Outside: emit visible text, but hold back the longest suffix that could still grow into the start marker.
      size_t hold = LongestSuffixThatIsPrefixOf(buffer_, marker);
      size_t safe = buffer_.size() - hold;

      if (safe > 0) {
        EmitVisible(out, buffer_.substr(0, safe));
        buffer_.erase(0, safe);
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
  std::string reasoning_end_marker_;
  ToolCallPayloadParser payload_parser_;
  std::string buffer_;            // pending bytes from Push() that haven't yet been routed
  std::string tool_call_buffer_;  // accumulated bytes of the in-progress tool-call block (incl. start marker)
  bool inside_tool_call_ = false;
  RejectedBatchState rejected_batch_state_ = RejectedBatchState::kNone;
  MarkdownFenceTracker markdown_fence_tracker_;
  size_t scan_position_ = 0;
  size_t prefix_probe_position_ = 0;
  size_t prefix_quote_position_ = std::string::npos;
  size_t prefix_quote_search_position_ = std::string::npos;
  size_t ignored_prefix_quote_ = std::string::npos;
  bool prefix_classified_ = false;
  bool scan_inside_string_ = false;
  bool scan_escaped_ = false;
};

}  // namespace fl
