// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "model_info.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// ========================================================================
// JSON models for the Azure Foundry Asset Gallery catalog REST API
// (`asset-gallery/v1.0/models`). Production `value` records use nested
// `properties` and `annotations`; flat `summaries` records expose the same
// metadata as top-level fields. Everything is optional because the catalog
// may omit fields at will.
// ========================================================================

// --- Request types ---

struct CatalogFilter {
  std::string field;
  std::string op;  // "eq", etc.
  std::vector<std::string> values;
};

struct AzureCatalogRequest {
  std::vector<CatalogFilter> filters;
  int page_size = 50;
  std::optional<std::string> continuation_token;
};

// --- Response types ---

struct CatalogModelPackageVariant {
  std::optional<std::string> name;
  std::optional<std::string> execution_provider;
  std::optional<std::string> device;
  std::optional<std::string> compatibility_string;
};

struct CatalogModelPackageMetadata {
  std::optional<int> schema_version;
  std::vector<CatalogModelPackageVariant> variants;
};

struct TextLimits {
  std::optional<int64_t> input_context_window;
  std::optional<int64_t> max_output_tokens;
};

struct ModelLimits {
  std::optional<TextLimits> text_limits;
  std::vector<std::string> supported_input_modalities;
  std::vector<std::string> supported_output_modalities;
};

/// Variant metadata nested inside VariantInformation.
struct VariantMetadata {
  std::optional<std::string> model_type;
  std::vector<std::string> quantization;
  std::optional<std::string> device;
  std::optional<std::string> execution_provider;
  std::optional<int64_t> file_size_bytes;
  std::optional<std::string> model_format;
  std::optional<CatalogModelPackageMetadata> model_package;
};

struct VariantParent {
  std::optional<std::string> asset_id;
};

struct VariantInformation {
  std::vector<VariantParent> parents;
  std::optional<VariantMetadata> variant_metadata;
};

/// A single nested `value` or flat `summaries` entry. Records without the
/// required identity or variant information are skipped during conversion.
struct CatalogLocalModel {
  // Populated from the response's azureml-served-by-cluster header, not JSON.
  std::string detected_region;
  std::optional<std::string> asset_id;
  std::optional<std::string> name;
  std::optional<std::string> alias;
  std::optional<std::string> display_name;
  std::optional<std::string> version;  // numeric string, e.g. "4"
  std::optional<std::string> publisher;
  std::optional<std::string> license;
  std::optional<std::string> license_description;
  std::optional<std::string> min_fl_version;
  std::optional<bool> supports_tool_calling;
  std::optional<bool> supports_reasoning;
  std::optional<bool> is_test_model;
  std::optional<std::string> foundry_local;
  std::optional<std::string> created_time;
  std::vector<std::string> inference_tasks;
  std::vector<std::string> model_capabilities;
  std::vector<std::string> deployment_options;
  bool has_deployment_options = false;
  std::optional<ModelLimits> model_limits;
  std::optional<VariantInformation> variant_information;
};

struct AzureCatalogResponse {
  std::optional<int> total_count;
  std::optional<std::string> continuation_token;
  std::vector<CatalogLocalModel> models;
  std::size_t skipped_record_count = 0;
};

// ========================================================================
// JSON serialization (nlohmann)
// ========================================================================

// --- Request serialization (to_json) ---

void to_json(nlohmann::json& j, const CatalogFilter& f);
void to_json(nlohmann::json& j, const AzureCatalogRequest& r);

// --- Response deserialization (from_json) ---

void from_json(const nlohmann::json& j, CatalogModelPackageVariant& v);
void from_json(const nlohmann::json& j, CatalogModelPackageMetadata& p);
void from_json(const nlohmann::json& j, TextLimits& t);
void from_json(const nlohmann::json& j, ModelLimits& m);
void from_json(const nlohmann::json& j, VariantMetadata& v);
void from_json(const nlohmann::json& j, VariantParent& v);
void from_json(const nlohmann::json& j, VariantInformation& v);
void from_json(const nlohmann::json& j, CatalogLocalModel& m);
void from_json(const nlohmann::json& j, AzureCatalogResponse& r);

// ========================================================================
// Conversion: CatalogLocalModel → ModelInfo
// ========================================================================

/// Converts a catalog API model entry into our internal ModelInfo.
/// Returns nullopt if the entry is invalid, has no alias, or has no variant
/// information (i.e. it's the abstract parent model rather than a runnable variant).
std::optional<ModelInfo> CatalogModelToModelInfo(const CatalogLocalModel& catalog_model);

}  // namespace fl
