// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_catalog_models.h"

#include "util/json_helpers.h"
#include "utils.h"

#include <foundry_local/foundry_local_c.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>

namespace fl {

namespace {

/// Parse an ISO 8601 datetime string to Unix timestamp (seconds).
/// Handles formats like "2024-01-15T10:30:00Z" and "2024-01-15T10:30:00.1234567+00:00".
/// Returns 0 on failure.
int64_t ParseIso8601ToUnix(const std::string& iso_str) {
  if (iso_str.empty()) {
    return 0;
  }

  // std::chrono::parse would be nice but requires a very recent GCC.
  std::tm tm = {};
  std::istringstream ss(iso_str);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

  if (ss.fail()) {
    return 0;
  }

#ifdef _WIN32
  time_t t = _mkgmtime(&tm);
#else
  time_t t = timegm(&tm);
#endif

  return t == static_cast<time_t>(-1) ? 0 : static_cast<int64_t>(t);
}

/// Parse a numeric string field (e.g. the catalog's string-typed "version"). Returns
/// nullopt on absence or malformed input.
std::optional<int> ParseIntString(const std::optional<std::string>& value) {
  if (!value || value->empty()) {
    return std::nullopt;
  }

  try {
    return std::stoi(*value);
  } catch (...) {
    return std::nullopt;
  }
}

std::string JoinStrings(const std::vector<std::string>& values) {
  std::ostringstream joined;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      joined << ',';
    }
    joined << values[i];
  }
  return joined.str();
}

bool ContainsStringIgnoreCase(const std::vector<std::string>& values, const std::string& target) {
  const auto lowered_target = ToLower(target);
  return std::any_of(values.begin(), values.end(),
                     [&](const std::string& value) { return ToLower(value) == lowered_target; });
}

DeviceType ParseDeviceType(const std::string& device) {
  const auto lower = ToLower(device);
  if (lower == "cpu") {
    return DeviceType::kCPU;
  }

  if (lower == "gpu") {
    return DeviceType::kGPU;
  }

  if (lower == "npu") {
    return DeviceType::kNPU;
  }
  return DeviceType::kNotSet;
}

}  // anonymous namespace

// ========================================================================
// Request serialization (to_json)
// ========================================================================

void to_json(nlohmann::json& j, const CatalogFilter& f) {
  j = nlohmann::json{
      {"field", f.field},
      {"operator", f.op},
      {"values", f.values},
  };
}

void to_json(nlohmann::json& j, const AzureCatalogRequest& r) {
  j = nlohmann::json{
      {"filters", r.filters},
      {"pageSize", r.page_size},
  };

  if (r.continuation_token && !r.continuation_token->empty()) {
    j["continuationToken"] = *r.continuation_token;
  }
}

// ========================================================================
// Response deserialization (from_json)
// ========================================================================

void from_json(const nlohmann::json& j, TextLimits& t) {
  opt_int64(j, "inputContextWindow", t.input_context_window);
  opt_int64(j, "maxOutputTokens", t.max_output_tokens);
}

void from_json(const nlohmann::json& j, ModelLimits& m) {
  if (j.contains("textLimits") && j["textLimits"].is_object()) {
    m.text_limits = j["textLimits"].get<TextLimits>();
  }

  if (j.contains("supportedInputModalities") && j["supportedInputModalities"].is_array()) {
    m.supported_input_modalities = j["supportedInputModalities"].get<std::vector<std::string>>();
  }

  if (j.contains("supportedOutputModalities") && j["supportedOutputModalities"].is_array()) {
    m.supported_output_modalities = j["supportedOutputModalities"].get<std::vector<std::string>>();
  }
}

void from_json(const nlohmann::json& j, VariantMetadata& v) {
  opt_str(j, "modelType", v.model_type);

  if (j.contains("quantization") && j["quantization"].is_array()) {
    v.quantization = j["quantization"].get<std::vector<std::string>>();
  }

  opt_str(j, "device", v.device);
  opt_str(j, "executionProvider", v.execution_provider);
  opt_int64(j, "fileSizeBytes", v.file_size_bytes);
}

void from_json(const nlohmann::json& j, VariantParent& v) {
  opt_str(j, "assetId", v.asset_id);
}

void from_json(const nlohmann::json& j, VariantInformation& v) {
  if (j.contains("parents") && j["parents"].is_array()) {
    v.parents = j["parents"].get<std::vector<VariantParent>>();
  }

  if (j.contains("variantMetadata") && j["variantMetadata"].is_object()) {
    v.variant_metadata = j["variantMetadata"].get<VariantMetadata>();
  }
}

void from_json(const nlohmann::json& j, CatalogLocalModel& m) {
  opt_str(j, "assetId", m.asset_id);
  opt_str(j, "name", m.name);
  opt_str(j, "alias", m.alias);
  opt_str(j, "displayName", m.display_name);
  opt_str(j, "version", m.version);
  opt_str(j, "publisher", m.publisher);
  opt_str(j, "license", m.license);
  opt_str(j, "createdTime", m.created_time);

  if (j.contains("inferenceTasks") && j["inferenceTasks"].is_array()) {
    m.inference_tasks = j["inferenceTasks"].get<std::vector<std::string>>();
  }

  if (j.contains("modelCapabilities") && j["modelCapabilities"].is_array()) {
    m.model_capabilities = j["modelCapabilities"].get<std::vector<std::string>>();
  }

  if (j.contains("deploymentOptions") && j["deploymentOptions"].is_array()) {
    m.deployment_options = j["deploymentOptions"].get<std::vector<std::string>>();
  }

  if (j.contains("modelLimits") && j["modelLimits"].is_object()) {
    m.model_limits = j["modelLimits"].get<ModelLimits>();
  }

  if (j.contains("variantInformation") && j["variantInformation"].is_object()) {
    m.variant_information = j["variantInformation"].get<VariantInformation>();
  }
}

void from_json(const nlohmann::json& j, AzureCatalogResponse& r) {
  opt_int(j, "totalCount", r.total_count);
  opt_str(j, "continuationToken", r.continuation_token);

  if (j.contains("summaries") && j["summaries"].is_array()) {
    r.models = j["summaries"].get<std::vector<CatalogLocalModel>>();
  }
}

// ========================================================================
// CatalogLocalModel → ModelInfo conversion
// ========================================================================

std::optional<ModelInfo> CatalogModelToModelInfo(const CatalogLocalModel& cm) {
  // Required fields — skip entry if missing.
  if (!cm.asset_id || cm.asset_id->empty()) {
    return std::nullopt;
  }

  if (!cm.name || cm.name->empty()) {
    return std::nullopt;
  }

  if (!cm.alias || cm.alias->empty()) {
    return std::nullopt;
  }

  // Entries with no variant information are the abstract parent model, not a
  // runnable variant — skip them.
  if (!cm.variant_information) {
    return std::nullopt;
  }

  const int version = ParseIntString(cm.version).value_or(0);

  // Extract parent model URI (used for alias and stored as a property).
  std::string parent_uri;
  if (!cm.variant_information->parents.empty() && cm.variant_information->parents[0].asset_id) {
    parent_uri = *cm.variant_information->parents[0].asset_id;
  }

  ModelInfo info;
  info.model_id = *cm.name + ":" + std::to_string(version);
  info.name = *cm.name;
  info.version = version;
  info.alias = *cm.alias;
  info.uri = *cm.asset_id;

  // Device type, execution provider, and model type from variant metadata.
  if (cm.variant_information->variant_metadata) {
    const auto& vm = *cm.variant_information->variant_metadata;
    if (vm.device) {
      info.device_type = ParseDeviceType(*vm.device);
    }

    if (vm.execution_provider) {
      info.execution_provider = *vm.execution_provider;
    }

    // ModelType — defaults to "ONNX" (matches C# ToAzureFoundryLocalModel).
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR] = vm.model_type.value_or("ONNX");

    if (!vm.quantization.empty()) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_QUANTIZATION_STR] = JoinStrings(vm.quantization);
    }

    if (vm.file_size_bytes) {
      info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT] = *vm.file_size_bytes / (1024 * 1024);
    }
  } else {
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR] = "ONNX";
  }

  if (!cm.inference_tasks.empty()) {
    info.task = cm.inference_tasks.front();
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TASK_STR] = cm.inference_tasks.front();
  }

  if (cm.license && !cm.license->empty()) {
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR] = *cm.license;
  }

  if (cm.publisher && !cm.publisher->empty()) {
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = *cm.publisher;
  }

  if (cm.display_name && !cm.display_name->empty()) {
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR] = *cm.display_name;
  }

  if (!cm.model_capabilities.empty()) {
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_CAPABILITIES_STR] = JoinStrings(cm.model_capabilities);
    info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT] =
        ContainsStringIgnoreCase(cm.model_capabilities, "tool-calling") ? 1 : 0;
    info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT] =
        ContainsStringIgnoreCase(cm.model_capabilities, "reasoning") ? 1 : 0;
  }

  if (cm.model_limits) {
    if (!cm.model_limits->supported_input_modalities.empty()) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR] =
          JoinStrings(cm.model_limits->supported_input_modalities);
    }

    if (!cm.model_limits->supported_output_modalities.empty()) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR] =
          JoinStrings(cm.model_limits->supported_output_modalities);
    }

    if (cm.model_limits->text_limits) {
      if (cm.model_limits->text_limits->input_context_window) {
        info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT] =
            *cm.model_limits->text_limits->input_context_window;
      }

      if (cm.model_limits->text_limits->max_output_tokens) {
        info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT] =
            *cm.model_limits->text_limits->max_output_tokens;
      }
    }
  }

  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR] = "FoundryLocal";

  // Parent model URI.
  if (!parent_uri.empty()) {
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PARENT_URI_STR] = parent_uri;
  }

  // CreatedAtUnix — createdTime → Unix timestamp.
  if (cm.created_time && !cm.created_time->empty()) {
    info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT] = ParseIso8601ToUnix(*cm.created_time);
  }

  return info;
}

}  // namespace fl

