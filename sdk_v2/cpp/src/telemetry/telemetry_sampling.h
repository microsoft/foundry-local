// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <string_view>

namespace fl::TelemetryInternal {

// Percentage of non-process telemetry events retained. Keep at 100% until we intentionally reduce volume.
// 1DS popSample is metadata only; ShouldSampleTelemetryEvent performs the actual client-side sampling.
inline constexpr double kTelemetrySampleRatePercent = 100.0;
inline constexpr double kCoreAudioTranscribeSampleRatePercent = 2.0;

static_assert(kTelemetrySampleRatePercent >= 0.0 && kTelemetrySampleRatePercent <= 100.0);
static_assert(kCoreAudioTranscribeSampleRatePercent >= 0.0 && kCoreAudioTranscribeSampleRatePercent <= 100.0);

inline double SampleRateForAction(std::string_view action_name) {
  return action_name == "OpenAIAudioTranscribe" ? kCoreAudioTranscribeSampleRatePercent
                                                 : kTelemetrySampleRatePercent;
}

inline uint64_t HashSamplingKey(std::string_view app_session_guid, std::string_view event_key) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char c : app_session_guid) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  for (const unsigned char c : event_key) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline bool ShouldSampleTelemetryEvent(std::string_view app_session_guid, std::string_view event_key,
                                       double sample_rate_percent = kTelemetrySampleRatePercent) {
  if (!(sample_rate_percent > 0.0)) {
    return false;
  }
  if (sample_rate_percent >= 100.0) {
    return true;
  }

  // One million buckets support rates down to 0.0001% while keeping the decision stable for every event
  // sharing the same correlation key. The random process GUID prevents sequential keys from biasing samples.
  constexpr uint64_t kBucketCount = 1'000'000;
  const auto threshold = static_cast<uint64_t>(
      static_cast<long double>(sample_rate_percent) * kBucketCount / 100.0L);
  return HashSamplingKey(app_session_guid, event_key) % kBucketCount < threshold;
}

}  // namespace fl::TelemetryInternal
