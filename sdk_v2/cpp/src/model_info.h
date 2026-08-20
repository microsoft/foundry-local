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

/// Which catalog served a model. Drives shadow-variant dedup and source preference
/// when the same model_id is offered by more than one catalog source.
///
/// `kPublic = 0` so that a zero-initialized / legacy `ModelInfo` (and any cache JSON
/// without a `catalogSource` field) decodes as the public Azure catalog. The numeric
/// order of the enum is NOT the preference order — preference is expressed by
/// `CatalogSourcePriority` (local ranks most-preferred).
enum class CatalogSource {
  kPublic = 0,   ///< Public Azure catalog.
  kPrivate = 1,  ///< Private online catalog (future follow-up).
  kLocal = 2,    ///< BYOM / disk-only stub (short-term marker until the BYOM catalog lands).
};

/// Preference rank for a catalog source: lower = more preferred (local > private > public).
/// Used as the final tiebreak in `Model::CompareBestFirst` so it only ever reorders genuine
/// same-`model_id` duplicates. Independent of the enum's underlying value order.
inline int CatalogSourcePriority(CatalogSource source) {
  switch (source) {
    case CatalogSource::kLocal:
      return 0;
    case CatalogSource::kPrivate:
      return 1;
    case CatalogSource::kPublic:
      return 2;
  }

  return 3;
}

struct ModelInfo {
  std::string model_id;
  std::string name;
  int version = 0;
  std::string alias;
  std::string uri;

  DeviceType device_type = DeviceType::kNotSet;
  std::string execution_provider;  // e.g. "WebGPUExecutionProvider", empty if not set
  std::string task;

  // Which catalog source served this model. Drives shadow-variant dedup and preference.
  // Defaults to kPublic so zero-initialized / legacy infos decode as public.
  // Round-trips through the on-disk catalog cache (absent → kPublic).
  CatalogSource catalog_source = CatalogSource::kPublic;

  // Local cache path for this model. ModelInfo is the single owner of the path: it is populated
  // by a source when the model is found in the local cache, set by Model::Download() after a
  // successful download, and cleared by Model::RemoveFromCache(). Empty for uncached models.
  // Runtime-only and not exposed via the C API. It is persisted to the on-disk catalog snapshot
  // only when it still exists on disk at save time (ModelInfoToJson validates), and a fresh disk
  // scan overrides it on every fetch — so treat any deserialized value as a best-effort hint.
  std::string local_path;

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
