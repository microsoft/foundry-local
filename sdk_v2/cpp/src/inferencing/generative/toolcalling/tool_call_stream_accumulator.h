// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_config.h"
#include "inferencing/generative/toolcalling/tool_call_utils.h"

#include <algorithm>
#include <regex>
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

  ToolCallStreamAccumulator(std::string start_marker, std::string end_marker)
      : start_marker_(std::move(start_marker)), end_marker_(std::move(end_marker)) {}

  /// Construct with a ToolCallConfig for header-inspection (Harmony) or simple (ChatML) mode.
  /// In header-inspection mode, start_marker is the bot marker and end_tokens come from config.
  ToolCallStreamAccumulator(std::string start_marker, std::string end_marker, ToolCallConfig config)
      : start_marker_(std::move(start_marker)),
        end_marker_(std::move(end_marker)),
        config_(std::move(config)) {}

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

    if (config_.mode == ToolCallConfig::Mode::kHeaderInspection) {
      DrainHeaderInspection(out, /*flushing=*/false);
    } else {
      Drain(out, /*flushing=*/false);
    }

    return out;
  }

  /// Drain at end-of-stream. An unterminated tool-call block becomes visible text — it turned out not to be a real
  /// tool call (no closing marker arrived), so the caller still sees what the model produced.
  Output Flush() {
    Output out;

    if (start_marker_.empty() || end_marker_.empty()) {
      return out;
    }

    if (config_.mode == ToolCallConfig::Mode::kHeaderInspection) {
      DrainHeaderInspection(out, /*flushing=*/true);
    } else {
      Drain(out, /*flushing=*/true);
    }

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

          auto parsed = ParseToolCalls(tool_call_buffer_, start_marker_, end_marker_);
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
          // Unterminated tool-call block: surface the buffered bytes as visible text so the caller still sees what
          // the model produced. Matches ReasoningStreamSplitter::Flush behavior for unterminated reasoning.
          out.visible_text.append(tool_call_buffer_);
          out.visible_text.append(buffer_);
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
  std::string buffer_;            // pending bytes from Push() that haven't yet been routed
  std::string tool_call_buffer_;  // accumulated bytes of the in-progress tool-call block (incl. start marker)
  bool inside_tool_call_ = false;

  ToolCallConfig config_;  // default = Simple mode

  // --- Header-inspection mode state ---
  enum class HState { Idle, InHeader, InBody };
  HState hstate_ = HState::Idle;
  std::string header_buffer_;     // header text between bot marker and message_token
  std::string body_buffer_;       // body text between message_token and end_token
  std::string extracted_name_;    // function name extracted from header regex

  /// Header-inspection state machine for Harmony-style models.
  /// States: Idle → InHeader → InBody → (emit) → Idle
  void DrainHeaderInspection(Output& out, bool flushing) {
    while (true) {
      switch (hstate_) {
        case HState::Idle: {
          // Look for the start marker (bot token)
          size_t found = buffer_.find(start_marker_);
          if (found != std::string::npos) {
            // Emit everything before the marker as visible text
            if (found > 0) {
              out.visible_text.append(buffer_, 0, found);
            }
            buffer_.erase(0, found + start_marker_.size());
            hstate_ = HState::InHeader;
            header_buffer_.clear();
            continue;
          }

          if (flushing) {
            out.visible_text.append(buffer_);
            buffer_.clear();
            return;
          }

          // Hold back suffix that could grow into start_marker
          size_t hold = LongestSuffixThatIsPrefixOf(buffer_, start_marker_);
          size_t safe = buffer_.size() - hold;
          if (safe > 0) {
            out.visible_text.append(buffer_, 0, safe);
            buffer_.erase(0, safe);
          }
          return;
        }

        case HState::InHeader: {
          // Accumulate until we see the message_token
          size_t msg_pos = buffer_.find(config_.message_token);
          if (msg_pos != std::string::npos) {
            header_buffer_.append(buffer_, 0, msg_pos);
            buffer_.erase(0, msg_pos + config_.message_token.size());

            // Trim header at channel_token if present (e.g., "assistant to=functions.foo<|channel|>default")
            std::string header_to_match = header_buffer_;
            if (!config_.channel_token.empty()) {
              size_t ch_pos = header_to_match.find(config_.channel_token);
              if (ch_pos != std::string::npos) {
                header_to_match = header_to_match.substr(0, ch_pos);
              }
            }

            // Try to extract function name from header
            std::smatch match;
            std::regex re(config_.header_regex);
            if (std::regex_search(header_to_match, match, re) && match.size() > 1) {
              // This is a tool call — extract name, transition to InBody
              extracted_name_ = match[1].str();
              body_buffer_.clear();
              hstate_ = HState::InBody;
            } else {
              // Not a tool call — this is regular assistant text.
              // Emit the header content as visible text and look for end token to return to Idle.
              out.visible_text.append(header_buffer_);
              out.visible_text.append(config_.message_token);
              header_buffer_.clear();
              hstate_ = HState::Idle;
            }
            continue;
          }

          if (flushing) {
            // Unterminated header — emit as visible text
            out.visible_text.append(start_marker_);
            out.visible_text.append(header_buffer_);
            out.visible_text.append(buffer_);
            header_buffer_.clear();
            buffer_.clear();
            hstate_ = HState::Idle;
            return;
          }

          // Buffer everything — header not complete yet
          header_buffer_.append(buffer_);
          buffer_.clear();
          return;
        }

        case HState::InBody: {
          // Accumulate until we see any end token
          size_t best_pos = std::string::npos;
          size_t best_len = 0;
          for (const auto& end_tok : config_.end_tokens) {
            size_t pos = buffer_.find(end_tok);
            if (pos != std::string::npos && (best_pos == std::string::npos || pos < best_pos)) {
              best_pos = pos;
              best_len = end_tok.size();
            }
          }

          if (best_pos != std::string::npos) {
            // Found end token — finalize the tool call
            body_buffer_.append(buffer_, 0, best_pos);
            buffer_.erase(0, best_pos + best_len);

            // Emit as a parsed tool call
            ParsedToolCall call;
            call.name = extracted_name_;
            call.arguments = body_buffer_;
            call.id = GenerateToolCallId();
            out.ready_calls.push_back(std::move(call));

            body_buffer_.clear();
            extracted_name_.clear();
            hstate_ = HState::Idle;
            continue;
          }

          if (flushing) {
            // Unterminated body — emit everything as visible text (not a valid tool call)
            out.visible_text.append(start_marker_);
            out.visible_text.append(header_buffer_);
            out.visible_text.append(config_.message_token);
            out.visible_text.append(body_buffer_);
            out.visible_text.append(buffer_);
            header_buffer_.clear();
            body_buffer_.clear();
            buffer_.clear();
            hstate_ = HState::Idle;
            return;
          }

          // Buffer body content, hold back suffix that could be an end token prefix
          size_t hold = 0;
          for (const auto& end_tok : config_.end_tokens) {
            hold = std::max(hold, LongestSuffixThatIsPrefixOf(buffer_, end_tok));
          }
          size_t safe = buffer_.size() - hold;
          if (safe > 0) {
            body_buffer_.append(buffer_, 0, safe);
            buffer_.erase(0, safe);
          }
          return;
        }
      }
    }
  }
};

}  // namespace fl
