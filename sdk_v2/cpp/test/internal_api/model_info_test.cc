// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Round-trip tests for ModelInfo JSON serialization/deserialization.
//
#include "model_info.h"

#include <foundry_local/foundry_local_c.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace fl;

// ========================================================================
// Reasoning fields round-trip
// ========================================================================

TEST(ModelInfoRoundTrip, DetectedRegionSurvivesRoundTrip) {
  ModelInfo original;
  original.model_id = "test-model:1";
  original.name = "test-model";
  original.detected_region = "westus2";

  nlohmann::json j = ModelInfoToJson(original);
  EXPECT_EQ(j["detectedRegion"], "westus2");

  ModelInfo restored = ModelInfoFromJson(j);
  EXPECT_EQ(restored.detected_region, "westus2");
}

TEST(ModelInfoRoundTrip, MissingDetectedRegionOmittedFromJsonAndParsesEmpty) {
  ModelInfo original;
  original.model_id = "test-model:1";
  original.name = "test-model";
  // detected_region left empty (e.g. BYO model or pre-region cache file).

  nlohmann::json j = ModelInfoToJson(original);
  EXPECT_FALSE(j.contains("detectedRegion"));

  // An old cache document without the field must still parse with an empty region.
  ModelInfo restored = ModelInfoFromJson(j);
  EXPECT_TRUE(restored.detected_region.empty());
}

TEST(ModelInfoRoundTrip, ReasoningFieldsSurviveRoundTrip) {
  ModelInfo original;
  original.model_id = "test-model:1";
  original.name = "test-model";
  original.version = 1;
  original.alias = "test";
  original.uri = "local://test";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR] = "<think>";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR] = "</think>";
  original.int_properties[FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT] = 1;

  nlohmann::json j = ModelInfoToJson(original);
  ModelInfo restored = ModelInfoFromJson(j);

  EXPECT_EQ(restored.model_id, "test-model:1");
  EXPECT_EQ(restored.name, "test-model");
  EXPECT_EQ(restored.version, 1);
  EXPECT_EQ(restored.alias, "test");
  EXPECT_EQ(restored.uri, "local://test");

  EXPECT_EQ(restored.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 1);
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR), "<think>");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR), "</think>");
}

// ========================================================================
// Tool calling fields round-trip
// ========================================================================

TEST(ModelInfoRoundTrip, ToolCallingFieldsSurviveRoundTrip) {
  ModelInfo original;
  original.model_id = "test-model:1";
  original.name = "test-model";
  original.version = 1;
  original.alias = "test";
  original.uri = "local://test";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR] = "<|tool_call|>";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR] = "<|/tool_call|>";
  original.int_properties[FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT] = 1;

  nlohmann::json j = ModelInfoToJson(original);
  ModelInfo restored = ModelInfoFromJson(j);

  EXPECT_EQ(restored.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR), "<|tool_call|>");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR), "<|/tool_call|>");
}

// ========================================================================
// All metadata fields round-trip
// ========================================================================

TEST(ModelInfoRoundTrip, AllMetadataFieldsSurviveRoundTrip) {
  ModelInfo original;
  original.model_id = "full-model:5";
  original.name = "full-model";
  original.version = 5;
  original.alias = "full";
  original.uri = "azureml://registries/azureml/models/full-model/versions/5";
  original.device_type = DeviceType::kCPU;
  original.execution_provider = "CPUExecutionProvider";

  // String properties
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR] = "Full Model";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Microsoft";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR] = "MIT";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TASK_STR] = "chat-completion";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR] = "0.5.0";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR] = "AzureFoundry";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR] = "ONNX";

  // Reasoning fields
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR] = "<think>";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR] = "</think>";
  original.int_properties[FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT] = 1;

  // Tool call fields
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR] = "<|tool_call|>";
  original.string_properties[FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR] = "<|/tool_call|>";
  original.int_properties[FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT] = 1;

  // Int properties
  original.int_properties[FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT] = 4096;
  original.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT] = 8192;

  // Prompt templates
  original.prompt_templates.Add("system", "<|system|>\n{Content}<|end|>");
  original.prompt_templates.Add("user", "<|user|>\n{Content}<|end|>");

  // Model settings
  original.model_settings.Add("temperature", "0.7");
  original.model_settings.Add("top_p", "0.9");

  // Round-trip
  nlohmann::json j = ModelInfoToJson(original);
  ModelInfo restored = ModelInfoFromJson(j);

  // Core identity
  EXPECT_EQ(restored.model_id, "full-model:5");
  EXPECT_EQ(restored.name, "full-model");
  EXPECT_EQ(restored.version, 5);
  EXPECT_EQ(restored.alias, "full");
  EXPECT_EQ(restored.uri, "azureml://registries/azureml/models/full-model/versions/5");

  // Runtime
  EXPECT_EQ(restored.device_type, DeviceType::kCPU);
  EXPECT_EQ(restored.execution_provider, "CPUExecutionProvider");

  // String properties
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR), "Full Model");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR), "Microsoft");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR), "MIT");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR), "chat-completion");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR), "0.5.0");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR), "AzureFoundry");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR), "ONNX");

  // Reasoning fields
  EXPECT_EQ(restored.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 1);
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR), "<think>");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR), "</think>");

  // Tool call fields
  EXPECT_EQ(restored.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR), "<|tool_call|>");
  EXPECT_EQ(restored.string_properties.at(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR), "<|/tool_call|>");

  // Int properties
  EXPECT_EQ(restored.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT), 4096);
  EXPECT_EQ(restored.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT), 8192);

  // Prompt templates
  ASSERT_FALSE(restored.prompt_templates.empty());
  EXPECT_EQ(restored.prompt_templates.size(), 2u);

  // Model settings
  ASSERT_FALSE(restored.model_settings.empty());
  EXPECT_EQ(restored.model_settings.size(), 2u);
}

// ========================================================================
// supportsReasoning=false serializes as JSON bool false
// ========================================================================

TEST(ModelInfoRoundTrip, SupportsReasoningFalseSerializesAsBoolFalse) {
  ModelInfo original;
  original.model_id = "test-model:1";
  original.name = "test-model";
  original.version = 1;
  original.alias = "test";
  original.uri = "local://test";
  original.int_properties[FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT] = 0;

  nlohmann::json j = ModelInfoToJson(original);

  // The JSON should contain supportsReasoning as a boolean false
  ASSERT_TRUE(j.contains("supportsReasoning"));
  EXPECT_EQ(j["supportsReasoning"], false);

  // Deserialize back and verify
  ModelInfo restored = ModelInfoFromJson(j);
  EXPECT_EQ(restored.int_properties.at(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 0);
}

// ========================================================================
// Missing reasoning fields are omitted from JSON
// ========================================================================

TEST(ModelInfoRoundTrip, MissingReasoningFieldsOmittedFromJson) {
  ModelInfo original;
  original.model_id = "test-model:1";
  original.name = "test-model";
  original.version = 1;
  original.alias = "test";
  original.uri = "local://test";
  // Intentionally NOT setting any reasoning fields.

  nlohmann::json j = ModelInfoToJson(original);

  EXPECT_FALSE(j.contains("supportsReasoning"));
  EXPECT_FALSE(j.contains("reasoningStart"));
  EXPECT_FALSE(j.contains("reasoningEnd"));
}
