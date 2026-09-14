// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "foundry_local/foundry_local_c.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fl {

/// Resolved reasoning boundary markers for a model and request: the marker strings the tokenizer decodes to, plus the
/// token IDs representing them when the model publishes those IDs.
struct ReasoningMarkers {
  std::string start;
  std::string end;
  std::vector<int32_t> start_token_ids;
  std::vector<int32_t> end_token_ids;
  bool start_token_is_published = false;

  bool Configured() const noexcept { return !start.empty() && !end.empty(); }
};

namespace reasoning_detail {

/// Index of the last occurrence of `marker` in `tokens`, or nullopt when it does not occur.
inline std::optional<size_t> LastTokenSequenceIndex(std::span<const int32_t> tokens,
                                                    const std::vector<int32_t>& marker) {
  if (marker.empty() || tokens.size() < marker.size()) {
    return std::nullopt;
  }

  for (size_t pos = tokens.size() - marker.size() + 1; pos-- > 0;) {
    if (std::equal(marker.begin(), marker.end(), tokens.begin() + static_cast<std::ptrdiff_t>(pos))) {
      return pos;
    }
  }

  return std::nullopt;
}

}  // namespace reasoning_detail

/// One reasoning marker as the model publishes it: the token ID the tokenizer reports, and the text that ID decodes
/// to. Either may be absent — a model that defines no marker publishes neither, and a tokenizer that skips special
/// tokens can decode a valid ID to an empty string.
struct PublishedMarker {
  std::optional<int32_t> id;
  std::string text;
};

inline bool UsesPublishedToken(const std::string& marker, const PublishedMarker& published) {
  return !marker.empty() && published.id.has_value() && published.text == marker;
}

/// Token IDs that represent `marker` in generated output, or an empty sequence when they cannot be established.
///
/// The marker *string* can be overridden per request, while the ID the model publishes always describes the model's
/// own marker. Pairing an overridden string with the published ID would match a completely different token: the
/// splitter would flip its reasoning state on a token that is not the boundary, leaking the scratchpad into the
/// visible answer or hiding the answer inside it. So the published ID is used only when it is proven to be this
/// marker — when it decodes to exactly the marker text — and otherwise the IDs are derived from the marker text
/// itself with the model's own encoder.
///
/// `encode` maps text to the token IDs it encodes to and must return an empty sequence when it cannot (no
/// tokenizer, a failure, a marker the tokenizer round-trips to nothing). An empty result is safe: the splitter and
/// the prompt probe both fall back to matching the decoded text.
template <typename EncodeFn>
std::vector<int32_t> ResolveMarkerTokenIds(const std::string& marker,
                                           const PublishedMarker& published,
                                           EncodeFn&& encode) {
  if (marker.empty()) {
    return {};
  }

  if (UsesPublishedToken(marker, published)) {
    return {*published.id};
  }

  return std::forward<EncodeFn>(encode)(marker);
}

/// Whether a rendered prompt leaves a reasoning block open.
///
/// Some package templates end the assistant prompt with the configured beginning-of-reasoning marker. That marker is
/// prompt input, so generation starts *inside* reasoning and the model only ever emits the closing marker. A splitter
/// that always starts outside would report the scratchpad as visible text, leak the closing marker, and count no
/// reasoning tokens.
///
/// Two independent conditions must both hold, because the prompt also contains untrusted message content:
///
///  1. Position — the last opener in the rendered text must be followed by nothing but whitespace. A marker-shaped
///     sequence inside a user, system, or tool message is always followed by the rest of that message and by the
///     assistant turn header, so message content can never seed the state. Without this, a tool result quoting an
///     unbalanced marker would silently reclassify an entire answer as hidden reasoning.
///  2. Identity — when marker token IDs are known, the encoded prompt must agree that reasoning is open (its last
///     opener sequence comes after its last closer sequence). Special tokens must be matched by ID because a
///     tokenizer may decode them to nothing. Markers with no known IDs fall back to the positional rule alone.
///
/// Identity can only refute the positional rule when the marker is the model's published dedicated token. An
/// encode-derived sequence carries no such guarantee, even when it contains one ID: a tokenizer may merge the
/// marker with adjacent prompt text. Its absence is therefore inconclusive, and the positional rule stands exactly
/// as it does for a prompt with no encoded form at all (the media path).
inline bool PromptOpensReasoning(std::span<const int32_t> prompt_token_ids,
                                 const ReasoningMarkers& markers,
                                 std::string_view prompt_text) {
  if (!markers.Configured()) {
    return false;
  }

  const auto last_start = prompt_text.rfind(markers.start);
  if (last_start == std::string_view::npos) {
    return false;
  }

  const auto tail = prompt_text.substr(last_start + markers.start.size());
  const auto tail_is_whitespace = std::all_of(tail.begin(), tail.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });

  if (!tail_is_whitespace) {
    return false;
  }

  if (markers.start_token_ids.empty() || markers.end_token_ids.empty() || prompt_token_ids.empty()) {
    return true;
  }

  const auto last_start_token = reasoning_detail::LastTokenSequenceIndex(prompt_token_ids, markers.start_token_ids);
  if (!last_start_token.has_value()) {
    return !markers.start_token_is_published;
  }

  const auto last_end_token = reasoning_detail::LastTokenSequenceIndex(prompt_token_ids, markers.end_token_ids);
  return !last_end_token.has_value() || *last_end_token < *last_start_token;
}

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

  /// @param starts_inside_reasoning  Seed the stream as already inside a reasoning block, for prompts whose template
  ///        emitted the opening marker (see PromptOpensReasoning). The opener is prompt input, so it never reaches
  ///        the splitter and is never counted as a generated reasoning token; the model's closing marker is consumed
  ///        and suppressed exactly as it is for a block the model opened itself.
  ReasoningStreamSplitter(std::string start_marker,
                          std::string end_marker,
                          std::vector<int32_t> start_token_ids = {},
                          std::vector<int32_t> end_token_ids = {},
                          std::vector<int32_t> ignored_token_ids = {},
                          bool starts_inside_reasoning = false)
      : start_marker_(std::move(start_marker)),
        end_marker_(std::move(end_marker)),
        start_token_ids_(std::move(start_token_ids)),
        end_token_ids_(std::move(end_token_ids)),
        ignored_token_ids_(std::move(ignored_token_ids)),
        // Reads the marker members, which the declaration order below guarantees are already initialized. Seeding is
        // ignored when no markers are configured, so a passthrough splitter can never be stuck inside reasoning.
        inside_reasoning_(HasTextMarkers() && starts_inside_reasoning) {}

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
