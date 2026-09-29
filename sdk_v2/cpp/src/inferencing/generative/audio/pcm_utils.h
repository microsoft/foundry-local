// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fl {

/// Convert 16-bit signed little-endian PCM bytes to float32 samples in [-1.0, 1.0].
/// Each pair of bytes becomes one float sample. A trailing odd byte is ignored.
std::vector<float> ConvertS16LEToFloat(const uint8_t* pcm_bytes, size_t byte_count);

namespace AudioInternal {

/// Load a mono or multichannel 16 kHz PCM16/float32 WAV file and mix it to mono float32 samples.
std::vector<float> LoadPcmWavAsFloatSamples(const std::string& audio_file_path);

/// Convert a count of mono 16 kHz PCM samples to whole milliseconds.
int64_t AudioDurationMsFromSamples(int64_t samples);

}  // namespace AudioInternal

}  // namespace fl
