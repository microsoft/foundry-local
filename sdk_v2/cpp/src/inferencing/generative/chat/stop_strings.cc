// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/stop_strings.h"

#include "exception.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace fl {
namespace {

constexpr size_t kOpenAiMaxStopStrings = 4;
constexpr size_t kEngineMaxStopStrings = 16;
constexpr size_t kEngineMaxStopStringBytes = 16 * 1024;
constexpr size_t kMaxSerializedStopStringsBytes = 128 * 1024;

bool IsValidUtf8(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    if (lead <= 0x7F) {
      ++i;
      continue;
    }

    size_t width = 0;
    uint32_t code_point = 0;
    if ((lead & 0xE0) == 0xC0) {
      width = 2;
      code_point = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
      width = 3;
      code_point = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      width = 4;
      code_point = lead & 0x07;
    } else {
      return false;
    }

    if (i + width > text.size()) {
      return false;
    }

    for (size_t j = 1; j < width; ++j) {
      const auto continuation = static_cast<unsigned char>(text[i + j]);
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }

      code_point = (code_point << 6) | (continuation & 0x3F);
    }

    if ((width == 2 && code_point < 0x80) || (width == 3 && code_point < 0x800) ||
        (width == 4 && code_point < 0x10000) || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point <= 0xDFFF)) {
      return false;
    }

    i += width;
  }

  return true;
}

std::vector<std::string> NormalizeStopStringsImpl(const nlohmann::json& stop_json,
                                                  size_t max_count,
                                                  bool allow_empty_array,
                                                  const char* field_name) {
  std::vector<std::string> result;

  auto validate_and_append = [&](std::string value, std::string label) {
    if (value.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, label + " must not be empty");
    }

    if (value.find('\0') != std::string::npos) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, label + " must not contain embedded NUL bytes");
    }

    if (!IsValidUtf8(value)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, label + " must be valid UTF-8");
    }

    if (std::find(result.begin(), result.end(), value) == result.end()) {
      result.push_back(std::move(value));
    }
  };

  if (stop_json.is_string()) {
    validate_and_append(stop_json.get<std::string>(), std::string(field_name));
  } else if (stop_json.is_array()) {
    if (stop_json.empty()) {
      if (allow_empty_array) {
        return {};
      }

      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               std::string(field_name) + " array must contain at least one string");
    }

    if (stop_json.size() > max_count) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               std::string(field_name) + " supports at most " + std::to_string(max_count) + " strings");
    }

    for (size_t i = 0; i < stop_json.size(); ++i) {
      if (!stop_json[i].is_string()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 std::string(field_name) + "[" + std::to_string(i) + "] must be a string");
      }

      validate_and_append(stop_json[i].get<std::string>(),
                          std::string(field_name) + "[" + std::to_string(i) + "]");
    }
  } else {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             std::string(field_name) + " must be a string or array of strings");
  }

  size_t total_bytes = 0;
  for (const auto& stop : result) {
    total_bytes += stop.size();
  }

  if (total_bytes > kEngineMaxStopStringBytes) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             std::string(field_name) + " strings must total at most " +
                 std::to_string(kEngineMaxStopStringBytes) + " UTF-8 bytes");
  }

  return result;
}

}  // namespace

std::vector<std::string> NormalizeOpenAiStopStrings(const nlohmann::json& stop_json) {
  return NormalizeStopStringsImpl(stop_json, kOpenAiMaxStopStrings, /*allow_empty_array=*/true, "stop");
}

void StoreStopStringsOption(const std::vector<std::string>& stop_strings, KeyValuePairs& options) {
  if (stop_strings.empty()) {
    options.Remove(kInternalStopStringsOptionKey);
    return;
  }

  options[kInternalStopStringsOptionKey] = nlohmann::json(stop_strings).dump();
}

std::vector<std::string> LoadStopStringsOption(const KeyValuePairs& options) {
  const char* serialized = options.Find(kInternalStopStringsOptionKey);
  if (serialized == nullptr || *serialized == '\0') {
    return {};
  }

  const std::string_view serialized_view(serialized);
  if (serialized_view.size() > kMaxSerializedStopStringsBytes) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             std::string(kInternalStopStringsOptionKey) + " must be at most " +
                 std::to_string(kMaxSerializedStopStringsBytes) + " serialized bytes");
  }

  nlohmann::json stop_json;
  try {
    stop_json = nlohmann::json::parse(serialized_view);
  } catch (const nlohmann::json::parse_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             std::string("internal stop strings option is not valid JSON: ") + e.what());
  }

  return NormalizeStopStringsImpl(stop_json, kEngineMaxStopStrings, /*allow_empty_array=*/true,
                                  kInternalStopStringsOptionKey);
}

StopStringFilter::StopStringFilter(std::vector<std::string> stop_strings) : stop_strings_(std::move(stop_strings)) {
  for (const auto& stop : stop_strings_) {
    longest_stop_length_ = std::max(longest_stop_length_, stop.size());
  }
}

std::string StopStringFilter::Push(std::string_view fragment) {
  return PushWithTokenAlignment(fragment).text;
}

StopStringFilter::PushResult StopStringFilter::PushWithTokenAlignment(std::string_view fragment) {
  if (fragment.empty() || matched_) {
    return {};
  }

  if (stop_strings_.empty()) {
    return {std::string(fragment), true};
  }

  const bool had_pending_text = !pending_.empty();
  pending_.append(fragment);

  if (const auto match = FindBestMatch()) {
    matched_ = true;
    matched_index_ = match->index;

    std::string safe_prefix = pending_.substr(0, match->start);
    pending_.clear();
    return {std::move(safe_prefix), false};
  }

  const size_t keep = LongestPendingSuffix();
  const size_t safe_count = pending_.size() - keep;
  std::string safe_prefix = pending_.substr(0, safe_count);
  pending_.erase(0, safe_count);
  const bool token_aligned = !had_pending_text && safe_count == fragment.size();
  return {std::move(safe_prefix), token_aligned};
}

std::string StopStringFilter::Flush() {
  if (matched_) {
    pending_.clear();
    return {};
  }

  return std::exchange(pending_, {});
}

std::optional<StopStringFilter::MatchCandidate> StopStringFilter::FindBestMatch() const {
  if (pending_.empty()) {
    return std::nullopt;
  }

  std::optional<MatchCandidate> best;
  const std::string_view pending_view(pending_);

  for (size_t start = 0; start < pending_view.size(); ++start) {
    for (size_t index = 0; index < stop_strings_.size(); ++index) {
      const auto& stop = stop_strings_[index];
      if (stop.size() > pending_view.size() - start) {
        continue;
      }

      if (pending_view.compare(start, stop.size(), stop) != 0) {
        continue;
      }

      MatchCandidate candidate{start, start + stop.size(), index};
      if (!best.has_value() || candidate.end < best->end ||
          (candidate.end == best->end &&
           (candidate.start < best->start ||
            (candidate.start == best->start && candidate.index < best->index)))) {
        best = candidate;
      }
    }
  }

  return best;
}

size_t StopStringFilter::LongestPendingSuffix() const {
  if (pending_.empty() || longest_stop_length_ <= 1) {
    return 0;
  }

  const std::string_view pending_view(pending_);
  const size_t max_keep = std::min(pending_view.size(), longest_stop_length_ - 1);
  for (size_t keep = max_keep; keep > 0; --keep) {
    const auto suffix = pending_view.substr(pending_view.size() - keep);
    for (const auto& stop : stop_strings_) {
      if (stop.size() <= keep) {
        continue;
      }

      if (std::equal(suffix.begin(), suffix.end(), stop.begin())) {
        return keep;
      }
    }
  }

  return 0;
}

}  // namespace fl
