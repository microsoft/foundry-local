// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "util/key_value_pairs.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace fl {

// -----------------------------------------------------------------------
// Model info
// -----------------------------------------------------------------------

/// Device type the model is optimized for. Mirrors flDeviceType.
enum class DeviceType {
  kNotSet = 0,
  kCPU = 1,
  kGPU = 2,
  kNPU = 3,
};

/// Returns "CPU"/"GPU"/"NPU" or "Invalid" for kNotSet.
std::string DeviceTypeToString(DeviceType dt);

struct ModelInfo {
  std::string model_id;
  std::string name;
  int version = 0;
  std::string alias;
  std::string uri;

  DeviceType device_type = DeviceType::kNotSet;
  std::string execution_provider;  // e.g. "WebGPUExecutionProvider", empty if not set
  std::string task;

  // Azure region the catalog was served from (auto-detected from cluster headers).
  // Empty for non-Azure / BYO models. Used to target the matching regional model
  // registry when downloading. Round-trips through the on-disk catalog cache.
  std::string detected_region;

  KeyValuePairs prompt_templates;
  KeyValuePairs model_settings;

  // Extensible property bags (matching C API flModelInfo design).
  // Use FOUNDRY_LOCAL_MODEL_PROP_* key constants for well-known properties.
  // std::less<> enables heterogeneous lookup so find() accepts string_view/const char*
  // without creating a std::string temporary.
  std::map<std::string, std::string, std::less<>> string_properties;
  std::map<std::string, int64_t, std::less<>> int_properties;

  /// Look up a string property by key, returning nullptr if missing.
  const std::string* GetPropertyStr(std::string_view key) const {
    auto it = string_properties.find(key);
    return (it != string_properties.end()) ? &it->second : nullptr;
  }

  /// Look up an int property by key, returning nullptr if missing.
  const int64_t* GetPropertyInt(std::string_view key) const {
    auto it = int_properties.find(key);
    return (it != int_properties.end()) ? &it->second : nullptr;
  }

  /// Look up a string property by key, returning default_value if missing.
  const std::string& GetPropertyWithDefault(std::string_view key,
                                            const std::string& default_value) const {
    auto it = string_properties.find(key);
    return (it != string_properties.end()) ? it->second : default_value;
  }

  /// Look up an int property by key, returning default_value if missing.
  int64_t GetPropertyWithDefault(std::string_view key, int64_t default_value) const {
    auto it = int_properties.find(key);
    return (it != int_properties.end()) ? it->second : default_value;
  }
};

/// Deserialize a ModelInfo from JSON.
ModelInfo ModelInfoFromJson(const nlohmann::json& j);

/// Serialize a ModelInfo to JSON.
nlohmann::json ModelInfoToJson(const ModelInfo& info);

}  // namespace fl
