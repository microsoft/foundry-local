// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "util/key_value_pairs.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

inline constexpr char kInternalStopStringsOptionKey[] = "__foundry_local_internal_stop_strings_json";

/// Normalize the OpenAI `stop` field to a validated vector of UTF-8 strings.
/// Accepts a single string or an array of strings, enforces OpenAI's max-count limit,
/// and also applies the upstream Engine's aggregate 16 KiB limit so later backend
/// routing never has to second-guess the payload.
std::vector<std::string> NormalizeOpenAiStopStrings(const nlohmann::json& stop_json);

/// Serialize normalized stop strings into the internal request-options channel.
void StoreStopStringsOption(const std::vector<std::string>& stop_strings, KeyValuePairs& options);

/// Parse the internal serialized stop-string option. Missing means "no stop strings".
std::vector<std::string> LoadStopStringsOption(const KeyValuePairs& options);

/// Incrementally matches decoded UTF-8 stop strings over already-decoded token fragments.
///
/// Matching is exact byte-for-byte over the UTF-8 emitted by the tokenizer stream. ORT GenAI's
/// tokenizer stream only yields complete UTF-8 sequences, so holding back a suffix of emitted bytes
/// never splits a code point; every chunk returned from Push()/Flush() remains valid UTF-8.
class StopStringFilter {
 public:
  struct TokenFragment {
    std::string text;
    std::optional<int32_t> token_id;
  };

  struct PushResult {
    std::string text;
    bool token_aligned = false;
    std::vector<TokenFragment> fragments;
  };

  explicit StopStringFilter(std::vector<std::string> stop_strings = {});

  /// Feed one decoded fragment into the matcher and return the confirmed-safe prefix.
  /// If a stop string completes, the matching bytes and every later byte are suppressed.
  std::string Push(std::string_view fragment);

  /// Feed one decoded token fragment and preserve the token provenance of every returned safe prefix.
  PushResult PushWithTokenAlignment(std::string_view fragment, std::optional<int32_t> token_id = std::nullopt);

  /// Flush any pending safe suffix at normal end-of-stream.
  std::string Flush();

  /// Flush pending safe text while preserving the originating token boundaries.
  PushResult FlushWithTokenAlignment();

  bool matched() const {
    return matched_;
  }

  std::optional<size_t> matched_index() const {
    return matched_index_;
  }

 private:
  struct MatchCandidate {
    size_t start = 0;
    size_t end = 0;
    size_t index = 0;
  };

  std::optional<MatchCandidate> FindBestMatch() const;
  size_t LongestPendingSuffix() const;
  size_t AlignSafePrefixToTokenBoundary(size_t length) const;
  PushResult ConsumePendingPrefix(size_t length);

  std::vector<std::string> stop_strings_;
  size_t longest_stop_length_ = 0;
  std::string pending_;
  std::deque<TokenFragment> pending_fragments_;
  bool matched_ = false;
  std::optional<size_t> matched_index_;
};

}  // namespace fl
