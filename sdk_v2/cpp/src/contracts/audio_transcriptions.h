// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace fl {
struct SpeechResultItem;

// ========================================================================
// OpenAI Audio Transcriptions API — Request/Response contract types.
// Mirrors the POST /v1/audio/transcriptions endpoint.
// ========================================================================

// --- Request types ---

/// The audio transcription request.
/// JSON keys match the OpenAI API specification.
struct AudioTranscriptionRequest {
  std::string model;                           // required
  std::string filename;                        // required — file path on disk
  std::optional<std::string> language;         // ISO-639-1 language code
  std::optional<float> temperature;            // 0.0–1.0
  std::optional<std::string> response_format;  // "json", "text", "verbose_json", "srt", "vtt"
  std::optional<std::string> prompt;           // optional style/context guide
  std::optional<bool> stream;                  // streaming flag
};

// --- Response types ---

/// The audio transcription response.
/// JSON keys: "id" (optional), "text"
struct AudioTranscriptionResponse {
  std::string id;  // unique response/chunk identifier (e.g. "audio_...")
  std::string text;
};

// ========================================================================
// JSON serialization (nlohmann ADL)
// ========================================================================

// --- Request deserialization ---
void from_json(const nlohmann::json& j, AudioTranscriptionRequest& r);

// --- Response serialization ---
void to_json(nlohmann::json& j, const AudioTranscriptionResponse& r);

nlohmann::json ToAudioTranscriptionJson(const SpeechResultItem& result, const std::string& id,
                                        bool include_timestamps);

}  // namespace fl
