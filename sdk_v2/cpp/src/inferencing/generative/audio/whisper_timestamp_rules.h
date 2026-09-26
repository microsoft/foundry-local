// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace fl::AudioInternal {

/// Model-specific Whisper control token IDs. IDs differ between Whisper variants (e.g. `<|0.00|>` is 50364 in
/// tiny and 50365 in large-v3-turbo), so they must be resolved from each model's tokenizer rather than hard-coded.
struct WhisperTimestampTokens {
  int32_t eot = 0;              // <|endoftext|>
  int32_t no_timestamps = 0;    // <|notimestamps|>
  int32_t timestamp_begin = 0;  // <|0.00|>
  int32_t timestamp_end = 0;    // <|30.00|>
};

/// Whisper's default `max_initial_timestamp` of 1.0s expressed in 0.02s timestamp steps.
inline constexpr int kWhisperMaxInitialTimestampIndex = 50;

/// Seconds represented by one Whisper timestamp step.
inline constexpr double kWhisperTimestampStepSeconds = 0.02;

/// Last timestamp index of a single 30 s Whisper window.
inline constexpr int kWhisperWindowTimestampSteps = 1500;

/// EOT is masked only when strictly more than 1.0 s remains after an opening timestamp.
inline constexpr int kWhisperMinRemainingAudioTimestampSteps = 50;

/// Whether the resolved token IDs match Whisper's expected control-token ordering and 30 s timestamp range.
bool IsValidWhisperTimestampTokens(const WhisperTimestampTokens& tokens);

/// Convert a verified Whisper timestamp token ID to milliseconds, or nullopt for a non-timestamp token or invalid
/// token layout.
std::optional<int64_t> WhisperTimestampMilliseconds(int32_t token, const WhisperTimestampTokens& tokens);

/// Apply OpenAI Whisper's timestamp decoding rules (ApplyTimestampRules in openai/whisper decoding.py) to one row of
/// next-token logits, in place. Without these rules, greedy decoding picks `<|notimestamps|>` as the first token even
/// when the prompt omits it, so no timestamp tokens are ever generated.
///
/// Rules: suppress `<|notimestamps|>` and other non-EOT control tokens; timestamps appear in pairs (except before
/// EOT); timestamps never decrease and every segment has non-zero length; the first token must be a timestamp no later
/// than `max_initial_timestamp_index`; and a timestamp is forced whenever the total timestamp probability exceeds the
/// most likely text token.
///
/// One deliberate deviation: the reference ends a window with EOT right after an opening timestamp pair and then
/// re-decodes the rest of the audio from that timestamp. There is no such seek loop here, so EOT is masked after a
/// pair while more than `kWhisperMinRemainingAudioTimestampSteps` of audio remain before
/// `audio_end_timestamp_index`.
///
/// @param logits     Next-token logits for a single sequence (length = vocab size).
/// @param generated  Tokens generated so far, excluding the prompt.
/// @param audio_end_timestamp_index  End of the decoded audio in 0.02s steps (capped at one window), or nullopt when
///                   unknown, in which case EOT after a pair is allowed as in the reference.
/// Leaves `logits` untouched if the token IDs are inconsistent with the vocabulary size.
void ApplyWhisperTimestampRules(std::span<float> logits,
                                std::span<const int32_t> generated,
                                const WhisperTimestampTokens& tokens,
                                std::optional<int> max_initial_timestamp_index = kWhisperMaxInitialTimestampIndex,
                                std::optional<int> audio_end_timestamp_index = std::nullopt);

}  // namespace fl::AudioInternal
