// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/audio/pcm_utils.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>

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

std::optional<double> AudioInternal::TryReadWavDurationSeconds(const std::string& audio_file_path) {
  std::ifstream in(audio_file_path, std::ios::binary);
  in.seekg(0, std::ios::end);
  const std::streamoff file_size = in.tellg();
  in.seekg(0, std::ios::beg);
  if (!in || file_size < 12) {
    return std::nullopt;
  }

  char riff[4];
  uint32_t riff_size = 0;
  char wave[4];
  in.read(riff, sizeof(riff));
  in.read(reinterpret_cast<char*>(&riff_size), sizeof(riff_size));
  in.read(wave, sizeof(wave));
  if (!in || std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0) {
    return std::nullopt;
  }

  const uint64_t riff_end_value = static_cast<uint64_t>(riff_size) + 8;
  if (riff_end_value < 12 || riff_end_value > static_cast<uint64_t>(file_size) ||
      riff_end_value > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    return std::nullopt;
  }
  const auto riff_end = static_cast<std::streamoff>(riff_end_value);

  uint32_t byte_rate = 0;
  while (in) {
    const std::streamoff header_start = in.tellg();
    if (header_start < 0 || riff_end - header_start < 8) {
      break;
    }

    char chunk_id[4];
    uint32_t chunk_size = 0;
    in.read(chunk_id, sizeof(chunk_id));
    in.read(reinterpret_cast<char*>(&chunk_size), sizeof(chunk_size));
    if (!in) {
      return std::nullopt;
    }

    const std::streamoff chunk_start = in.tellg();
    const uint64_t padded_size = static_cast<uint64_t>(chunk_size) + (chunk_size % 2);
    if (chunk_start < 0 || padded_size > static_cast<uint64_t>(riff_end - chunk_start)) {
      return std::nullopt;
    }

    if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
      if (chunk_size < 16) {
        return std::nullopt;
      }

      uint16_t audio_format = 0;
      uint16_t channels = 0;
      uint32_t sample_rate = 0;
      uint16_t block_align = 0;
      uint16_t bits_per_sample = 0;
      in.read(reinterpret_cast<char*>(&audio_format), sizeof(audio_format));
      in.read(reinterpret_cast<char*>(&channels), sizeof(channels));
      in.read(reinterpret_cast<char*>(&sample_rate), sizeof(sample_rate));
      in.read(reinterpret_cast<char*>(&byte_rate), sizeof(byte_rate));
      in.read(reinterpret_cast<char*>(&block_align), sizeof(block_align));
      in.read(reinterpret_cast<char*>(&bits_per_sample), sizeof(bits_per_sample));
      if (!in || audio_format == 0 || channels == 0 || sample_rate == 0 || bits_per_sample == 0 ||
          bits_per_sample % 8 != 0 || block_align != channels * (bits_per_sample / 8) ||
          static_cast<uint64_t>(byte_rate) != static_cast<uint64_t>(sample_rate) * block_align) {
        return std::nullopt;
      }

      in.seekg(chunk_start + static_cast<std::streamoff>(padded_size));
    } else if (std::strncmp(chunk_id, "data", 4) == 0) {
      if (byte_rate == 0) {
        return std::nullopt;
      }

      return static_cast<double>(chunk_size) / byte_rate;
    } else {
      in.seekg(chunk_start + static_cast<std::streamoff>(padded_size));
    }
  }

  return std::nullopt;
}

}  // namespace fl
