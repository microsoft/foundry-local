// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/audio/pcm_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fl {

std::vector<float> ConvertS16LEToFloat(const uint8_t* pcm_bytes, size_t byte_count) {
  const size_t sample_count = byte_count / 2;
  std::vector<float> samples(sample_count);

  for (size_t i = 0; i < sample_count; ++i) {
    // Little-endian: low byte first, high byte second.
    int16_t sample;
    std::memcpy(&sample, pcm_bytes + i * 2, sizeof(int16_t));
    samples[i] = static_cast<float>(sample) / 32768.0f;
  }

  return samples;
}

std::vector<uint8_t> ConvertFloatToS16LE(const std::vector<float>& samples) {
  std::vector<uint8_t> out(samples.size() * 2);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float clipped = std::isnan(samples[i]) ? 0.0f : std::clamp(samples[i], -1.0f, 1.0f);
    const auto value = static_cast<uint16_t>(static_cast<int16_t>(std::lrint(clipped * 32767.0f)));
    out[2 * i] = static_cast<uint8_t>(value & 0xFF);
    out[2 * i + 1] = static_cast<uint8_t>(value >> 8);
  }

  return out;
}

}  // namespace fl
