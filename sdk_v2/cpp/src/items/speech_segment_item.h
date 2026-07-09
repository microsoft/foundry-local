// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

/// One word within a speech segment. Output-only — produced by the SDK,
/// never constructed by callers via the C ABI.
struct SpeechWord {
  std::string text;
  std::optional<std::int64_t> start_time_ms;
  std::optional<std::int64_t> end_time_ms;
  std::optional<float> confidence;
  std::string speaker_id;  // empty when absent
};

/// A recognized / translated speech segment.
///
/// Streaming model (see SpeechOutputTypes.md): zero-or-more PARTIAL segments for
/// the current segment, then exactly one FINAL closes it. Segment identity
/// is implicit in stream order; there is no segment id. `utterance_start`
/// tags the first segment of a new utterance.
///
/// PARTIAL `text` is the cumulative current hypothesis for the segment, not
/// a delta.
///
/// As an entry of a SpeechResultItem, `kind` is FINAL (or NONE for a single
/// non-segmented transcript).
struct SpeechSegmentItem : Item {
  flSpeechSegmentKind kind = FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE;
  std::string text;
  std::optional<std::int64_t> start_time_ms;
  std::optional<std::int64_t> end_time_ms;
  bool utterance_start = false;
  std::vector<SpeechWord> words;
  std::string language;  // empty when absent

  SpeechSegmentItem() : Item(FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT) {}

  SpeechSegmentItem(flSpeechSegmentKind kind_in, std::string text_in)
      : Item(FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT),
        kind(kind_in),
        text(std::move(text_in)) {}

  // Non-copyable and non-movable for simplicity.
  // We would need to worry about the effect on the cached C ABI pointers if needed.
  SpeechSegmentItem(const SpeechSegmentItem&) = delete;
  SpeechSegmentItem& operator=(const SpeechSegmentItem&) = delete;
  SpeechSegmentItem(SpeechSegmentItem&&) = delete;
  SpeechSegmentItem& operator=(SpeechSegmentItem&&) = delete;

  /// Snapshot the current field values into the cached C ABI representation.
  /// Must be called once after the item is fully populated and before any
  /// `GetApiData` call. Fields must not be mutated after Finalize().
  void Finalize();

  /// Copy the cached C ABI snapshot into `out`. Requires a prior Finalize().
  /// Borrowed pointers in `out` remain valid for the lifetime of this item.
  void GetApiData(flSpeechSegmentData& out) const { out = cached_; }

 private:
  flSpeechSegmentData cached_{};
  std::vector<flSpeechWord> cached_words_;
};

}  // namespace fl
