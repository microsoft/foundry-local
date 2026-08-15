// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Unit tests for the public foundry_local::ModelInfo C++ wrapper accessors.
//
// These tests bypass the catalog by constructing an internal fl::ModelInfo
// directly with known property values, exposing it as a flModelInfo opaque
// handle via AsHandle<>, and then wrapping it with the public
// foundry_local::ModelInfo non-owning view. This lets us assert deterministic
// values for the optional/typed accessors without loading a real model.
//
#include "c_api_types.h"
#include "model_info.h"

#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Build a foundry_local::ModelInfo view over an internal fl::ModelInfo.
foundry_local::ModelInfo MakeView(const fl::ModelInfo& info) {
  return foundry_local::ModelInfo(*AsHandle<flModelInfo>(&info));
}

// A minimal fl::ModelInfo populated only with identity fields.
fl::ModelInfo MakeBareInfo() {
  fl::ModelInfo info;
  info.model_id = "test-model:1";
  info.name = "test-model";
  info.version = 1;
  info.alias = "test";
  info.uri = "local://test";
  return info;
}

}  // namespace

// ============================================================================
// ContextLength
// ============================================================================

TEST(ModelInfoAccessors, ContextLengthReturnsValueWhenSet) {
  fl::ModelInfo info = MakeBareInfo();
  info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT] = 8192;

  auto view = MakeView(info);

  auto v = view.ContextLength();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 8192);
}

TEST(ModelInfoAccessors, ContextLengthReturnsNulloptWhenAbsent) {
  fl::ModelInfo info = MakeBareInfo();

  auto view = MakeView(info);

  EXPECT_FALSE(view.ContextLength().has_value());
}

// ============================================================================
// InputModalities / OutputModalities / Capabilities
// ============================================================================

TEST(ModelInfoAccessors, InputModalitiesReturnsValueWhenSet) {
  fl::ModelInfo info = MakeBareInfo();
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR] = "text,image";

  auto view = MakeView(info);

  auto v = view.InputModalities();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, "text,image");
}

TEST(ModelInfoAccessors, InputModalitiesReturnsNulloptWhenAbsent) {
  fl::ModelInfo info = MakeBareInfo();

  auto view = MakeView(info);

  EXPECT_FALSE(view.InputModalities().has_value());
}

TEST(ModelInfoAccessors, OutputModalitiesReturnsValueWhenSet) {
  fl::ModelInfo info = MakeBareInfo();
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR] = "text";

  auto view = MakeView(info);

  auto v = view.OutputModalities();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, "text");
}

TEST(ModelInfoAccessors, OutputModalitiesReturnsNulloptWhenAbsent) {
  fl::ModelInfo info = MakeBareInfo();

  auto view = MakeView(info);

  EXPECT_FALSE(view.OutputModalities().has_value());
}

TEST(ModelInfoAccessors, CapabilitiesReturnsValueWhenSet) {
  fl::ModelInfo info = MakeBareInfo();
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_CAPABILITIES_STR] = "chat,tool-calling,reasoning";

  auto view = MakeView(info);

  auto v = view.Capabilities();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, "chat,tool-calling,reasoning");
}

TEST(ModelInfoAccessors, CapabilitiesReturnsNulloptWhenAbsent) {
  fl::ModelInfo info = MakeBareInfo();

  auto view = MakeView(info);

  EXPECT_FALSE(view.Capabilities().has_value());
}

// ============================================================================
// GetRuntime
// ============================================================================

TEST(ModelInfoAccessors, GetRuntimeReturnsNulloptWhenDeviceNotSet) {
  fl::ModelInfo info = MakeBareInfo();
  // Leave device_type at default (kNotSet).

  auto view = MakeView(info);

  EXPECT_EQ(view.DeviceType(), FOUNDRY_LOCAL_DEVICE_NOTSET);
  EXPECT_FALSE(view.GetRuntime().has_value());
}

TEST(ModelInfoAccessors, GetRuntimeReturnsAggregateForCpuWithExecutionProvider) {
  fl::ModelInfo info = MakeBareInfo();
  info.device_type = fl::DeviceType::kCPU;
  info.execution_provider = "CPUExecutionProvider";

  auto view = MakeView(info);

  auto runtime = view.GetRuntime();
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(runtime->device_type, FOUNDRY_LOCAL_DEVICE_CPU);
  ASSERT_TRUE(runtime->execution_provider.has_value());
  EXPECT_EQ(*runtime->execution_provider, "CPUExecutionProvider");

  // Aggregate must agree with the individual accessors.
  EXPECT_EQ(runtime->device_type, view.DeviceType());
  ASSERT_TRUE(view.ExecutionProvider().has_value());
  EXPECT_EQ(*runtime->execution_provider, *view.ExecutionProvider());
}

TEST(ModelInfoAccessors, GetRuntimeOmitsExecutionProviderWhenEmpty) {
  fl::ModelInfo info = MakeBareInfo();
  info.device_type = fl::DeviceType::kGPU;
  // execution_provider intentionally left empty.

  auto view = MakeView(info);

  auto runtime = view.GetRuntime();
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(runtime->device_type, FOUNDRY_LOCAL_DEVICE_GPU);
  EXPECT_FALSE(runtime->execution_provider.has_value());
  EXPECT_FALSE(view.ExecutionProvider().has_value());
}

// ============================================================================
// GetModelSettings
// ============================================================================

TEST(ModelInfoAccessors, GetModelSettingsReturnsPopulatedView) {
  fl::ModelInfo info = MakeBareInfo();
  info.model_settings.Add("temperature", "0.7");
  info.model_settings.Add("top_p", "0.9");

  auto view = MakeView(info);

  auto settings = view.GetModelSettings();
  ASSERT_TRUE(settings.has_value());

  auto entries = settings->GetAll();
  EXPECT_EQ(entries.size(), 2u);

  // Build a sorted (key,value) snapshot for order-independent comparison.
  std::vector<std::pair<std::string, std::string>> seen;
  seen.reserve(entries.size());
  for (const auto& e : entries) {
    seen.emplace_back(std::string(e.key), std::string(e.value));
  }
  std::sort(seen.begin(), seen.end());

  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0].first, "temperature");
  EXPECT_EQ(seen[0].second, "0.7");
  EXPECT_EQ(seen[1].first, "top_p");
  EXPECT_EQ(seen[1].second, "0.9");

  // Existing keyed accessor must remain consistent with the view.
  auto temp = view.GetModelSetting("temperature");
  ASSERT_TRUE(temp.has_value());
  EXPECT_EQ(*temp, "0.7");

  auto top_p = view.GetModelSetting("top_p");
  ASSERT_TRUE(top_p.has_value());
  EXPECT_EQ(*top_p, "0.9");

  EXPECT_FALSE(view.GetModelSetting("missing").has_value());
}

TEST(ModelInfoAccessors, GetModelSettingsReturnsNulloptWhenEmpty) {
  fl::ModelInfo info = MakeBareInfo();
  // model_settings intentionally left empty.

  auto view = MakeView(info);

  EXPECT_FALSE(view.GetModelSettings().has_value());
  EXPECT_FALSE(view.GetModelSetting("temperature").has_value());
}

// ============================================================================
// SupportsToolCalling
// ============================================================================

TEST(ModelInfoAccessors, SupportsToolCallingReturnsNulloptWhenAbsent) {
  fl::ModelInfo info = MakeBareInfo();

  auto view = MakeView(info);

  EXPECT_FALSE(view.SupportsToolCalling().has_value());
}

TEST(ModelInfoAccessors, SupportsToolCallingReturnsTrueWhenSetTrue) {
  fl::ModelInfo info = MakeBareInfo();
  info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT] = 1;

  auto view = MakeView(info);

  auto v = view.SupportsToolCalling();
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(*v);
}

TEST(ModelInfoAccessors, SupportsToolCallingReturnsFalseWhenSetFalse) {
  fl::ModelInfo info = MakeBareInfo();
  info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT] = 0;

  auto view = MakeView(info);

  auto v = view.SupportsToolCalling();
  ASSERT_TRUE(v.has_value());
  EXPECT_FALSE(*v);
}

// ============================================================================
// FilesizeMb
// ============================================================================

TEST(ModelInfoAccessors, FilesizeMbReturnsNulloptWhenAbsent) {
  fl::ModelInfo info = MakeBareInfo();

  auto view = MakeView(info);

  EXPECT_FALSE(view.FilesizeMb().has_value());
}

TEST(ModelInfoAccessors, FilesizeMbReturnsValueWhenSet) {
  fl::ModelInfo info = MakeBareInfo();
  info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT] = 4096;

  auto view = MakeView(info);

  auto v = view.FilesizeMb();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 4096);
}

TEST(ModelInfoAccessors, FilesizeMbReturnsZeroWhenSetZero) {
  fl::ModelInfo info = MakeBareInfo();
  info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT] = 0;

  auto view = MakeView(info);

  auto v = view.FilesizeMb();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 0);
}

// ============================================================================
// MaxOutputTokens
// ============================================================================

TEST(ModelInfoAccessors, MaxOutputTokensReturnsNulloptWhenAbsent) {
  fl::ModelInfo info = MakeBareInfo();

  auto view = MakeView(info);

  EXPECT_FALSE(view.MaxOutputTokens().has_value());
}

TEST(ModelInfoAccessors, MaxOutputTokensReturnsValueWhenSet) {
  fl::ModelInfo info = MakeBareInfo();
  info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT] = 8192;

  auto view = MakeView(info);

  auto v = view.MaxOutputTokens();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 8192);
}

// ============================================================================
// IsTestModel
// ============================================================================

TEST(ModelInfoAccessors, IsTestModelDefaultsToFalse) {
  fl::ModelInfo info = MakeBareInfo();

  auto view = MakeView(info);

  EXPECT_FALSE(view.IsTestModel());
}

TEST(ModelInfoAccessors, IsTestModelReturnsTrueWhenSet) {
  fl::ModelInfo info = MakeBareInfo();
  info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_IS_TEST_MODEL_INT] = 1;

  auto view = MakeView(info);

  EXPECT_TRUE(view.IsTestModel());
}

TEST(ModelInfoAccessors, IsTestModelReturnsFalseWhenSetZero) {
  fl::ModelInfo info = MakeBareInfo();
  info.int_properties[FOUNDRY_LOCAL_MODEL_PROP_IS_TEST_MODEL_INT] = 0;

  auto view = MakeView(info);

  EXPECT_FALSE(view.IsTestModel());
}
