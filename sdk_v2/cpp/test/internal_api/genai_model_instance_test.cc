// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/genai_model_instance.h"
#include "ep_detection/ep_detector.h"
#include "inferencing/model_load_manager.h"
#include "internal_api/test_helpers.h"
#include "logger.h"
#include "utils/safe_getenv.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace {

class CpuCudaEpDetector : public fl::IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {
        {"CPU", {"CPUExecutionProvider"}},
        {"GPU", {"CUDAExecutionProvider"}},
    };
  }

  bool PrepareForModelLoad(std::string_view) override { return true; }
};

constexpr std::string_view kQwenToolCallProjection =
    "<|im_start|>user\ncapability-probe<|im_end|>\n"
    "<|im_start|>assistant\n"
    "<think>\n\n</think>\n\n"
    "<tool_call>\n"
    "<function=probe_function>\n"
    "<parameter=probe_parameter>\n"
    "probe_value\n"
    "</parameter>\n"
    "</function>\n"
    "</tool_call><|im_end|>\n";
constexpr std::string_view kQwenToolResultProjection =
    "<|im_start|>user\ncapability-probe<|im_end|>\n"
    "<|im_start|>assistant\n"
    "<think>\n\n</think>\n\n"
    "<tool_call>\n"
    "<function=probe_first>\n"
    "</function>\n"
    "</tool_call>\n"
    "<tool_call>\n"
    "<function=probe_second>\n"
    "</function>\n"
    "</tool_call><|im_end|>\n"
    "<|im_start|>user\n"
    "<tool_response>\n"
    "probe-result-first\n"
    "</tool_response>\n"
    "<tool_response>\n"
    "probe-result-second\n"
    "</tool_response><|im_end|>\n";
constexpr std::string_view kQwenReasoningProjection =
    "<|im_start|>assistant\n<think>\nreasoning-probe-private-a\n</think>\n\n"
    "reasoning-probe-visible-a<|im_end|>\n"
    "<|im_start|>assistant\n<think>\nreasoning-probe-private-b\n</think>\n\n"
    "reasoning-probe-visible-b<|im_end|>\n";
constexpr std::string_view kQwenNoPreserveReasoningProjection =
    "<|im_start|>assistant\nreasoning-probe-visible-a<|im_end|>\n"
    "<|im_start|>assistant\nreasoning-probe-visible-b<|im_end|>\n";

std::filesystem::path FindModelConfigDirectory(
    const std::filesystem::path& package_path) {
  constexpr std::string_view kConfigFile = "genai_config.json";
  if (std::filesystem::exists(package_path / kConfigFile)) {
    return package_path;
  }

  if (std::filesystem::is_directory(package_path)) {
    for (const auto& entry : std::filesystem::directory_iterator(package_path)) {
      if (entry.is_directory() &&
          std::filesystem::exists(entry.path() / kConfigFile)) {
        return entry.path();
      }
    }
  }

  return {};
}

TEST(ModelCapabilitiesTest, ExactQwen35TextTypeAndTemplateEnableBothCapabilities) {
  const auto capabilities = fl::model_capabilities_internal::ResolveRenderedProbes(
      "qwen3_5_text", kQwenToolCallProjection, kQwenToolResultProjection,
      "<tool_call>", "</tool_call>");

  EXPECT_TRUE(capabilities.supports_tool_calling);
  EXPECT_TRUE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_TRUE(capabilities.positional_tool_results);
}

TEST(ModelCapabilitiesTest, ExactQwen35TextTypeWithNonmatchingTemplateDisablesBothCapabilities) {
  const auto capabilities =
      fl::model_capabilities_internal::ResolveRenderedProbes(
          "qwen3_5_text", "not-qwen", "not-qwen", "<tool_call>", "</tool_call>");

  EXPECT_FALSE(capabilities.supports_tool_calling);
  EXPECT_FALSE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_FALSE(capabilities.positional_tool_results);
}

class UnsupportedModelCapabilitiesTest : public ::testing::TestWithParam<std::string_view> {};

TEST_P(UnsupportedModelCapabilitiesTest, ExactQwenTemplateOnboardsCompatibleModelTypes) {
  const auto capabilities = fl::model_capabilities_internal::ResolveRenderedProbes(
      GetParam(), kQwenToolCallProjection, kQwenToolResultProjection,
      "<tool_call>", "</tool_call>");

  EXPECT_TRUE(capabilities.supports_tool_calling);
  EXPECT_FALSE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_TRUE(capabilities.positional_tool_results);
}

INSTANTIATE_TEST_SUITE_P(
    UnsupportedModelTypes,
    UnsupportedModelCapabilitiesTest,
    ::testing::Values("qwen3_5", "qwen3", "qwen3_moe", "qwen3_5_moe", "qwen3_vl", "qwen3_5_vl", "unknown", ""));

TEST(ModelCapabilitiesTest, ResultProjectionMismatchDisablesOnlyPositionalToolResults) {
  const auto capabilities = fl::model_capabilities_internal::ResolveRenderedProbes(
      "qwen3_5_text", kQwenToolCallProjection, "not-qwen",
      "<tool_call>", "</tool_call>");

  EXPECT_TRUE(capabilities.supports_tool_calling);
  EXPECT_TRUE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_FALSE(capabilities.positional_tool_results);
}

TEST(ModelCapabilitiesTest, MissingResultProjectionPreservesToolCallSupport) {
  const auto capabilities = fl::model_capabilities_internal::ResolveRenderedProbes(
      "qwen3_5_text", kQwenToolCallProjection, {},
      "<tool_call>", "</tool_call>");

  EXPECT_TRUE(capabilities.supports_tool_calling);
  EXPECT_TRUE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_FALSE(capabilities.positional_tool_results);
}

TEST(ModelCapabilitiesTest, GenericTaggedJsonProtocolIsOnboarded) {
  constexpr std::string_view kToolCallProjection =
      "<|tool_start|>{\"name\":\"probe_function\",\"arguments\":"
      "{\"probe_parameter\":\"probe_value\"}}<|tool_end|>";
  constexpr std::string_view kToolResultProjection =
      "<|tool_start|>{\"name\":\"probe_first\"}<|tool_end|>"
      "<|tool_start|>{\"name\":\"probe_second\"}<|tool_end|>"
      "probe-result-first probe-result-second";

  const auto capabilities = fl::model_capabilities_internal::ResolveRenderedProbes(
      "custom", kToolCallProjection, kToolResultProjection,
      "<|tool_start|>", "<|tool_end|>");

  EXPECT_TRUE(capabilities.supports_tool_calling);
  EXPECT_FALSE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_TRUE(capabilities.positional_tool_results);
}

TEST(ModelCapabilitiesTest, MissingOrAmbiguousBoundariesFailClosed) {
  const auto missing_end = fl::model_capabilities_internal::ResolveRenderedProbes(
      "custom", kQwenToolCallProjection, kQwenToolResultProjection,
      "<tool_call>", "<missing>");
  const auto identical_boundaries = fl::model_capabilities_internal::ResolveRenderedProbes(
      "custom", kQwenToolCallProjection, kQwenToolResultProjection,
      "<tool_call>", "<tool_call>");

  EXPECT_FALSE(missing_end.supports_tool_calling);
  EXPECT_FALSE(missing_end.positional_tool_results);
  EXPECT_FALSE(identical_boundaries.supports_tool_calling);
  EXPECT_FALSE(identical_boundaries.positional_tool_results);
}

TEST(ModelCapabilitiesTest, SplitOrOutOfBlockPayloadFailsClosed) {
  constexpr std::string_view kSplitPayload =
      "<tool_call>probe_function</tool_call>"
      "<tool_call>probe_parameter probe_value</tool_call>";
  constexpr std::string_view kOutOfBlockPayload =
      "<tool_call>probe_function</tool_call>"
      "probe_parameter probe_value"
      "<tool_call>unrelated</tool_call>";
  constexpr std::string_view kNestedPayload =
      "<tool_call><tool_call>probe_function probe_parameter probe_value</tool_call></tool_call>";

  const auto split = fl::model_capabilities_internal::ResolveRenderedProbes(
      "custom", kSplitPayload, {}, "<tool_call>", "</tool_call>");
  const auto outside = fl::model_capabilities_internal::ResolveRenderedProbes(
      "custom", kOutOfBlockPayload, {}, "<tool_call>", "</tool_call>");
  const auto nested = fl::model_capabilities_internal::ResolveRenderedProbes(
      "custom", kNestedPayload, {}, "<tool_call>", "</tool_call>");

  EXPECT_FALSE(split.supports_tool_calling);
  EXPECT_FALSE(outside.supports_tool_calling);
  EXPECT_FALSE(nested.supports_tool_calling);
}

TEST(ModelCapabilitiesTest, IdCorrelatedResultsDoNotEnablePositionalProjection) {
  constexpr std::string_view kIdCorrelatedResults =
      "probe_first probe_second probe-call-a probe-result-second "
      "probe-call-b probe-result-first";
  const auto capabilities = fl::model_capabilities_internal::ResolveRenderedProbes(
      "custom", kQwenToolCallProjection, kIdCorrelatedResults,
      "<tool_call>", "</tool_call>");

  EXPECT_TRUE(capabilities.supports_tool_calling);
  EXPECT_FALSE(capabilities.positional_tool_results);
}

TEST(ModelCapabilitiesTest, QualifiedQwenReasoningProbeEnablesHistoryPreservation) {
  const auto capabilities = fl::model_capabilities_internal::ResolveRenderedProbes(
      "qwen3_5_text", kQwenToolCallProjection, kQwenToolResultProjection,
      "<tool_call>", "</tool_call>", kQwenReasoningProjection,
      kQwenNoPreserveReasoningProjection, "<think>", "</think>");

  EXPECT_TRUE(capabilities.supports_tool_calling);
  EXPECT_TRUE(capabilities.supports_reasoning_history);
  EXPECT_TRUE(capabilities.supports_preserve_thinking);
  EXPECT_TRUE(capabilities.supports_reasoning_controls);
}

TEST(ModelCapabilitiesTest, ReasoningPreservationProbeFailsClosed) {
  const auto missing_default_reasoning = fl::model_capabilities_internal::ResolveRenderedProbes(
      "qwen3_5_text", kQwenToolCallProjection, kQwenToolResultProjection,
      "<tool_call>", "</tool_call>", kQwenNoPreserveReasoningProjection,
      kQwenNoPreserveReasoningProjection, "<think>", "</think>");
  const auto ignores_opt_out = fl::model_capabilities_internal::ResolveRenderedProbes(
      "qwen3_5_text", kQwenToolCallProjection, kQwenToolResultProjection,
      "<tool_call>", "</tool_call>", kQwenReasoningProjection,
      kQwenReasoningProjection, "<think>", "</think>");
  const auto non_qwen_reasoning_template = fl::model_capabilities_internal::ResolveRenderedProbes(
      "qwen3", kQwenToolCallProjection, kQwenToolResultProjection,
      "<tool_call>", "</tool_call>", kQwenReasoningProjection,
      kQwenNoPreserveReasoningProjection, "<think>", "</think>");

  EXPECT_FALSE(missing_default_reasoning.supports_reasoning_history);
  EXPECT_FALSE(missing_default_reasoning.supports_preserve_thinking);
  EXPECT_TRUE(missing_default_reasoning.supports_reasoning_controls);
  EXPECT_TRUE(ignores_opt_out.supports_reasoning_history);
  EXPECT_FALSE(ignores_opt_out.supports_preserve_thinking);
  EXPECT_TRUE(ignores_opt_out.supports_reasoning_controls);
  EXPECT_TRUE(non_qwen_reasoning_template.supports_reasoning_history);
  EXPECT_TRUE(non_qwen_reasoning_template.supports_preserve_thinking);
  EXPECT_TRUE(non_qwen_reasoning_template.supports_tool_calling);
  EXPECT_FALSE(non_qwen_reasoning_template.native_qwen_xml_tool_calls);
  EXPECT_TRUE(non_qwen_reasoning_template.positional_tool_results);
}

TEST(ModelCapabilitiesTest, QualifiedQwenPackageResolvesBothProductionCapabilities) {
  constexpr const char* kQwenModelAlias = "qwen3.5-0.8b-generic-cpu-2";
  constexpr const char* kQualifiedModelPath = "FOUNDRY_QUALIFIED_QWEN_MODEL_PATH";
  constexpr const char* kRequireQualifiedModel = "FOUNDRY_REQUIRE_QUALIFIED_QWEN_MODEL";
  const auto explicit_path = fl::test::SafeGetEnv(kQualifiedModelPath);
  if (explicit_path.empty()) {
    const auto required = fl::test::SafeGetEnv(kRequireQualifiedModel);
    ASSERT_TRUE(required != "1" && required != "true" && required != "True")
        << kRequireQualifiedModel << " requires " << kQualifiedModelPath
        << " to name an installed Qwen qualification package";
    GTEST_SKIP() << "Set " << kQualifiedModelPath
                 << " to a qualified Qwen package root or model directory";
  }

  const auto model_path = FindModelConfigDirectory(explicit_path);
  ASSERT_FALSE(model_path.empty())
      << kQualifiedModelPath << " must name a package root or model directory containing genai_config.json";

  fl::StderrLogger logger;
  CpuCudaEpDetector cpu_cuda_detector;
  fl::ModelLoadManager load_manager(cpu_cuda_detector, logger);
  const auto result = load_manager.LoadModel(model_path.string(), kQwenModelAlias);

  ASSERT_EQ(result.status, fl::ModelLoadManager::LoadStatus::kSuccess);
  ASSERT_NE(result.model, nullptr);
  EXPECT_EQ(result.model->ModelType(), "qwen3_5_text");
  EXPECT_TRUE(result.model->SupportsToolCalling());
  EXPECT_TRUE(result.model->HasNativeQwenXmlToolCalls());
  EXPECT_TRUE(result.model->HasPositionalToolResults());
  EXPECT_TRUE(result.model->SupportsReasoningHistory());
  EXPECT_TRUE(result.model->SupportsPreserveThinking());
  EXPECT_TRUE(result.model->SupportsReasoningControls());
  EXPECT_TRUE(load_manager.UnloadModel(kQwenModelAlias));
}

}  // namespace
