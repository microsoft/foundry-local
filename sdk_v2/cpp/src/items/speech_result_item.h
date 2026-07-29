// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"
#include "items/speech_segment_item.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

/// Final aggregate result for a completed audio request.
/// Output-only. `segments` entries are SpeechSegmentItems with kind = FINAL
/// (or NONE for a single non-segmented transcript).
struct SpeechResultItem : Item {
  std::string text;
  std::string language;  // empty when absent
  std::optional<std::int64_t> duration_ms;
  std::vector<std::unique_ptr<SpeechSegmentItem>> segments;

  SpeechResultItem() : Item(FOUNDRY_LOCAL_ITEM_SPEECH_RESULT) {}

  explicit SpeechResultItem(std::string text_in)
      : Item(FOUNDRY_LOCAL_ITEM_SPEECH_RESULT), text(std::move(text_in)) {}

  // Non-copyable and non-movable for simplicity.
  // We would need to worry about the effect on the cached C ABI pointers if needed.
  SpeechResultItem(const SpeechResultItem&) = delete;
  SpeechResultItem& operator=(const SpeechResultItem&) = delete;
  SpeechResultItem(SpeechResultItem&&) = delete;
  SpeechResultItem& operator=(SpeechResultItem&&) = delete;

  /// Snapshot the current field values (and each segment's) into the cached
  /// C ABI representation. Must be called once after the item is fully
  /// populated and before any `GetApiData` call. Calls `Finalize()` on every
  /// child segment. Fields must not be mutated after Finalize().
  void Finalize() {
    cached_segment_ptrs_.clear();
    cached_segment_ptrs_.reserve(segments.size());
    for (const auto& s : segments) {
      if (s) {
        s->Finalize();
        cached_segment_ptrs_.push_back(s->AsApiType());
      } else {
        cached_segment_ptrs_.push_back(nullptr);
      }
    }

    cached_ = {};
    cached_.version = FOUNDRY_LOCAL_API_VERSION;
    cached_.text = text.c_str();
    cached_.language = language.empty() ? nullptr : language.c_str();
    cached_.duration_ms = duration_ms.value_or(FOUNDRY_LOCAL_DURATION_UNSET);
    cached_.segments = cached_segment_ptrs_.empty() ? nullptr : cached_segment_ptrs_.data();
    cached_.segments_count = cached_segment_ptrs_.size();
  }

  /// Copy the cached C ABI snapshot into `out`. Requires a prior Finalize().
  /// Borrowed pointers in `out` remain valid for the lifetime of this item.
  void GetApiData(flSpeechResultData& out) const { out = cached_; }

 private:
  flSpeechResultData cached_{};
  std::vector<const flItem*> cached_segment_ptrs_;
};

}  // namespace fl
