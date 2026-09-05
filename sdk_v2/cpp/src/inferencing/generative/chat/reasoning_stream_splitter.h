// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "foundry_local/foundry_local_c.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fl {

/// Token-aware state machine that splits generated output around reasoning markers into typed segments.
///
/// Marker token IDs come from ORT GenAI's model metadata. Matching IDs before inspecting decoded text is required
/// for special tokens, whose decoded chunks can be empty when the tokenizer skips special tokens. Token-prefix
/// buffering also supports compatibility callers that provide markers composed of multiple token IDs.
///
/// The text-only Push overload preserves the prior decoded-marker behavior for callers without token IDs. When
/// `start_marker` is empty, both modes degrade to a DEFAULT passthrough for non-reasoning models.
class ReasoningStreamSplitter {
 public:
  struct Segment {
    std::string text;
    flTextItemType type;
  };

  ReasoningStreamSplitter(std::string start_marker,
                          std::string end_marker,
                          std::vector<int32_t> start_token_ids = {},
                          std::vector<int32_t> end_token_ids = {},
                          std::vector<int32_t> ignored_token_ids = {})
      : start_marker_(std::move(start_marker)),
        end_marker_(std::move(end_marker)),
        start_token_ids_(std::move(start_token_ids)),
        end_token_ids_(std::move(end_token_ids)),
        ignored_token_ids_(std::move(ignored_token_ids)) {}

  /// Feed one generated token into the splitter. Marker IDs are consumed even when decoded_text is empty.
  std::vector<Segment> Push(int32_t token_id, std::string decoded_text) {
    if (!HasTextMarkers()) {
      if (decoded_text.empty() || IsIgnoredToken(token_id)) {
        return {};
      }

      return {{std::move(decoded_text), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT}};
    }

    if (!HasTokenMarkers()) {
      return PushText(decoded_text, IsIgnoredToken(token_id));
    }

    std::vector<Segment> out;
    pending_tokens_.push_back({token_id, std::move(decoded_text)});
    DrainTokens(out, /*flushing=*/false);
    return out;
  }

  /// Feed a decoded token into the text-only fallback.
  std::vector<Segment> Push(const std::string& token) {
    return PushText(token, false);
  }

  /// Drain pending content at end-of-generation. A partial marker is content in the current reasoning state.
  std::vector<Segment> Flush() {
    std::vector<Segment> out;

    if (!HasTextMarkers()) {
      return out;
    }

    if (HasTokenMarkers()) {
      DrainTokens(out, /*flushing=*/true);
      DrainText(out, /*flushing=*/true);
    } else {
      DrainText(out, /*flushing=*/true);
    }

    return out;
  }

  /// Whether the splitter is currently inside a reasoning block. Used by callers that want to make
  /// downstream decisions (e.g. suppressing chunks) without inspecting segment types.
  bool InsideReasoning() const noexcept { return inside_reasoning_; }

  /// Number of generated content tokens classified as reasoning. Boundary marker tokens are excluded.
  int ReasoningTokenCount() const noexcept { return reasoning_token_count_; }

 private:
  struct PendingToken {
    int32_t id;
    std::string text;
  };

  struct PendingTextToken {
    std::string text;
    bool reasoning_counted = false;
    bool ignored = false;
  };

  bool HasTextMarkers() const noexcept {
    return !start_marker_.empty() && !end_marker_.empty();
  }

  bool HasTokenMarkers() const noexcept {
    return !start_token_ids_.empty() && !end_token_ids_.empty();
  }

  std::vector<Segment> PushText(const std::string& token, bool ignored) {
    std::vector<Segment> out;

    if (token.empty()) {
      return out;
    }

    if (!HasTextMarkers()) {
      if (!ignored) {
        out.push_back({token, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT});
      }
      return out;
    }

    pending_text_tokens_.push_back({token, false, ignored});
    text_buffer_ += token;
    DrainText(out, /*flushing=*/false);
    return out;
  }

  void DrainTokens(std::vector<Segment>& out, bool flushing) {
    while (!pending_tokens_.empty()) {
      const auto& marker = inside_reasoning_ ? end_token_ids_ : start_token_ids_;
      const auto found = FindTokenSequence(pending_tokens_, marker);

      if (found < pending_tokens_.size()) {
        const auto state_before_prefix = inside_reasoning_;
        EmitPendingTokens(out, found);
        if (inside_reasoning_ != state_before_prefix) {
          continue;
        }

        // A decoded-marker prefix buffered before this ID marker is ordinary content because the complete boundary
        // is represented by the IDs below.
        DrainText(out, /*flushing=*/true);
        if (inside_reasoning_ != state_before_prefix) {
          continue;
        }

        pending_tokens_.erase(
            pending_tokens_.begin(),
            pending_tokens_.begin() + static_cast<std::ptrdiff_t>(marker.size()));
        inside_reasoning_ = !inside_reasoning_;
        trim_default_prefix_ = !inside_reasoning_;
        continue;
      }

      if (flushing) {
        const auto state_before_flush = inside_reasoning_;
        EmitPendingTokens(out, pending_tokens_.size());
        if (inside_reasoning_ == state_before_flush) {
          return;
        }

        continue;
      }

      const auto hold = LongestTokenSuffixThatIsPrefixOf(pending_tokens_, marker);
      const auto safe = pending_tokens_.size() - hold;
      const auto state_before_safe_tokens = inside_reasoning_;
      EmitPendingTokens(out, safe);
      if (inside_reasoning_ != state_before_safe_tokens) {
        continue;
      }

      return;
    }
  }

  void EmitPendingTokens(std::vector<Segment>& out, size_t count) {
    if (count == 0) {
      return;
    }

    for (size_t i = 0; i < count; ++i) {
      auto token = std::move(pending_tokens_.front());
      pending_tokens_.erase(pending_tokens_.begin());
      const auto was_inside_reasoning = inside_reasoning_;

      if (IsIgnoredToken(token.id)) {
        // EOS and configured control tokens are neither reasoning content nor visible output, regardless of how
        // the tokenizer chooses to decode them.
      } else if (token.text.empty()) {
        if (inside_reasoning_) {
          ++reasoning_token_count_;
        }
      } else {
        pending_text_tokens_.push_back({token.text});
        text_buffer_ += token.text;
        DrainText(out, /*flushing=*/false);
      }

      if (inside_reasoning_ != was_inside_reasoning) {
        return;
      }
    }
  }

  static size_t FindTokenSequence(const std::vector<PendingToken>& tokens,
                                  const std::vector<int32_t>& marker) {
    if (marker.empty() || tokens.size() < marker.size()) {
      return tokens.size();
    }

    for (size_t pos = 0; pos + marker.size() <= tokens.size(); ++pos) {
      const auto matches = std::equal(
          marker.begin(), marker.end(), tokens.begin() + static_cast<std::ptrdiff_t>(pos),
          [](int32_t marker_id, const PendingToken& token) { return marker_id == token.id; });
      if (matches) {
        return pos;
      }
    }

    return tokens.size();
  }

  bool IsIgnoredToken(int32_t token_id) const {
    return std::find(ignored_token_ids_.begin(), ignored_token_ids_.end(), token_id) !=
           ignored_token_ids_.end();
  }

  static size_t LongestTokenSuffixThatIsPrefixOf(const std::vector<PendingToken>& tokens,
                                                 const std::vector<int32_t>& marker) {
    const auto max_length = std::min(tokens.size(), marker.size());
    for (size_t length = max_length; length > 0; --length) {
      const auto token_start = tokens.end() - static_cast<std::ptrdiff_t>(length);
      const auto matches = std::equal(
          marker.begin(), marker.begin() + static_cast<std::ptrdiff_t>(length), token_start,
          [](int32_t marker_id, const PendingToken& token) { return marker_id == token.id; });
      if (matches) {
        return length;
      }
    }

    return 0;
  }

  void DrainText(std::vector<Segment>& out, bool flushing) {
    while (true) {
      const std::string& marker = inside_reasoning_ ? end_marker_ : start_marker_;
      flTextItemType current_type = inside_reasoning_ ? FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING
                                                      : FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT;

      // Marker may be empty (e.g. end_marker not configured). With no end marker we can never close a
      // reasoning block — drain the buffer with the current type and stop.
      if (marker.empty()) {
        EmitTextSegment(out, ConsumeText(text_buffer_.size(), current_type, /*is_content=*/true), current_type);
        return;
      }

      size_t found = text_buffer_.find(marker);

      if (found != std::string::npos) {
        // Emit prefix with current type, consume marker, flip state.
        EmitTextSegment(out, ConsumeText(found, current_type, /*is_content=*/true), current_type);
        ConsumeText(marker.size(), current_type, /*is_content=*/false);
        const auto closed_reasoning = inside_reasoning_;
        inside_reasoning_ = !inside_reasoning_;
        trim_default_prefix_ = !inside_reasoning_;

        // Preserve the established behavior of dropping a newline immediately after a closed reasoning block.
        if (closed_reasoning && !text_buffer_.empty() && text_buffer_.front() == '\n') {
          ConsumeText(1, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT, /*is_content=*/false);
          trim_default_prefix_ = false;
        }

        continue;  // re-scan the remaining buffer for the next marker
      }

      // No full marker. If we're flushing, emit everything and stop. Otherwise hold back the longest suffix
      // of buffer_ that could still grow into the marker.
      if (flushing) {
        EmitTextSegment(out, ConsumeText(text_buffer_.size(), current_type, /*is_content=*/true), current_type);
        return;
      }

      size_t hold = LongestSuffixThatIsPrefixOf(text_buffer_, marker);
      size_t safe = text_buffer_.size() - hold;

      if (safe > 0) {
        EmitTextSegment(out, ConsumeText(safe, current_type, /*is_content=*/true), current_type);
      }

      return;
    }
  }

  std::string ConsumeText(size_t length, flTextItemType type, bool is_content) {
    std::string text;
    text.reserve(length);
    text_buffer_.erase(0, length);

    auto remaining = length;
    while (remaining > 0 && !pending_text_tokens_.empty()) {
      auto& token = pending_text_tokens_.front();
      const auto consumed = std::min(remaining, token.text.size());

      // Ignored token bytes participate in marker matching but are never emitted or counted.
      if (is_content && !token.ignored) {
        text.append(token.text, 0, consumed);
        if (type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING && !token.reasoning_counted) {
          ++reasoning_token_count_;
          token.reasoning_counted = true;
        }
      }

      token.text.erase(0, consumed);
      remaining -= consumed;
      if (token.text.empty()) {
        pending_text_tokens_.erase(pending_text_tokens_.begin());
      }
    }

    return text;
  }

  void EmitTextSegment(std::vector<Segment>& out, std::string text, flTextItemType type) {
    if (type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT && trim_default_prefix_ && !text.empty()) {
      if (text.starts_with("\r\n")) {
        text.erase(0, 2);
      } else if (text.starts_with('\n')) {
        text.erase(0, 1);
      }
      trim_default_prefix_ = false;
    }

    EmitSegment(out, std::move(text), type);
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
  std::vector<int32_t> start_token_ids_;
  std::vector<int32_t> end_token_ids_;
  std::vector<int32_t> ignored_token_ids_;
  std::vector<PendingToken> pending_tokens_;
  std::vector<PendingTextToken> pending_text_tokens_;
  std::string text_buffer_;
  bool inside_reasoning_ = false;
  bool trim_default_prefix_ = false;
  int reasoning_token_count_ = 0;
};

}  // namespace fl
