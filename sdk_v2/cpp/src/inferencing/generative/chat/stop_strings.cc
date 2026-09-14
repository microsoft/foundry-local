// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/stop_strings.h"

#include "exception.h"
#include "util/string_utils.h"

#include <algorithm>
#include <utility>

namespace fl {
namespace {

constexpr size_t kOpenAiMaxStopStrings = 4;
constexpr size_t kEngineMaxStopStrings = 16;
constexpr size_t kEngineMaxStopStringBytes = 16 * 1024;
constexpr size_t kMaxSerializedStopStringsBytes = 128 * 1024;

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

StopStringFilter::PushResult StopStringFilter::PushWithTokenAlignment(
    std::string_view fragment,
    std::optional<int32_t> token_id) {
  if (fragment.empty() || matched_) {
    return {};
  }

  if (stop_strings_.empty()) {
    auto text = std::string(fragment);
    return {text, true, {{std::move(text), token_id}}};
  }

  const bool had_pending_text = !pending_.empty();
  pending_.append(fragment);
  pending_fragments_.push_back({std::string(fragment), token_id});

  if (const auto match = FindBestMatch()) {
    matched_ = true;
    matched_index_ = match->index;

    auto result = ConsumePendingPrefix(match->start);
    pending_.clear();
    pending_fragments_.clear();
    return result;
  }

  const size_t keep = LongestPendingSuffix();
  const size_t safe_count = AlignSafePrefixToTokenBoundary(pending_.size() - keep);
  auto result = ConsumePendingPrefix(safe_count);
  result.token_aligned = !had_pending_text && safe_count == fragment.size();
  return result;
}

std::string StopStringFilter::Flush() {
  return FlushWithTokenAlignment().text;
}

StopStringFilter::PushResult StopStringFilter::FlushWithTokenAlignment() {
  if (matched_) {
    pending_.clear();
    pending_fragments_.clear();
    return {};
  }

  return ConsumePendingPrefix(pending_.size());
}

StopStringFilter::PushResult StopStringFilter::ConsumePendingPrefix(size_t length) {
  PushResult result;
  result.text.reserve(length);

  auto remaining = length;
  while (remaining > 0 && !pending_fragments_.empty()) {
    auto& pending_fragment = pending_fragments_.front();
    const auto consumed = std::min(remaining, pending_fragment.text.size());
    auto text = pending_fragment.text.substr(0, consumed);

    result.text += text;
    result.fragments.push_back({std::move(text), pending_fragment.token_id});

    pending_fragment.text.erase(0, consumed);
    remaining -= consumed;
    if (pending_fragment.text.empty()) {
      pending_fragments_.pop_front();
    }
  }

  pending_.erase(0, length);
  return result;
}

size_t StopStringFilter::AlignSafePrefixToTokenBoundary(size_t length) const {
  size_t fragment_start = 0;
  for (const auto& fragment : pending_fragments_) {
    const auto fragment_end = fragment_start + fragment.text.size();
    if (length > fragment_start && length < fragment_end && fragment.token_id.has_value()) {
      return fragment_start;
    }
    if (length <= fragment_end) {
      return length;
    }

    fragment_start = fragment_end;
  }

  return length;
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
