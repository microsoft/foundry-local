// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/audio/whisper_timestamp_rules.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace fl::AudioInternal {

namespace {

constexpr float kMasked = -std::numeric_limits<float>::infinity();

void Mask(std::span<float> logits, size_t begin, size_t end) {
  end = std::min(end, logits.size());
  if (begin < end) {
    std::fill(logits.begin() + static_cast<std::ptrdiff_t>(begin), logits.begin() + static_cast<std::ptrdiff_t>(end),
              kMasked);
  }
}

// log(sum(exp(x))) over the unmasked entries; -inf when every entry is masked.
float LogSumExp(std::span<const float> values) {
  float max_value = kMasked;
  for (float v : values) {
    max_value = std::max(max_value, v);
  }

  if (!std::isfinite(max_value)) {
    return kMasked;
  }

  double sum = 0.0;
  for (float v : values) {
    sum += std::exp(static_cast<double>(v) - max_value);
  }

  return static_cast<float>(std::log(sum) + max_value);
}

}  // namespace

bool IsValidWhisperTimestampTokens(const WhisperTimestampTokens& tokens) {
  return tokens.eot >= 0 && tokens.no_timestamps > tokens.eot && tokens.timestamp_begin > tokens.no_timestamps &&
         tokens.timestamp_end > tokens.timestamp_begin &&
         tokens.timestamp_end - tokens.timestamp_begin == kWhisperWindowTimestampSteps;
}

std::optional<int64_t> WhisperTimestampMilliseconds(int32_t token, const WhisperTimestampTokens& tokens) {
  if (!IsValidWhisperTimestampTokens(tokens) || token < tokens.timestamp_begin || token > tokens.timestamp_end) {
    return std::nullopt;
  }

  return static_cast<int64_t>(token - tokens.timestamp_begin) * 20;
}

void ApplyWhisperTimestampRules(std::span<float> logits,
                                std::span<const int32_t> generated,
                                const WhisperTimestampTokens& tokens,
                                std::optional<int> max_initial_timestamp_index,
                                std::optional<int> audio_end_timestamp_index) {
  const size_t vocab = logits.size();
  if (!IsValidWhisperTimestampTokens(tokens) || static_cast<size_t>(tokens.timestamp_end) >= vocab) {
    return;
  }

  const auto eot = static_cast<size_t>(tokens.eot);
  const auto ts_begin = static_cast<size_t>(tokens.timestamp_begin);
  const auto ts_end = static_cast<size_t>(tokens.timestamp_end);
  const auto is_timestamp = [&](int32_t token) {
    return token >= tokens.timestamp_begin && token <= tokens.timestamp_end;
  };

  // Control tokens between <|endoftext|> and <|0.00|> (<|notimestamps|>, <|startoftranscript|>, language/task
  // tokens, ...) are never valid transcription output in timestamp mode.
  Mask(logits, eot + 1, ts_begin);

  const size_t n = generated.size();
  const bool last_was_timestamp = n >= 1 && is_timestamp(generated[n - 1]);
  const bool penultimate_was_timestamp = n < 2 || is_timestamp(generated[n - 2]);

  if (last_was_timestamp) {
    if (penultimate_was_timestamp) {
      Mask(logits, ts_begin, ts_end + 1);  // a closing+opening pair was just emitted: text must follow

      // Deviation from the reference: there, <|endoftext|> right after an opening timestamp means "continue from this
      // timestamp in the next window" and transcribe() re-decodes the remaining audio. Without that seek loop any
      // speech after the timestamp would be silently dropped, so EOT is masked while enough audio remains. Near the
      // end of the audio EOT stays allowed; forcing text there only produces filler such as " []".
      const int opening_index = generated[n - 1] - tokens.timestamp_begin;
      if (n >= 2 && audio_end_timestamp_index.has_value() &&
          opening_index + kWhisperMinRemainingAudioTimestampSteps < *audio_end_timestamp_index) {
        logits[eot] = kMasked;
      }
    } else {
      Mask(logits, 0, eot);  // an unpaired timestamp must be followed by another timestamp or EOT
      Mask(logits, ts_end + 1, vocab);
    }
  }

  auto last_timestamp = std::find_if(generated.rbegin(), generated.rend(), is_timestamp);
  if (last_timestamp != generated.rend()) {
    // Timestamps must not decrease; a segment's closing timestamp must also be strictly after its opening one.
    const auto last = static_cast<size_t>(*last_timestamp);
    const size_t limit = (last_was_timestamp && !penultimate_was_timestamp) ? last : last + 1;
    Mask(logits, ts_begin, limit);
  }

  if (n == 0) {
    Mask(logits, 0, ts_begin);
    Mask(logits, ts_end + 1, vocab);
    if (max_initial_timestamp_index.has_value() && *max_initial_timestamp_index >= 0) {
      Mask(logits, ts_begin + static_cast<size_t>(*max_initial_timestamp_index) + 1, ts_end + 1);
    }
  }

  // log_softmax subtracts the same normalizer from every entry, so comparing raw-logit logsumexp against the max
  // text logit is equivalent to the reference's log-probability comparison.
  const std::span<const float> text_logits(logits.data(), ts_begin);
  const std::span<const float> timestamp_logits(logits.data() + ts_begin, ts_end - ts_begin + 1);
  const float timestamp_logsumexp = LogSumExp(timestamp_logits);
  float max_text_logit = *std::max_element(text_logits.begin(), text_logits.end());
  if (ts_end + 1 < vocab) {
    max_text_logit = std::max(max_text_logit, *std::max_element(logits.begin() + ts_end + 1, logits.end()));
  }

  if (timestamp_logsumexp > max_text_logit) {
    Mask(logits, 0, ts_begin);
    Mask(logits, ts_end + 1, vocab);
  }
}

}  // namespace fl::AudioInternal
