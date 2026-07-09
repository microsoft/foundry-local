// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_catalog_models.h"

#include "util/json_helpers.h"
#include "utils.h"

#include <foundry_local/foundry_local_c.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>

namespace fl {

namespace {

/// Extract short model name from parent asset URI.
/// Pattern: find "/models/" then capture everything until the next "/".
std::string ExtractShortName(const std::string& parent_model_uri) {
  // Use a capture group rather than lookbehind: ECMAScript regex on libstdc++/libc++
  // does not implement lookbehind and would throw at construction.
  static const std::regex pattern(R"(/models/([^/]+))");
  std::smatch match;
  if (std::regex_search(parent_model_uri, match, pattern)) {
    return match[1].str();
  }
  return {};
}

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

void to_json(nlohmann::json& j, const CatalogResource& r) {
  j = nlohmann::json{
      {"resourceId", r.resource_id},
      {"entityContainerType", r.entity_container_type},
  };
}

void to_json(nlohmann::json& j, const IndexEntitiesRequest& r) {
  j = nlohmann::json{
      {"filters", r.filters},
      {"pageSize", r.page_size},
  };

  if (r.skip.has_value()) {
    j["skip"] = *r.skip;
  }

  if (r.continuation_token.has_value()) {
    j["continuationToken"] = *r.continuation_token;
  }
}

void to_json(nlohmann::json& j, const AzureCatalogRequest& r) {
  j = nlohmann::json{
      {"resourceIds", r.resource_ids},
      {"indexEntitiesRequest", r.index_entities_request},
  };
}

// ========================================================================
// Response deserialization (from_json)
// ========================================================================

void from_json(const nlohmann::json& j, VariantMetadata& v) {
  opt_str(j, "modelType", v.model_type);
  opt_str(j, "device", v.device);
  opt_str(j, "executionProvider", v.execution_provider);
  opt_int64(j, "fileSizeBytes", v.file_size_bytes);
}

void from_json(const nlohmann::json& j, VariantParent& v) {
  opt_str(j, "assetId", v.asset_id);
}

void from_json(const nlohmann::json& j, VariantInfo& v) {
  if (j.contains("parents") && j["parents"].is_array()) {
    v.parents = j["parents"].get<std::vector<VariantParent>>();
  }

  if (j.contains("variantMetadata") && j["variantMetadata"].is_object()) {
    v.variant_metadata = j["variantMetadata"].get<VariantMetadata>();
  }
}

void from_json(const nlohmann::json& j, CreationContext& c) {
  opt_str(j, "createdTime", c.created_time);
}

void from_json(const nlohmann::json& j, CatalogProperties& p) {
  opt_str(j, "id", p.id);
  opt_str(j, "name", p.name);
  opt_int64(j, "version", p.version);
  opt_str(j, "minFLVersion", p.min_fl_version);

  if (j.contains("variantInfo") && j["variantInfo"].is_object()) {
    p.variant_info = j["variantInfo"].get<VariantInfo>();
  }

  if (j.contains("creationContext") && j["creationContext"].is_object()) {
    p.creation_context = j["creationContext"].get<CreationContext>();
  }
}

void from_json(const nlohmann::json& j, CatalogTags& t) {
  opt_str(j, "alias", t.alias);
  opt_str(j, "foundryLocal", t.foundry_local);
  opt_str(j, "promptTemplate", t.prompt_template);
  opt_str(j, "task", t.task);
  opt_str(j, "license", t.license);
  opt_str(j, "licenseDescription", t.license_description);
  opt_str(j, "supportsToolCalling", t.supports_tool_calling);
  opt_str(j, "toolCallStart", t.tool_call_start);
  opt_str(j, "toolCallEnd", t.tool_call_end);
  opt_str(j, "supportsReasoning", t.supports_reasoning);
  opt_str(j, "reasoningStart", t.reasoning_start);
  opt_str(j, "reasoningEnd", t.reasoning_end);
  opt_str(j, "maxOutputTokens", t.max_output_tokens);
}

void from_json(const nlohmann::json& j, SystemCatalogData& s) {
  opt_str(j, "publisher", s.publisher);
  opt_str(j, "displayName", s.display_name);
  opt_int(j, "maxOutputTokens", s.max_output_tokens);
}

void from_json(const nlohmann::json& j, CatalogAnnotations& a) {
  if (j.contains("tags") && j["tags"].is_object()) {
    a.tags = j["tags"].get<CatalogTags>();
  }

  if (j.contains("systemCatalogData") && j["systemCatalogData"].is_object()) {
    a.system_catalog_data = j["systemCatalogData"].get<SystemCatalogData>();
  }
}

void from_json(const nlohmann::json& j, CatalogLocalModel& m) {
  opt_str(j, "assetId", m.asset_id);
  opt_str(j, "entityId", m.entity_id);

  if (j.contains("annotations") && j["annotations"].is_object()) {
    m.annotations = j["annotations"].get<CatalogAnnotations>();
  }

  if (j.contains("properties") && j["properties"].is_object()) {
    m.properties = j["properties"].get<CatalogProperties>();
  }
}

void from_json(const nlohmann::json& j, IndexEntitiesResponse& r) {
  opt_int(j, "totalCount", r.total_count);
  opt_int(j, "nextSkip", r.next_skip);
  opt_str(j, "continuationToken", r.continuation_token);

  if (j.contains("value") && j["value"].is_array()) {
    r.models = j["value"].get<std::vector<CatalogLocalModel>>();
  }
}

void from_json(const nlohmann::json& j, AzureCatalogResponse& r) {
  if (j.contains("indexEntitiesResponse") && j["indexEntitiesResponse"].is_object()) {
    r.index_entities_response = j["indexEntitiesResponse"].get<IndexEntitiesResponse>();
  }
}

void from_json(const nlohmann::json& j, CatalogPromptTemplate& t) {
  opt_str(j, "system", t.system);
  opt_str(j, "user", t.user);
  opt_str(j, "assistant", t.assistant);
  opt_str(j, "prompt", t.prompt);
}

// ========================================================================
// CatalogLocalModel → ModelInfo conversion
// Mirrors C# LocalModelHelper.ToAzureFoundryLocalModel
// ========================================================================

std::optional<ModelInfo> CatalogModelToModelInfo(const CatalogLocalModel& cm) {
  // Required fields — skip entry if missing.
  if (!cm.asset_id || cm.asset_id->empty()) {
    return std::nullopt;
  }

  if (!cm.properties || !cm.properties->name || cm.properties->name->empty()) {
    return std::nullopt;
  }

  if (!cm.entity_id || cm.entity_id->empty()) {
    return std::nullopt;
  }

  if (!cm.properties->variant_info) {
    return std::nullopt;
  }

  const auto& props = *cm.properties;
  int version = static_cast<int>(props.version.value_or(0));

  // Extract parent model URI (used for alias and stored as a property).
  std::string parent_uri;
  if (props.variant_info && !props.variant_info->parents.empty() &&
      props.variant_info->parents[0].asset_id) {
    parent_uri = *props.variant_info->parents[0].asset_id;
  }

  // Determine alias — prefer tags.alias, then short name from parent, then model name.
  std::string alias;
  if (cm.annotations && cm.annotations->tags && cm.annotations->tags->alias) {
    alias = *cm.annotations->tags->alias;
  } else {
    if (!parent_uri.empty()) {
      alias = ExtractShortName(parent_uri);
    }

    if (alias.empty()) {
      alias = *props.name;
    }
  }

  ModelInfo info;
  info.model_id = *props.name + ":" + std::to_string(version);
  info.name = *props.name;
  info.version = version;
  info.alias = alias;
  info.uri = *cm.asset_id;

  // Device type, execution provider, and model type from variant metadata
  if (props.variant_info && props.variant_info->variant_metadata) {
    const auto& vm = *props.variant_info->variant_metadata;
    if (vm.device) {
      info.device_type = ParseDeviceType(*vm.device);
    }

    if (vm.execution_provider) {
      info.execution_provider = *vm.execution_provider;
    }

    // ModelType — defaults to "ONNX" (matches C# ToAzureFoundryLocalModel)
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR] =
        vm.model_type.value_or("ONNX");
  } else {
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR] = "ONNX";
  }

  // Prompt templates — parse the JSON string from tags
  if (cm.annotations && cm.annotations->tags &&
      cm.annotations->tags->prompt_template && !cm.annotations->tags->prompt_template->empty()) {
    try {
      auto pt_json = nlohmann::json::parse(*cm.annotations->tags->prompt_template);
      auto pt = pt_json.get<CatalogPromptTemplate>();

      if (pt.prompt) {
        info.prompt_templates.Add("prompt", *pt.prompt);
      }

      if (pt.system) {
        info.prompt_templates.Add("system", *pt.system);
      }

      if (pt.user) {
        info.prompt_templates.Add("user", *pt.user);
      }

      if (pt.assistant) {
        info.prompt_templates.Add("assistant", *pt.assistant);
      }
    } catch (...) {
      // Malformed prompt template JSON — skip it
    }
  }

  // String properties — use FOUNDRY_LOCAL_MODEL_PROP_* keys for consistency with the public C API.
  if (cm.annotations && cm.annotations->tags) {
    const auto& tags = *cm.annotations->tags;

    // Helper: parse a catalog string tag ("true"/"false"/"1"/"0") into int_properties.
    // Skips unrecognized values — no property set.
    auto set_bool_int = [&](const std::optional<std::string>& tag, const char* key) {
      if (!tag) {
        return;
      }

      const auto v = ToLower(*tag);
      if (v == "true") {
        info.int_properties[key] = 1;
      } else if (v == "false") {
        info.int_properties[key] = 0;
      }
    };

    if (tags.task) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TASK_STR] = *tags.task;
      info.task = *tags.task;
    }

    if (tags.license) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR] = *tags.license;
    }

    if (tags.license_description) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR] = *tags.license_description;
    }

    set_bool_int(tags.supports_tool_calling, FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT);

    if (tags.tool_call_start) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR] = *tags.tool_call_start;
    }

    if (tags.tool_call_end) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR] = *tags.tool_call_end;
    }

    set_bool_int(tags.supports_reasoning, FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT);

    if (tags.reasoning_start) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR] = *tags.reasoning_start;
    }

    if (tags.reasoning_end) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR] = *tags.reasoning_end;
    }
  }

  if (cm.annotations && cm.annotations->system_catalog_data) {
    const auto& scd = *cm.annotations->system_catalog_data;
    if (scd.publisher) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = *scd.publisher;
    }

    if (scd.display_name) {
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR] = *scd.display_name;
    }

    if (scd.max_output_tokens) {
      info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT] = *scd.max_output_tokens;
    }
  }

  // Fallback: tags.maxOutputTokens (string form) if systemCatalogData didn't supply one.
  if (info.int_properties.find(FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT) == info.int_properties.end() &&
      cm.annotations && cm.annotations->tags && cm.annotations->tags->max_output_tokens) {
    try {
      info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT] =
          std::stoi(*cm.annotations->tags->max_output_tokens);
    } catch (...) {
      // Malformed value — ignore.
    }
  }

  if (props.min_fl_version) {
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR] = *props.min_fl_version;
  }

  // File size in MB
  if (props.variant_info && props.variant_info->variant_metadata &&
      props.variant_info->variant_metadata->file_size_bytes) {
    int64_t bytes = *props.variant_info->variant_metadata->file_size_bytes;
    info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT] = bytes / (1024 * 1024);
  }

  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR] = "AzureFoundry";

  // Parent model URI
  if (!parent_uri.empty()) {
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PARENT_URI_STR] = parent_uri;
  }

  // CreatedAtUnix — Properties.CreationContext.CreatedTime → Unix timestamp
  if (props.creation_context && props.creation_context->created_time) {
    int64_t unix_ts = ParseIso8601ToUnix(*props.creation_context->created_time);
    info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT] = unix_ts;
  }

  // TestModel — tags.foundryLocal == "test" (case-insensitive)
  if (cm.annotations && cm.annotations->tags && cm.annotations->tags->foundry_local) {
    const auto& fl_tag = *cm.annotations->tags->foundry_local;
    // Trim and compare case-insensitively
    bool is_test = (fl_tag == "test" || fl_tag == "Test" || fl_tag == "TEST");
    if (is_test) {
      info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_IS_TEST_MODEL_INT] = 1;
    }
  }

  return info;
}

}  // namespace fl
