// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "items/speech_segment_item.h"

namespace fl {

void SpeechSegmentItem::Finalize() {
  cached_words_.clear();
  cached_words_.reserve(words.size());
  for (const auto& w : words) {
    flSpeechWord aw{};
    aw.version = FOUNDRY_LOCAL_API_VERSION;
    aw.text = w.text.c_str();
    aw.start_time_ms = w.start_time_ms.value_or(FOUNDRY_LOCAL_DURATION_UNSET);
    aw.end_time_ms = w.end_time_ms.value_or(FOUNDRY_LOCAL_DURATION_UNSET);
    aw.confidence = w.confidence.value_or(FOUNDRY_LOCAL_CONFIDENCE_UNSET);
    aw.speaker_id = w.speaker_id.empty() ? nullptr : w.speaker_id.c_str();
    cached_words_.push_back(aw);
  }

  cached_ = {};
  cached_.version = FOUNDRY_LOCAL_API_VERSION;
  cached_.kind = kind;
  cached_.text = text.c_str();
  cached_.start_time_ms = start_time_ms.value_or(FOUNDRY_LOCAL_DURATION_UNSET);
  cached_.end_time_ms = end_time_ms.value_or(FOUNDRY_LOCAL_DURATION_UNSET);
  cached_.utterance_start = utterance_start;
  cached_.language = language.empty() ? nullptr : language.c_str();
  cached_.words = cached_words_.empty() ? nullptr : cached_words_.data();
  cached_.words_count = cached_words_.size();
}

}  // namespace fl
