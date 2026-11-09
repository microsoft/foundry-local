// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/audio_transcriptions.h"
#include "items/speech_result_item.h"

#include "util/json_helpers.h"

namespace fl {

// ========================================================================
// Request deserialization (from_json)
// ========================================================================

void from_json(const nlohmann::json& j, AudioTranscriptionRequest& r) {
  // Required fields
  r.model = j.at("model").get<std::string>();
  r.filename = j.at("filename").get<std::string>();

  // Optional scalar fields
  opt_str(j, "language", r.language);
  opt_float(j, "temperature", r.temperature);
  opt_str(j, "response_format", r.response_format);
  opt_str(j, "prompt", r.prompt);
  opt_bool(j, "stream", r.stream);
}

// ========================================================================
// Response serialization (to_json)
// ========================================================================

void to_json(nlohmann::json& j, const AudioTranscriptionResponse& r) {
  j = nlohmann::json{
      {"text", r.text},
  };

  if (!r.id.empty()) {
    j["id"] = r.id;
  }
}

nlohmann::json ToAudioTranscriptionJson(const SpeechResultItem& result, const std::string& id,
                                        bool include_timestamps) {
  nlohmann::json output = AudioTranscriptionResponse{.id = id, .text = result.text};
  if (!include_timestamps) return output;
  auto segments = nlohmann::json::array();
  for (const auto& segment : result.segments) {
    nlohmann::json entry = {{"text", segment->text}};
    if (segment->start_time_ms) entry["start_time_ms"] = *segment->start_time_ms;
    if (segment->end_time_ms) entry["end_time_ms"] = *segment->end_time_ms;
    auto words = nlohmann::json::array();
    for (const auto& word : segment->words) {
      nlohmann::json detail = {{"text", word.text}};
      if (word.start_time_ms) detail["start_time_ms"] = *word.start_time_ms;
      if (word.end_time_ms) detail["end_time_ms"] = *word.end_time_ms;
      words.push_back(std::move(detail));
    }
    entry["words"] = std::move(words);
    segments.push_back(std::move(entry));
  }
  output["segments"] = std::move(segments);
  return output;
}

}  // namespace fl
