// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "model_info.h"

#include <foundry_local/foundry_local_c.h>
#include <nlohmann/json.hpp>

#include <string>

namespace fl {

// ---------------------------------------------------------------------------
// Helpers (same pattern as model.cc anonymous namespace)
// ---------------------------------------------------------------------------
std::string DeviceTypeToString(DeviceType dt) {
  switch (dt) {
    case DeviceType::kCPU:
      return "CPU";
    case DeviceType::kGPU:
      return "GPU";
    case DeviceType::kNPU:
      return "NPU";
    default:
      return "Invalid";
  }
}

namespace {

DeviceType DeviceTypeFromString(const std::string& s) {
  if (s == "CPU") {
    return DeviceType::kCPU;
  }

  if (s == "GPU") {
    return DeviceType::kGPU;
  }

  if (s == "NPU") {
    return DeviceType::kNPU;
  }

  return DeviceType::kNotSet;
}

/// Read a string field from JSON into a string_properties entry.
void ReadStringProp(const nlohmann::json& j, const char* json_key,
                    std::map<std::string, std::string, std::less<>>& props, const char* prop_key) {
  if (j.contains(json_key) && j[json_key].is_string()) {
    auto val = j[json_key].get<std::string>();
    if (!val.empty()) {
      props[prop_key] = std::move(val);
    }
  }
}

/// Read an integer field from JSON into an int_properties entry.
void ReadIntProp(const nlohmann::json& j, const char* json_key,
                 std::map<std::string, int64_t, std::less<>>& props, const char* prop_key) {
  if (j.contains(json_key) && j[json_key].is_number_integer()) {
    props[prop_key] = j[json_key].get<int64_t>();
  }
}

/// Read a JSON bool field into an int_properties entry (0 or 1).
void ReadBoolProp(const nlohmann::json& j, const char* json_key,
                  std::map<std::string, int64_t, std::less<>>& props, const char* prop_key) {
  if (j.contains(json_key) && j[json_key].is_boolean()) {
    props[prop_key] = j[json_key].get<bool>() ? 1 : 0;
  }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// JSON deserialization for ModelInfo
// Matches the AzureFoundryLocalModel JSON format from the server.
// This is the inverse of ModelInfoToJson() and mirrors Model::ToJson().
// ---------------------------------------------------------------------------

ModelInfo ModelInfoFromJson(const nlohmann::json& j) {
  ModelInfo info;

  // Core identity fields
  if (j.contains("id") && j["id"].is_string()) {
    info.model_id = j["id"].get<std::string>();
  }

  if (j.contains("name") && j["name"].is_string()) {
    info.name = j["name"].get<std::string>();
  }

  if (j.contains("version") && j["version"].is_number_integer()) {
    info.version = j["version"].get<int>();
  }

  if (j.contains("alias") && j["alias"].is_string()) {
    info.alias = j["alias"].get<std::string>();
  }

  if (j.contains("uri") && j["uri"].is_string()) {
    info.uri = j["uri"].get<std::string>();
  }

  if (j.contains("detectedRegion") && j["detectedRegion"].is_string()) {
    info.detected_region = j["detectedRegion"].get<std::string>();
  }

  // String properties — named top-level fields → string_properties map
  ReadStringProp(j, "providerType", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR);
  ReadStringProp(j, "modelType", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR);
  ReadStringProp(j, "displayName", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR);
  ReadStringProp(j, "publisher", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR);
  ReadStringProp(j, "license", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR);
  ReadStringProp(j, "licenseDescription", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR);
  ReadStringProp(j, "task", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_TASK_STR);
  const auto* task = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR);
  if (task && !task->empty()) {
    info.task = *task;
  }
  ReadStringProp(j, "minFLVersion", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR);
  ReadStringProp(j, "parentModelUri", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_PARENT_URI_STR);
  ReadStringProp(j, "toolCallStart", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR);
  ReadStringProp(j, "toolCallEnd", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR);
  ReadStringProp(j, "reasoningStart", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR);
  ReadStringProp(j, "reasoningEnd", info.string_properties, FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR);

  // runtime — nested object with deviceType + executionProvider
  if (j.contains("runtime") && j["runtime"].is_object()) {
    const auto& rt = j["runtime"];

    if (rt.contains("deviceType") && rt["deviceType"].is_string()) {
      info.device_type = DeviceTypeFromString(rt["deviceType"].get<std::string>());
    }

    if (rt.contains("executionProvider") && rt["executionProvider"].is_string()) {
      info.execution_provider = rt["executionProvider"].get<std::string>();
    }
  }

  // promptTemplate — singular, nested object with system/user/etc.
  if (j.contains("promptTemplate") && j["promptTemplate"].is_object()) {
    for (const auto& [key, val] : j["promptTemplate"].items()) {
      if (val.is_string()) {
        info.prompt_templates.Add(key, val.get<std::string>());
      }
    }
  }

  // modelSettings — {parameters: [{Name, Value}, ...]}
  if (j.contains("modelSettings") && j["modelSettings"].is_object()) {
    const auto& ms = j["modelSettings"];

    if (ms.contains("parameters") && ms["parameters"].is_array()) {
      for (const auto& param : ms["parameters"]) {
        if (param.contains("Name") && param["Name"].is_string()) {
          std::string key = param["Name"].get<std::string>();
          std::string value;
          if (param.contains("Value") && param["Value"].is_string()) {
            value = param["Value"].get<std::string>();
          }
          info.model_settings.Add(std::move(key), std::move(value));
        }
      }
    }
  }

  // Int properties
  ReadIntProp(j, "fileSizeMb", info.int_properties, FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT);
  ReadIntProp(j, "maxOutputTokens", info.int_properties, FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT);
  ReadIntProp(j, "createdAt", info.int_properties, FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT);

  // supportsToolCalling — JSON bool → int 0/1 in int_properties.
  // Catalog tags (always strings) are handled separately in azure_catalog_models.cc.
  ReadBoolProp(j, "supportsToolCalling", info.int_properties, FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT);

  // supportsReasoning — JSON bool → int 0/1 in int_properties.
  ReadBoolProp(j, "supportsReasoning", info.int_properties, FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT);

  if (j.contains("foundryLocal") && j["foundryLocal"].is_string()) {
    if (j["foundryLocal"].get<std::string>() == "test") {
      info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_IS_TEST_MODEL_INT] = 1;
    }
  }

  // "cached" is ignored on deserialize — Model tracks this separately.

  return info;
}

// ---------------------------------------------------------------------------
// JSON serialization for ModelInfo
// Produces AzureFoundryLocalModel JSON format, mirroring Model::ToJson().
// ---------------------------------------------------------------------------

nlohmann::json ModelInfoToJson(const ModelInfo& info) {
  nlohmann::json j;

  // Required fields
  j["id"] = info.model_id;
  j["name"] = info.name;
  j["version"] = info.version;
  j["alias"] = info.alias;
  j["uri"] = info.uri;

  // detectedRegion — only emit when set so BYO models and pre-region caches are unaffected.
  if (!info.detected_region.empty()) {
    j["detectedRegion"] = info.detected_region;
  }

  // providerType — required in C#, defaults to empty
  const auto* provider = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR);
  j["providerType"] = provider ? *provider : "";

  // modelType — required in C#
  const auto* model_type = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR);
  j["modelType"] = model_type ? *model_type : "";

  // Optional string properties — skip if missing
  const auto* display_name = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR);
  if (display_name && !display_name->empty()) {
    j["displayName"] = *display_name;
  }

  const auto* publisher = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR);
  if (publisher && !publisher->empty()) {
    j["publisher"] = *publisher;
  }

  const auto* license = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR);
  if (license && !license->empty()) {
    j["license"] = *license;
  }

  const auto* license_desc = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR);
  if (license_desc && !license_desc->empty()) {
    j["licenseDescription"] = *license_desc;
  }

  const auto* task = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR);
  if (task && !task->empty()) {
    j["task"] = *task;
  }

  const auto* min_fl = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR);
  if (min_fl && !min_fl->empty()) {
    j["minFLVersion"] = *min_fl;
  }

  const auto* parent_uri = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_PARENT_URI_STR);
  if (parent_uri && !parent_uri->empty()) {
    j["parentModelUri"] = *parent_uri;
  }

  const auto* tool_start = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR);
  if (tool_start && !tool_start->empty()) {
    j["toolCallStart"] = *tool_start;
  }

  const auto* tool_end = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR);
  if (tool_end && !tool_end->empty()) {
    j["toolCallEnd"] = *tool_end;
  }

  const auto* reasoning_start = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR);
  if (reasoning_start && !reasoning_start->empty()) {
    j["reasoningStart"] = *reasoning_start;
  }

  const auto* reasoning_end = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR);
  if (reasoning_end && !reasoning_end->empty()) {
    j["reasoningEnd"] = *reasoning_end;
  }

  // promptTemplate — singular, nested object
  if (!info.prompt_templates.empty()) {
    nlohmann::json pt;
    for (const auto& [key, value] : info.prompt_templates) {
      pt[key] = value;
    }
    j["promptTemplate"] = pt;
  }

  // modelSettings — {parameters: [{Name, Value}, ...]}
  if (!info.model_settings.empty()) {
    nlohmann::json params = nlohmann::json::array();
    for (const auto& [key, value] : info.model_settings) {
      nlohmann::json p;
      p["Name"] = key;
      if (!value.empty()) {
        p["Value"] = value;
      }
      params.push_back(p);
    }
    j["modelSettings"] = {{"parameters", params}};
  }

  // runtime — nested object with deviceType + executionProvider
  if (info.device_type != DeviceType::kNotSet || !info.execution_provider.empty()) {
    nlohmann::json rt;
    rt["deviceType"] = DeviceTypeToString(info.device_type);
    if (!info.execution_provider.empty()) {
      rt["executionProvider"] = info.execution_provider;
    }
    j["runtime"] = rt;
  }

  // Int properties
  const auto* file_size = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT);
  if (file_size) {
    j["fileSizeMb"] = *file_size;
  }

  const auto* max_tokens = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT);
  if (max_tokens) {
    j["maxOutputTokens"] = *max_tokens;
  }

  const auto* created_at = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT);
  if (created_at) {
    j["createdAt"] = *created_at;
  }

  // supportsToolCalling — int property 0/1 → JSON bool
  const auto* stc_int = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT);
  if (stc_int && *stc_int >= 0) {
    j["supportsToolCalling"] = (*stc_int != 0);
  }

  // supportsReasoning — int property 0/1 → JSON bool
  const auto* sr_int = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT);
  if (sr_int && *sr_int >= 0) {
    j["supportsReasoning"] = (*sr_int != 0);
  }

  // testModel — int property 0/1 → JSON bool
  const auto* test_model = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_IS_TEST_MODEL_INT);
  if (test_model) {
    j["testModel"] = (*test_model != 0);
  }

  return j;
}

}  // namespace fl
