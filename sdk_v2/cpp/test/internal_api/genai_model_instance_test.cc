// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/genai_model_instance.h"
#include "ep_detection/ep_detector.h"
#include "inferencing/model_load_manager.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
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
      "qwen3_5_text", kQwenToolCallProjection, kQwenToolResultProjection);

  EXPECT_TRUE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_TRUE(capabilities.positional_tool_results);
}

TEST(ModelCapabilitiesTest, ExactQwen35TextTypeWithNonmatchingTemplateDisablesBothCapabilities) {
  const auto capabilities =
      fl::model_capabilities_internal::ResolveRenderedProbes("qwen3_5_text", "not-qwen", "not-qwen");

  EXPECT_FALSE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_FALSE(capabilities.positional_tool_results);
}

class UnsupportedModelCapabilitiesTest : public ::testing::TestWithParam<std::string_view> {};

TEST_P(UnsupportedModelCapabilitiesTest, ExactQwenTemplateDoesNotEnableCapabilities) {
  const auto capabilities = fl::model_capabilities_internal::ResolveRenderedProbes(
      GetParam(), kQwenToolCallProjection, kQwenToolResultProjection);

  EXPECT_FALSE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_FALSE(capabilities.positional_tool_results);
}

INSTANTIATE_TEST_SUITE_P(
    UnsupportedModelTypes,
    UnsupportedModelCapabilitiesTest,
    ::testing::Values("qwen3_5", "qwen3", "qwen3_moe", "qwen3_5_moe", "qwen3_vl", "qwen3_5_vl", "unknown", ""));

TEST(ModelCapabilitiesTest, ResultProjectionMismatchDisablesOnlyPositionalToolResults) {
  const auto capabilities = fl::model_capabilities_internal::ResolveRenderedProbes(
      "qwen3_5_text", kQwenToolCallProjection, "not-qwen");

  EXPECT_TRUE(capabilities.native_qwen_xml_tool_calls);
  EXPECT_FALSE(capabilities.positional_tool_results);
}

TEST(ModelCapabilitiesTest, QualifiedQwenPackageResolvesBothProductionCapabilities) {
  constexpr const char* kQwenModelAlias = "qwen3.5-0.8b-generic-cpu-2";
  constexpr const char* kQualifiedModelPath =
      "FOUNDRY_QUALIFIED_QWEN_MODEL_PATH";
  const auto explicit_path = fl::test::SafeGetEnv(kQualifiedModelPath);
  std::filesystem::path model_path;
  if (!explicit_path.empty()) {
    model_path = FindModelConfigDirectory(explicit_path);
    ASSERT_FALSE(model_path.empty())
        << kQualifiedModelPath
        << " must name a package root or model directory containing "
           "genai_config.json";
  } else {
    const auto cache = fl::test::SafeGetEnv("FOUNDRY_TEST_DATA_DIR");
    if (cache.empty() ||
        !std::filesystem::exists(std::filesystem::path(cache) / "Microsoft" /
                                 kQwenModelAlias)) {
      GTEST_SKIP() << "Qualified Qwen package is absent; set "
                   << kQualifiedModelPath
                   << " or install the fallback package in FOUNDRY_TEST_DATA_DIR";
    }

    model_path = fl::test::GetTestModelPath(kQwenModelAlias);
  }

  fl::StderrLogger logger;
  fl::test::CpuOnlyEpDetector cpu_detector;
  CpuCudaEpDetector cpu_cuda_detector;
  fl::IEpDetector& ep_detector = explicit_path.empty()
                                     ? static_cast<fl::IEpDetector&>(cpu_detector)
                                     : static_cast<fl::IEpDetector&>(cpu_cuda_detector);
  fl::ModelLoadManager load_manager(ep_detector, logger);
  const auto result = load_manager.LoadModel(model_path.string(), kQwenModelAlias);

  ASSERT_EQ(result.status, fl::ModelLoadManager::LoadStatus::kSuccess);
  ASSERT_NE(result.model, nullptr);
  EXPECT_EQ(result.model->ModelType(), "qwen3_5_text");
  EXPECT_TRUE(result.model->HasNativeQwenXmlToolCalls());
  EXPECT_TRUE(result.model->HasPositionalToolResults());
  EXPECT_TRUE(load_manager.UnloadModel(kQwenModelAlias));
}

}  // namespace
