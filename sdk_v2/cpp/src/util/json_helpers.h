// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fl {

/// Preserve declaration order and reject ambiguous objects before DOM parsing can overwrite their duplicate keys.
inline nlohmann::ordered_json ParseJsonStrict(std::string_view source) {
  using Json = nlohmann::ordered_json;
  std::vector<std::unordered_set<std::string>> objects;
  return Json::parse(source, [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start) {
      objects.emplace_back();
    } else if (event == Json::parse_event_t::object_end) {
      objects.pop_back();
    } else if (event == Json::parse_event_t::key &&
               !objects.back().insert(value.get<std::string>()).second) {
      throw Json::parse_error::create(101, 0, "Duplicate JSON object member: " + value.dump(), &value);
    }
    return true;
  });
}

/// Generic optional field extraction from JSON.
/// Extracts the value at `key` if present and non-null.
template <typename T>
inline void opt(const nlohmann::json& j, const char* key, std::optional<T>& out) {
  if (j.contains(key) && !j[key].is_null()) {
    out = j[key].get<T>();
  }
}

/// Extract an optional string field, checking is_string().
inline void opt_str(const nlohmann::json& j, const char* key, std::optional<std::string>& out) {
  if (j.contains(key) && j[key].is_string()) {
    out = j[key].get<std::string>();
  }
}

/// Extract an optional int field, checking is_number_integer().
inline void opt_int(const nlohmann::json& j, const char* key, std::optional<int>& out) {
  if (j.contains(key) && j[key].is_number_integer()) {
    out = j[key].get<int>();
  }
}

/// Extract an optional int64_t field, checking is_number_integer().
inline void opt_int64(const nlohmann::json& j, const char* key, std::optional<int64_t>& out) {
  if (j.contains(key) && j[key].is_number_integer()) {
    out = j[key].get<int64_t>();
  }
}

/// Extract an optional float field, checking is_number().
inline void opt_float(const nlohmann::json& j, const char* key, std::optional<float>& out) {
  if (j.contains(key) && j[key].is_number()) {
    out = j[key].get<float>();
  }
}

/// Extract an optional bool field, checking is_boolean().
inline void opt_bool(const nlohmann::json& j, const char* key, std::optional<bool>& out) {
  if (j.contains(key) && j[key].is_boolean()) {
    out = j[key].get<bool>();
  }
}

}  // namespace fl
