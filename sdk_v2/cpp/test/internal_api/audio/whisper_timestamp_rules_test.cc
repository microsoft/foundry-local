// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Model-free unit tests for ApplyWhisperTimestampRules. A small synthetic vocabulary keeps the expectations readable:
//   0..4   text tokens
//   5      <|endoftext|>
//   6..9   control tokens (7 = <|notimestamps|>)
//   10..1510 timestamp tokens <|0.00|>..<|30.00|>
//

#include "inferencing/generative/audio/whisper_timestamp_rules.h"
#include "inferencing/generative/audio/onnx_audio_generator.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace fl::AudioInternal;

namespace {

constexpr int kVocab = 1511;
constexpr int kEot = 5;
constexpr int kNoTimestamps = 7;
constexpr int kTsBegin = 10;
constexpr int kTsEnd = 1510;
constexpr WhisperTimestampTokens kTokens{
    .eot = kEot,
    .no_timestamps = kNoTimestamps,
    .timestamp_begin = kTsBegin,
    .timestamp_end = kTsEnd,
};

std::vector<float> Uniform(float value = 0.0f) {
  return std::vector<float>(kVocab, value);
}

// Text tokens (including EOT) strongly preferred so the probability-mass rule does not force a timestamp.
std::vector<float> TextPreferred() {
  auto logits = Uniform();
  for (int i = 0; i <= kEot; ++i) {
    logits[i] = 10.0f;
  }

  return logits;
}

bool IsMasked(float v) {
  return std::isinf(v) && v < 0;
}

void ExpectMaskedRange(const std::vector<float>& logits, int begin, int end) {
  for (int i = begin; i < end; ++i) {
    EXPECT_TRUE(IsMasked(logits[i])) << "token " << i << " should be masked";
  }
}

void ExpectOpenRange(const std::vector<float>& logits, int begin, int end) {
  for (int i = begin; i < end; ++i) {
    EXPECT_FALSE(IsMasked(logits[i])) << "token " << i << " should not be masked";
  }
}

int Argmax(const std::vector<float>& logits) {
  int best = 0;
  for (int i = 1; i < static_cast<int>(logits.size()); ++i) {
    if (logits[i] > logits[best]) {
      best = i;
    }
  }

  return best;
}

}  // namespace

TEST(WhisperTimestampRulesTest, PromptFallsBackToNoTimestampsWhenRulesAreUnavailable) {
  EXPECT_EQ(BuildWhisperPrompt("en", false), "<|startoftranscript|><|en|><|transcribe|><|notimestamps|>");
  EXPECT_EQ(BuildWhisperPrompt("en", true), "<|startoftranscript|><|en|><|transcribe|>");
}

TEST(WhisperTimestampRulesTest, TimestampTokenIdsConvertToLocaleIndependentMilliseconds) {
  EXPECT_FALSE(WhisperTimestampMilliseconds(kTsBegin - 1, kTokens).has_value());
  EXPECT_EQ(WhisperTimestampMilliseconds(kTsBegin, kTokens), 0);
  EXPECT_EQ(WhisperTimestampMilliseconds(kTsBegin + 5, kTokens), 100);
  EXPECT_EQ(WhisperTimestampMilliseconds(kTsEnd, kTokens), 30000);
  EXPECT_FALSE(WhisperTimestampMilliseconds(kTsEnd + 1, kTokens).has_value());
}

TEST(WhisperTimestampRulesTest, FirstStepForcesInitialTimestamp) {
  // Mirrors the real failure: <|notimestamps|> is the greedy choice without the rules.
  auto logits = Uniform();
  logits[kNoTimestamps] = 10.0f;
  logits[2] = 9.0f;

  ApplyWhisperTimestampRules(logits, {}, kTokens, 3);

  ExpectMaskedRange(logits, 0, kTsBegin);
  ExpectOpenRange(logits, kTsBegin, kTsBegin + 4);
  ExpectMaskedRange(logits, kTsBegin + 4, kVocab);
  EXPECT_GE(Argmax(logits), kTsBegin);
}

TEST(WhisperTimestampRulesTest, FirstStepWithoutInitialLimitAllowsAnyTimestamp) {
  auto logits = Uniform();

  ApplyWhisperTimestampRules(logits, {}, kTokens, std::nullopt);

  ExpectMaskedRange(logits, 0, kTsBegin);
  ExpectOpenRange(logits, kTsBegin, kVocab);
}

TEST(WhisperTimestampRulesTest, ControlTokensAlwaysMasked) {
  auto logits = TextPreferred();
  const std::vector<int32_t> generated{kTsBegin, 1};

  ApplyWhisperTimestampRules(logits, generated, kTokens);

  ExpectMaskedRange(logits, kEot + 1, kTsBegin);
  EXPECT_FALSE(IsMasked(logits[kEot]));
}

TEST(WhisperTimestampRulesTest, TimestampPairRequiresText) {
  // <|0.00|>text<|0.02|><|0.02|>: a new segment was opened, so text must follow. With plenty of audio left,
  // <|endoftext|> is also masked: the reference would re-decode from <|0.02|> in a later window, but without that seek
  // loop the remaining speech would be lost.
  auto logits = Uniform();
  const std::vector<int32_t> generated{kTsBegin, 1, kTsBegin + 1, kTsBegin + 1};

  ApplyWhisperTimestampRules(logits, generated, kTokens, kWhisperMaxInitialTimestampIndex, 1000);

  ExpectOpenRange(logits, 0, kEot);
  EXPECT_TRUE(IsMasked(logits[kEot]));
  ExpectMaskedRange(logits, kTsBegin, kVocab);
}

TEST(WhisperTimestampRulesTest, DeferredFinalSegmentCannotEndTranscript) {
  // Mirrors the observed whisper-tiny failure on 29.3 s audio: after ...<|22.72|><|22.72|> the model strongly prefers
  // <|endoftext|>, which silently dropped the final sentence.
  auto logits = Uniform();
  logits[kEot] = 20.0f;
  logits[3] = 1.0f;
  const std::vector<int32_t> generated{kTsBegin, 1, 2, kTsBegin + 5, kTsBegin + 5};

  ApplyWhisperTimestampRules(logits, generated, kTokens, kWhisperMaxInitialTimestampIndex, 1465);

  EXPECT_EQ(Argmax(logits), 3);
}

TEST(WhisperTimestampRulesTest, EotAfterPairAllowedNearAudioEnd) {
  // Opening timestamp 0.10 s with the audio ending at 1.0 s: under 1 s remains, so ending here is legitimate and
  // masking EOT would only force filler text (observed as repeated " []" segments on whisper-base).
  auto logits = Uniform();
  const std::vector<int32_t> generated{kTsBegin, 1, kTsBegin + 5, kTsBegin + 5};

  ApplyWhisperTimestampRules(logits, generated, kTokens, kWhisperMaxInitialTimestampIndex,
                             5 + kWhisperMinRemainingAudioTimestampSteps);

  EXPECT_FALSE(IsMasked(logits[kEot]));
}

TEST(WhisperTimestampRulesTest, EotThresholdUsesStrictlyMoreThanOneSecondRemaining) {
  const std::vector<int32_t> generated{kTsBegin, 1, kTsBegin + 2, kTsBegin + 2};
  for (int remaining_steps : {49, 50, 51}) {
    auto logits = Uniform();
    ApplyWhisperTimestampRules(logits, generated, kTokens, kWhisperMaxInitialTimestampIndex,
                               2 + remaining_steps);
    EXPECT_EQ(IsMasked(logits[kEot]), remaining_steps > kWhisperMinRemainingAudioTimestampSteps)
        << "remaining timestamp steps: " << remaining_steps;
  }
}

TEST(WhisperTimestampRulesTest, EotAfterPairAllowedWhenAudioEndUnknown) {
  auto logits = Uniform();
  const std::vector<int32_t> generated{kTsBegin, 1, kTsBegin + 1, kTsBegin + 1};

  ApplyWhisperTimestampRules(logits, generated, kTokens);

  EXPECT_FALSE(IsMasked(logits[kEot]));
  ExpectMaskedRange(logits, kTsBegin, kVocab);
}

TEST(WhisperTimestampRulesTest, OpeningTimestampAloneRequiresText) {
  auto logits = Uniform();
  const std::vector<int32_t> generated{kTsBegin};

  ApplyWhisperTimestampRules(logits, generated, kTokens);

  ExpectOpenRange(logits, 0, kEot + 1);
  ExpectMaskedRange(logits, kTsBegin, kVocab);
}

TEST(WhisperTimestampRulesTest, ClosingTimestampRequiresTimestampOrEot) {
  // ...text<|0.06|>: the segment is closed, so only another timestamp (same value allowed) or EOT may follow.
  auto logits = TextPreferred();
  const std::vector<int32_t> generated{kTsBegin, 1, 2, kTsBegin + 3};

  ApplyWhisperTimestampRules(logits, generated, kTokens);

  ExpectMaskedRange(logits, 0, kEot);
  EXPECT_FALSE(IsMasked(logits[kEot]));
  ExpectMaskedRange(logits, kTsBegin, kTsBegin + 3);
  ExpectOpenRange(logits, kTsBegin + 3, kVocab);
}

TEST(WhisperTimestampRulesTest, ClosingTimestampMustExceedOpening) {
  // <|0.08|>text: the closing timestamp must be strictly greater than the opening one (non-zero segment length).
  auto logits = TextPreferred();
  const std::vector<int32_t> generated{kTsBegin + 4, 1};

  ApplyWhisperTimestampRules(logits, generated, kTokens);

  ExpectOpenRange(logits, 0, kEot + 1);
  ExpectMaskedRange(logits, kTsBegin, kTsBegin + 5);
  ExpectOpenRange(logits, kTsBegin + 5, kVocab);
}

TEST(WhisperTimestampRulesTest, DominantTimestampProbabilityForcesTimestamp) {
  // No single timestamp beats the best text token, but their combined probability mass does.
  auto logits = Uniform(-100.0f);
  logits[1] = 1.0f;
  for (int i = kTsBegin + 2; i < kVocab; ++i) {
    logits[i] = 0.5f;
  }

  const std::vector<int32_t> generated{kTsBegin, 1};
  ApplyWhisperTimestampRules(logits, generated, kTokens);

  ExpectMaskedRange(logits, 0, kTsBegin);
  EXPECT_GE(Argmax(logits), kTsBegin);
}

TEST(WhisperTimestampRulesTest, DominantTextKeepsTextSelectable) {
  auto logits = Uniform(-100.0f);
  logits[1] = 10.0f;
  logits[kTsBegin + 5] = 0.0f;

  const std::vector<int32_t> generated{kTsBegin, 1};
  ApplyWhisperTimestampRules(logits, generated, kTokens);

  EXPECT_EQ(Argmax(logits), 1);
  EXPECT_FALSE(IsMasked(logits[kTsBegin + 5]));
}

TEST(WhisperTimestampRulesTest, VocabularyExtensionsAfterTimestampRangeRemainSelectableText) {
  auto logits = Uniform(-100.0f);
  logits.push_back(10.0f);
  for (int i = kTsBegin; i <= kTsEnd; ++i) {
    logits[i] = 0.0f;
  }

  const std::vector<int32_t> generated{kTsBegin, 1};
  ApplyWhisperTimestampRules(logits, generated, kTokens);

  EXPECT_EQ(Argmax(logits), kVocab);
  EXPECT_FALSE(IsMasked(logits[kVocab]));
}

TEST(WhisperTimestampRulesTest, InconsistentTokenIdsLeaveLogitsUnchanged) {
  const std::vector<WhisperTimestampTokens> invalid{
      {.eot = -1, .no_timestamps = kNoTimestamps, .timestamp_begin = kTsBegin, .timestamp_end = kTsEnd},
      {.eot = kEot, .no_timestamps = kNoTimestamps, .timestamp_begin = kVocab, .timestamp_end = kVocab + 9},
      {.eot = kEot, .no_timestamps = kTsBegin, .timestamp_begin = kTsBegin, .timestamp_end = kTsEnd},
      {.eot = kEot, .no_timestamps = kEot, .timestamp_begin = kTsBegin, .timestamp_end = kTsEnd},
      {.eot = kTsBegin, .no_timestamps = kNoTimestamps, .timestamp_begin = kEot, .timestamp_end = kTsEnd},
      {.eot = kEot, .no_timestamps = kNoTimestamps, .timestamp_begin = kTsBegin, .timestamp_end = kTsEnd - 1},
  };

  for (const auto& tokens : invalid) {
    auto logits = Uniform(1.0f);
    ApplyWhisperTimestampRules(logits, {}, tokens);
    ExpectOpenRange(logits, 0, kVocab);
  }
}

TEST(WhisperTimestampRulesTest, RealModelTokenLayouts) {
  // whisper-tiny and whisper-large-v3-turbo place <|notimestamps|> / <|0.00|> at different IDs; the rules must
  // follow the resolved IDs rather than hard-coded ones.
  struct Layout {
    int vocab;
    WhisperTimestampTokens tokens;
  };

  const std::vector<Layout> layouts{
      {51865, {.eot = 50257, .no_timestamps = 50363, .timestamp_begin = 50364, .timestamp_end = 51864}},
      {51866, {.eot = 50257, .no_timestamps = 50364, .timestamp_begin = 50365, .timestamp_end = 51865}},
  };

  for (const auto& layout : layouts) {
    std::vector<float> logits(layout.vocab, 0.0f);
    logits[layout.tokens.no_timestamps] = 30.0f;

    ApplyWhisperTimestampRules(logits, {}, layout.tokens);

    EXPECT_TRUE(IsMasked(logits[layout.tokens.no_timestamps]));
    EXPECT_FALSE(IsMasked(logits[layout.tokens.timestamp_begin]));
    EXPECT_FALSE(IsMasked(logits[layout.tokens.timestamp_begin + kWhisperMaxInitialTimestampIndex]));
    EXPECT_TRUE(IsMasked(logits[layout.tokens.timestamp_begin + kWhisperMaxInitialTimestampIndex + 1]));
  }
}
