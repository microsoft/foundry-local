// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "model_info.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// ========================================================================
// JSON models for the Azure Foundry catalog REST API response.
// Mirrors the C# types in AzureFoundryModels.cs — we only define fields
// we actually use. Everything is optional because the catalog may omit
// fields at will.
// ========================================================================

// --- Request types ---

struct CatalogFilter {
  std::string field;
  std::string op;  // "eq", etc.
  std::vector<std::string> values;
};

struct CatalogResource {
  std::string resource_id;
  std::string entity_container_type;
};

struct IndexEntitiesRequest {
  std::vector<CatalogFilter> filters;
  int page_size = 50;
  std::optional<int> skip;
  std::optional<std::string> continuation_token;
};

struct AzureCatalogRequest {
  std::vector<CatalogResource> resource_ids;
  IndexEntitiesRequest index_entities_request;
};

// --- Response types ---

/// Variant metadata nested inside Properties → VariantInfo.
struct VariantMetadata {
  std::optional<std::string> model_type;
  std::optional<std::string> device;
  std::optional<std::string> execution_provider;
  std::optional<int64_t> file_size_bytes;
};

struct VariantParent {
  std::optional<std::string> asset_id;
};

struct VariantInfo {
  std::vector<VariantParent> parents;
  std::optional<VariantMetadata> variant_metadata;
};

struct CreationContext {
  std::optional<std::string> created_time;  // ISO 8601 datetime string
};

struct CatalogProperties {
  std::optional<std::string> id;
  std::optional<std::string> name;
  std::optional<int64_t> version;
  std::optional<VariantInfo> variant_info;
  std::optional<std::string> min_fl_version;
  std::optional<CreationContext> creation_context;
};

/// Catalog Tags inside Annotations.
struct CatalogTags {
  std::optional<std::string> alias;
  std::optional<std::string> foundry_local;
  std::optional<std::string> prompt_template;  // JSON string to parse separately
  std::optional<std::string> task;
  std::optional<std::string> license;
  std::optional<std::string> license_description;
  std::optional<std::string> supports_tool_calling;
  std::optional<std::string> tool_call_start;
  std::optional<std::string> tool_call_end;
  std::optional<std::string> supports_reasoning;
  std::optional<std::string> reasoning_start;
  std::optional<std::string> reasoning_end;
  std::optional<std::string> max_output_tokens;
};

struct SystemCatalogData {
  std::optional<std::string> publisher;
  std::optional<std::string> display_name;
  std::optional<int> max_output_tokens;
};

struct CatalogAnnotations {
  std::optional<CatalogTags> tags;
  std::optional<SystemCatalogData> system_catalog_data;
};

/// A single model entry in the catalog response.
struct CatalogLocalModel {
  std::optional<std::string> asset_id;
  std::optional<std::string> entity_id;
  std::optional<CatalogAnnotations> annotations;
  std::optional<CatalogProperties> properties;
};

struct IndexEntitiesResponse {
  std::optional<int> total_count;
  std::vector<CatalogLocalModel> models;
  std::optional<int> next_skip;
  std::optional<std::string> continuation_token;
};

struct AzureCatalogResponse {
  std::optional<IndexEntitiesResponse> index_entities_response;
};

// ========================================================================
// Prompt template parsed from the Tags.PromptTemplate JSON string.
// ========================================================================

struct CatalogPromptTemplate {
  std::optional<std::string> system;
  std::optional<std::string> user;
  std::optional<std::string> assistant;
  std::optional<std::string> prompt;
};

// ========================================================================
// JSON serialization (nlohmann)
// ========================================================================

// --- Request serialization (to_json) ---

void to_json(nlohmann::json& j, const CatalogFilter& f);
void to_json(nlohmann::json& j, const CatalogResource& r);
void to_json(nlohmann::json& j, const IndexEntitiesRequest& r);
void to_json(nlohmann::json& j, const AzureCatalogRequest& r);

// --- Response deserialization (from_json) ---

void from_json(const nlohmann::json& j, VariantMetadata& v);
void from_json(const nlohmann::json& j, VariantParent& v);
void from_json(const nlohmann::json& j, VariantInfo& v);
void from_json(const nlohmann::json& j, CreationContext& c);
void from_json(const nlohmann::json& j, CatalogProperties& p);
void from_json(const nlohmann::json& j, CatalogTags& t);
void from_json(const nlohmann::json& j, SystemCatalogData& s);
void from_json(const nlohmann::json& j, CatalogAnnotations& a);
void from_json(const nlohmann::json& j, CatalogLocalModel& m);
void from_json(const nlohmann::json& j, IndexEntitiesResponse& r);
void from_json(const nlohmann::json& j, AzureCatalogResponse& r);
void from_json(const nlohmann::json& j, CatalogPromptTemplate& t);

// ========================================================================
// Conversion: CatalogLocalModel → ModelInfo
// ========================================================================

/// Converts a catalog API model entry into our internal ModelInfo.
/// Returns nullopt if the entry is invalid (missing required fields).
std::optional<ModelInfo> CatalogModelToModelInfo(const CatalogLocalModel& catalog_model);

}  // namespace fl
