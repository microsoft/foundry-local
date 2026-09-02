// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Unit tests for SearchOptions and ApplySearchOptions.
// Uses a real OgaModel to create OgaGeneratorParams (the OGA API requires a model
// to construct params). Validates parameter mapping, token budget validation, and defaults.

#include "inferencing/generative/chat/search_options.h"
#include "exception.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/model_load_manager.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"

#include <ort_genai.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace fl;

TEST(SearchOptionsParsingTest, TemperatureOutsideSupportedRangeThrows) {
  for (const char* temperature : {"-1", "2.1", "nan"}) {
    KeyValuePairs params;
    params.Add(FOUNDRY_LOCAL_PARAM_TEMPERATURE, temperature);

    EXPECT_THROW(SearchOptions::FromParameters(params), fl::Exception);
  }
}

// ---------------------------------------------------------------------------
// Test fixture: loads the shared test model once per suite
// ---------------------------------------------------------------------------

class SearchOptionsTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    auto model_path = fl::test::GetTestModelPath(fl::test::kTestChatModelAlias);
    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);

    auto result = load_manager_->LoadModel(
        model_path.string(),
        fl::test::kTestChatModelAlias);

    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess)
        << "Failed to load test model from: " << model_path;

    model_ = result.model;
  }

  static void TearDownTestSuite() {
    if (load_manager_) {
      load_manager_->UnloadModel(fl::test::kTestChatModelAlias);
    }

    load_manager_.reset();
    ep_detector_.reset();
    model_ = nullptr;
  }

  /// Create a fresh OgaGeneratorParams for the loaded model.
  std::unique_ptr<OgaGeneratorParams> MakeParams() {
    return OgaGeneratorParams::Create(model_->GetOgaModel());
  }

  GenAIModelInstance& GetModel() { return *model_; }
  const GenAIConfig& GetConfig() { return model_->GetGenAIConfig(); }

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(SearchOptionsTest, DefaultOptionsApplySuccessfully) {
  SearchOptions opts;
  auto params = MakeParams();

  int max_length = ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);
  EXPECT_GT(max_length, 10);
  // Default output tokens = 2048, so max_length should be 10 + 2048 = 2058
  EXPECT_EQ(max_length, 2058);
}

TEST_F(SearchOptionsTest, MaxOutputTokensRespected) {
  SearchOptions opts;
  opts.max_output_tokens = 100;
  auto params = MakeParams();

  int max_length = ApplySearchOptions(opts, 50, GetConfig(), *params, ExecutionProvider::kDefault);
  EXPECT_EQ(max_length, 150);  // 50 input + 100 output
}

TEST_F(SearchOptionsTest, TokenBudgetExceededThrows) {
  SearchOptions opts;
  // Model max_length is 32768. Request more than that.
  opts.max_output_tokens = 32000;
  auto params = MakeParams();

  EXPECT_THROW(ApplySearchOptions(opts, 1000, GetConfig(), *params, ExecutionProvider::kDefault),
               fl::Exception);
}

TEST_F(SearchOptionsTest, TemperatureZeroDisablesSampling) {
  SearchOptions opts;
  opts.temperature = 0.0f;
  auto params = MakeParams();

  // Should not throw — temperature 0 → do_sample=false
  EXPECT_NO_THROW(ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault));
}

TEST_F(SearchOptionsTest, TemperaturePositiveEnablesSampling) {
  SearchOptions opts;
  opts.temperature = 0.7f;
  auto params = MakeParams();

  EXPECT_NO_THROW(ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault));
}

TEST_F(SearchOptionsTest, TemperatureOutsideSupportedRangeThrows) {
  for (float temperature : {-1.0f, 2.1f, std::stof("nan")}) {
    SearchOptions opts;
    opts.temperature = temperature;
    auto params = MakeParams();

    EXPECT_THROW(ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault),
                 fl::Exception);
  }
}

TEST_F(SearchOptionsTest, TemperatureRangeBoundariesApplySuccessfully) {
  for (float temperature : {0.0f, 2.0f}) {
    SearchOptions opts;
    opts.temperature = temperature;
    auto params = MakeParams();

    EXPECT_NO_THROW(ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault));
  }
}

TEST_F(SearchOptionsTest, AllOptionsSetSimultaneously) {
  SearchOptions opts;
  opts.temperature = 0.8f;
  opts.top_p = 0.9f;
  opts.top_k = 50;
  opts.max_output_tokens = 256;
  opts.frequency_penalty = 1.1f;
  opts.presence_penalty = 0.5f;
  opts.seed = 42;
  opts.do_sample = true;
  auto params = MakeParams();

  int max_length = ApplySearchOptions(opts, 20, GetConfig(), *params, ExecutionProvider::kDefault);
  EXPECT_EQ(max_length, 276);  // 20 + 256
}

TEST_F(SearchOptionsTest, ZeroMaxOutputTokensThrows) {
  SearchOptions opts;
  opts.max_output_tokens = 0;
  auto params = MakeParams();

  EXPECT_THROW(ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault),
               fl::Exception);
}

TEST_F(SearchOptionsTest, NegativeMaxOutputTokensThrows) {
  SearchOptions opts;
  opts.max_output_tokens = -5;
  auto params = MakeParams();

  EXPECT_THROW(ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault),
               fl::Exception);
}

TEST_F(SearchOptionsTest, ExplicitDoSampleOverridesTemperature) {
  SearchOptions opts;
  opts.temperature = 0.0f;  // Would normally disable sampling
  opts.do_sample = true;    // But explicit override takes priority
  auto params = MakeParams();

  EXPECT_NO_THROW(ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault));
}

TEST_F(SearchOptionsTest, LargeInputFitsExactly) {
  SearchOptions opts;
  // Model max_length is 32768. Set output to fit exactly.
  opts.max_output_tokens = 768;
  auto params = MakeParams();

  int max_length = ApplySearchOptions(opts, 32000, GetConfig(), *params, ExecutionProvider::kDefault);
  EXPECT_EQ(max_length, 32768);  // Exactly at limit
}

TEST_F(SearchOptionsTest, LargeInputExceedsByOneThrows) {
  SearchOptions opts;
  opts.max_output_tokens = 769;
  auto params = MakeParams();

  EXPECT_THROW(ApplySearchOptions(opts, 32000, GetConfig(), *params, ExecutionProvider::kDefault),
               fl::Exception);
}

TEST_F(SearchOptionsTest, ChunkedPrefillDefaultsTo2048ForSupportedExecutionProviders) {
  SearchOptions opts;

  for (ExecutionProvider ep : {ExecutionProvider::kCPU, ExecutionProvider::kCUDA,
                               ExecutionProvider::kTensorRT_RTX, ExecutionProvider::kWebGPU}) {
    auto params = MakeParams();
    ApplySearchOptions(opts, 10, GetConfig(), *params, ep);
    EXPECT_EQ(params->GetSearchNumber("chunk_size"), 2048) << "EP: " << static_cast<int>(ep);
  }
}

TEST_F(SearchOptionsTest, ChunkedPrefillSkippedForUnlistedEp) {
  SearchOptions opts;
  auto params = MakeParams();

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kOpenVINO);
  EXPECT_EQ(params->GetSearchNumber("chunk_size"), 0);
}

TEST_F(SearchOptionsTest, ChunkedPrefillResolvesSupportedProvidersFromConfig) {
  SearchOptions opts;
  GenAIConfig config;
  auto& model = config.model.emplace();
  auto& decoder = model.decoder.emplace();
  auto& session_options = decoder.session_options.emplace();
  config.search.emplace().max_length = 32768;

  for (const char* provider : {"cpu", "cuda", "NvTensorRtRtx", "WebGPU"}) {
    session_options.provider_options = {{{provider, "{}"}}};
    auto params = MakeParams();
    ApplySearchOptions(opts, 10, config, *params, ExecutionProvider::kDefault);
    EXPECT_EQ(params->GetSearchNumber("chunk_size"), 2048) << "Provider: " << provider;
  }
}

TEST_F(SearchOptionsTest, ChunkedPrefillPreservesModelSetting) {
  SearchOptions opts;
  auto params = MakeParams();
  params->SetSearchOption("chunk_size", 1024);

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kCPU);
  EXPECT_EQ(params->GetSearchNumber("chunk_size"), 1024);
}

TEST_F(SearchOptionsTest, ChunkedPrefillTreatsZeroAsUnset) {
  SearchOptions opts;
  auto params = MakeParams();
  params->SetSearchOption("chunk_size", 0);

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kCPU);
  EXPECT_EQ(params->GetSearchNumber("chunk_size"), 2048);
}
