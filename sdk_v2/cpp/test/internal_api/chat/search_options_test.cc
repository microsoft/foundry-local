// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Unit tests for SearchOptions and ApplySearchOptions.
// Uses a real OgaModel to create OgaGeneratorParams (the OGA API requires a model
// to construct params). Validates parameter mapping, token budget validation, and defaults.

#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/chat/stop_strings.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "exception.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/model_load_manager.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"

#include <ort_genai.h>
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace fl;

namespace {

SearchOptions ResolveTextOutputLimit(SearchOptions options = {}) {
  options.max_output_tokens = ResolveOutputLimit(options, false);
  return options;
}

}  // namespace

TEST(SearchOptionsParsingTest, TemperatureOutsideSupportedRangeThrows) {
  for (const char* temperature : {"-1", "2.1", "nan"}) {
    KeyValuePairs params;
    params.Add(FOUNDRY_LOCAL_PARAM_TEMPERATURE, temperature);

    EXPECT_THROW(SearchOptions::FromParameters(params), fl::Exception);
  }
}

TEST(OutputLimitResolutionTest, AbsentCurrentSchemaGenerationDefaultUsesTextAndMediaFallbacks) {
  const SearchOptions options;

  const auto text = ResolveOutputLimit(options, false);
  EXPECT_EQ(text, 2048);

  const auto media = ResolveOutputLimit(options, true);
  EXPECT_EQ(media, 3072);
}

TEST(OutputLimitResolutionTest, ExplicitPositiveRequestLimitWinsForTextAndMedia) {
  SearchOptions options;
  options.max_output_tokens = 64;

  for (bool has_media : {false, true}) {
    EXPECT_EQ(ResolveOutputLimit(options, has_media), 64);
  }
}

TEST(SearchOptionsParsingTest, RetainedGenerationSettingsAreBackendAware) {
  SearchOptions first;
  first.temperature = 0.5f;
  first.seed = 1;
  first.max_output_tokens = 16;
  first.tool_choice = FOUNDRY_LOCAL_TOOL_CHOICE_AUTO;

  SearchOptions second = first;
  second.max_output_tokens = 64;
  second.tool_choice = FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED;
  EXPECT_TRUE(first.HasSameRetainedGenerationSettings(second, ChatBackendKind::kGenerator));

  second.temperature = 1.0f;
  second.seed = 2;
  EXPECT_FALSE(first.HasSameRetainedGenerationSettings(second, ChatBackendKind::kGenerator));
  EXPECT_TRUE(first.HasSameRetainedGenerationSettings(second, ChatBackendKind::kEngine));

  second = first;
  second.frequency_penalty = 0.0f;
  second.presence_penalty = 0.0f;
  second.early_stopping = false;
  EXPECT_TRUE(first.HasSameRetainedGenerationSettings(second, ChatBackendKind::kGenerator));
}

TEST(SearchOptionsParsingTest, StopStringsRoundTripWithoutReplacingEarlyStopping) {
  KeyValuePairs params;
  StoreStopStringsOption({"END", "STOP"}, params);
  params.Add(FOUNDRY_LOCAL_PARAM_EARLY_STOPPING, "true");

  const SearchOptions options = SearchOptions::FromParameters(params);
  ASSERT_TRUE(options.early_stopping.has_value());
  EXPECT_TRUE(*options.early_stopping);
  EXPECT_EQ(options.stop_sequences, (std::vector<std::string>{"END", "STOP"}));
}

TEST(SamplingPlanTest, UnsetOptionsLeaveEveryKnobToModelPolicy) {
  const auto plan = ResolveSamplingPlan(SearchOptions{});
  EXPECT_FALSE(plan.greedy);
  EXPECT_FALSE(plan.do_sample.has_value());
  EXPECT_FALSE(plan.temperature.has_value());
  EXPECT_FALSE(plan.top_p.has_value());
  EXPECT_FALSE(plan.top_k.has_value());
}

TEST(SamplingPlanTest, ZeroTemperatureDropsIrrelevantTopPAndTopK) {
  SearchOptions options;
  options.temperature = 0.0f;
  options.top_p = 0.9f;
  options.top_k = 40;

  const auto plan = ResolveSamplingPlan(options);
  EXPECT_TRUE(plan.greedy);
  ASSERT_TRUE(plan.temperature.has_value());
  EXPECT_FLOAT_EQ(*plan.temperature, 0.0f);
  EXPECT_FALSE(plan.top_p.has_value());
  EXPECT_FALSE(plan.top_k.has_value());
  ASSERT_TRUE(plan.do_sample.has_value());
  EXPECT_FALSE(*plan.do_sample);
}

TEST(SamplingPlanTest, GreedyKeepsNeutralScalarsUpstreamAccepts) {
  SearchOptions options;
  options.do_sample = false;
  options.temperature = 1.0f;
  options.top_p = 1.0f;
  options.top_k = 0;

  const auto plan = ResolveSamplingPlan(options);
  EXPECT_TRUE(plan.greedy);
  ASSERT_TRUE(plan.do_sample.has_value());
  EXPECT_FALSE(*plan.do_sample);
  ASSERT_TRUE(plan.temperature.has_value());
  EXPECT_FLOAT_EQ(*plan.temperature, 1.0f);
  ASSERT_TRUE(plan.top_p.has_value());
  EXPECT_FLOAT_EQ(*plan.top_p, 1.0f);
  ASSERT_TRUE(plan.top_k.has_value());
  EXPECT_EQ(*plan.top_k, 0);
}

TEST(SamplingPlanTest, DoSampleFalseDropsContradictoryDistributionScalars) {
  SearchOptions options;
  options.do_sample = false;
  options.temperature = 0.7f;
  options.top_p = 0.9f;
  options.top_k = 32;

  const auto plan = ResolveSamplingPlan(options);
  EXPECT_TRUE(plan.greedy);
  ASSERT_TRUE(plan.do_sample.has_value());
  EXPECT_FALSE(*plan.do_sample);
  EXPECT_FALSE(plan.temperature.has_value());
  EXPECT_FALSE(plan.top_p.has_value());
  EXPECT_FALSE(plan.top_k.has_value());
}

TEST(SamplingPlanTest, TopKOneIsItsOwnGreedyCauseAndSurvives) {
  SearchOptions options;
  options.top_k = 1;
  options.top_p = 0.5f;
  options.temperature = 0.7f;

  const auto plan = ResolveSamplingPlan(options);
  EXPECT_TRUE(plan.greedy);
  ASSERT_TRUE(plan.top_k.has_value());
  EXPECT_EQ(*plan.top_k, 1);
  EXPECT_FALSE(plan.top_p.has_value());
  EXPECT_FALSE(plan.temperature.has_value());
}

TEST(SamplingPlanTest, ExplicitDoSampleAndZeroTemperatureBothSurvive) {
  SearchOptions options;
  options.do_sample = true;
  options.temperature = 0.0f;

  // Upstream accepts do_sample=true when the caller spelled greedy out itself (temperature 0 here).
  const auto plan = ResolveSamplingPlan(options);
  EXPECT_TRUE(plan.greedy);
  ASSERT_TRUE(plan.do_sample.has_value());
  EXPECT_TRUE(*plan.do_sample);
  ASSERT_TRUE(plan.temperature.has_value());
  EXPECT_FLOAT_EQ(*plan.temperature, 0.0f);
}

TEST(SamplingPlanTest, SampledTurnForwardsEveryExplicitScalar) {
  SearchOptions options;
  options.do_sample = true;
  options.temperature = 0.7f;
  options.top_p = 0.9f;
  options.top_k = 40;

  const auto plan = ResolveSamplingPlan(options);
  EXPECT_FALSE(plan.greedy);
  ASSERT_TRUE(plan.temperature.has_value());
  EXPECT_FLOAT_EQ(*plan.temperature, 0.7f);
  ASSERT_TRUE(plan.top_p.has_value());
  EXPECT_FLOAT_EQ(*plan.top_p, 0.9f);
  ASSERT_TRUE(plan.top_k.has_value());
  EXPECT_EQ(*plan.top_k, 40);
}

TEST(SamplingPlanTest, PositiveTemperatureEnablesSampling) {
  SearchOptions options;
  options.temperature = 0.7f;

  const auto plan = ResolveSamplingPlan(options);
  EXPECT_FALSE(plan.greedy);
  ASSERT_TRUE(plan.do_sample.has_value());
  EXPECT_TRUE(*plan.do_sample);
}

TEST(SamplingPlanTest, OutOfRangeScalarsAreRejected) {
  for (float top_p : {-0.1f, 1.1f, std::numeric_limits<float>::infinity()}) {
    SearchOptions options;
    options.top_p = top_p;
    EXPECT_THROW(ResolveSamplingPlan(options), fl::Exception) << "top_p=" << top_p;
  }

  SearchOptions negative_top_k;
  negative_top_k.top_k = -1;
  EXPECT_THROW(ResolveSamplingPlan(negative_top_k), fl::Exception);
}

TEST(EngineTurnOptionsPlanTest, RejectsAnUnresolvedOutputLimitBeforeBackendSubmission) {
  EXPECT_THROW(
      BuildEngineTurnOptionsPlan(SearchOptions{}, ToolCallContext{}, ChatBackendKind::kEngine, false),
      fl::Exception);
}

TEST(EngineTurnOptionsPlanTest, ProductionFacingPlanCarriesCanonicalResolvedFallback) {
  const auto options = ResolveTextOutputLimit();
  const auto plan =
      BuildEngineTurnOptionsPlan(options, ToolCallContext{}, ChatBackendKind::kEngine, false);

  EXPECT_EQ(plan.max_generated_tokens, 2048);
  EXPECT_FALSE(plan.sampling.do_sample.has_value());
  EXPECT_FALSE(plan.sampling.temperature.has_value());
  EXPECT_FALSE(plan.seed.has_value());
  EXPECT_TRUE(plan.stop_sequences.empty());
  EXPECT_FALSE(plan.guidance.has_value());
}

TEST(EngineTurnOptionsPlanTest, RejectsNonpositiveExplicitMaxOutputTokens) {
  for (int max_output_tokens : {0, -1}) {
    SearchOptions options;
    options.max_output_tokens = max_output_tokens;
    EXPECT_THROW(BuildEngineTurnOptionsPlan(options, ToolCallContext{}, ChatBackendKind::kEngine, false),
                 fl::Exception);
  }
}

TEST(EngineTurnOptionsPlanTest, CarriesStopStringsSeedAndGuidanceOnDynamicBackend) {
  SearchOptions options;
  options.max_output_tokens = 64;
  options.seed = 42;
  options.stop_sequences = {"END", "STOP"};

  ToolCallContext tool_ctx;
  tool_ctx.text_output = false;
  tool_ctx.tool_output = true;
  tool_ctx.guidance_type = "json_schema";
  tool_ctx.guidance_data = R"({"type":"object"})";

  const auto plan = BuildEngineTurnOptionsPlan(options, tool_ctx, ChatBackendKind::kEngine, false);
  EXPECT_EQ(plan.max_generated_tokens, 64);
  ASSERT_TRUE(plan.seed.has_value());
  EXPECT_EQ(*plan.seed, 42);
  EXPECT_EQ(plan.stop_sequences, (std::vector<std::string>{"END", "STOP"}));
  ASSERT_TRUE(plan.guidance.has_value());
  EXPECT_EQ(plan.guidance->type, "json_schema");
  EXPECT_EQ(plan.guidance->data, R"({"type":"object"})");
  EXPECT_TRUE(plan.guidance->user_specified);
}

TEST(EngineTurnOptionsPlanTest, NegativeSeedIsOmitted) {
  for (int seed : {-1, -2, std::numeric_limits<int>::min()}) {
    SearchOptions options;
    options.seed = seed;
    options = ResolveTextOutputLimit(std::move(options));

    const auto plan =
        BuildEngineTurnOptionsPlan(options, ToolCallContext{}, ChatBackendKind::kEngine, false);
    EXPECT_FALSE(plan.seed.has_value());
  }
}

TEST(EngineTurnOptionsPlanTest, ZeroSeedIsForwarded) {
  SearchOptions options;
  options.seed = 0;
  options = ResolveTextOutputLimit(std::move(options));

  const auto plan =
      BuildEngineTurnOptionsPlan(options, ToolCallContext{}, ChatBackendKind::kEngine, false);
  ASSERT_TRUE(plan.seed.has_value());
  EXPECT_EQ(*plan.seed, 0);
}

TEST(EngineTurnOptionsPlanTest, UserGuidanceAppliesWithoutToolOnlyMode) {
  ToolCallContext tool_ctx;
  tool_ctx.guidance_type = "json_schema";
  tool_ctx.guidance_data = R"({"type":"object","required":["answer"]})";

  const auto plan =
      BuildEngineTurnOptionsPlan(ResolveTextOutputLimit(), tool_ctx, ChatBackendKind::kEngine, false);

  ASSERT_TRUE(plan.guidance.has_value());
  EXPECT_EQ(plan.guidance->type, "json_schema");
  EXPECT_EQ(plan.guidance->data, R"({"type":"object","required":["answer"]})");
  EXPECT_TRUE(plan.guidance->user_specified);
}

TEST(EngineTurnOptionsPlanTest, PromptOpenedReasoningOmitsTheGrammarOpener) {
  ToolCallContext tool_ctx;
  tool_ctx.text_output = false;
  tool_ctx.tool_output = true;
  tool_ctx.supports_reasoning = true;
  tool_ctx.reasoning_start = "<think>";
  tool_ctx.reasoning_end = "</think>";
  tool_ctx.reasoning_start_token_id = 248058;
  tool_ctx.reasoning_end_token_id = 248059;

  const auto options = ResolveTextOutputLimit();
  const auto closed_plan =
      BuildEngineTurnOptionsPlan(options, tool_ctx, ChatBackendKind::kEngine, false);
  const auto open_plan =
      BuildEngineTurnOptionsPlan(options, tool_ctx, ChatBackendKind::kEngine, true);

  ASSERT_TRUE(closed_plan.guidance.has_value());
  ASSERT_TRUE(open_plan.guidance.has_value());
  EXPECT_FALSE(closed_plan.guidance->user_specified);
  EXPECT_FALSE(open_plan.guidance->user_specified);
  EXPECT_NE(closed_plan.guidance->data.find("cot: <[248058]> THINK_TEXT"), std::string::npos);
  EXPECT_NE(open_plan.guidance->data.find("cot: THINK_TEXT <[248059]>"), std::string::npos);
  EXPECT_EQ(open_plan.guidance->data.find("cot: <[248058]>"), std::string::npos);
}

TEST(EngineTurnOptionsPlanTest, NeutralPenaltiesDoNotOverrideModelDefaults) {
  SearchOptions options;
  options.frequency_penalty = 0.0f;
  options.presence_penalty = 0.0f;
  options = ResolveTextOutputLimit(std::move(options));

  EXPECT_NO_THROW(
      BuildEngineTurnOptionsPlan(options, ToolCallContext{}, ChatBackendKind::kEngine, false));
}

TEST(EngineTurnOptionsPlanTest, RejectsNonzeroPenalties) {
  for (const auto& [frequency, presence] :
       {std::pair{0.5f, 0.0f}, std::pair{0.0f, 0.3f}, std::pair{-0.5f, 0.0f},
        std::pair{0.0f, -0.3f}}) {
    SearchOptions options;
    options.frequency_penalty = frequency;
    options.presence_penalty = presence;
    options = ResolveTextOutputLimit(std::move(options));

    EXPECT_THROW(BuildEngineTurnOptionsPlan(options, ToolCallContext{}, ChatBackendKind::kEngine, false),
                 fl::Exception);
  }
}

TEST(EngineTurnOptionsPlanTest, RejectsTrueEarlyStoppingAndAcceptsNeutralFalse) {
  SearchOptions enabled;
  enabled.early_stopping = true;
  enabled = ResolveTextOutputLimit(std::move(enabled));
  EXPECT_THROW(BuildEngineTurnOptionsPlan(enabled, ToolCallContext{}, ChatBackendKind::kEngine, false),
               fl::Exception);

  SearchOptions disabled;
  disabled.early_stopping = false;
  disabled = ResolveTextOutputLimit(std::move(disabled));
  EXPECT_NO_THROW(
      BuildEngineTurnOptionsPlan(disabled, ToolCallContext{}, ChatBackendKind::kEngine, false));
}

TEST(SearchOptionsParsingTest, EngineSupportsPerTurnStopStringsAndSeed) {
  EXPECT_TRUE(ShouldForwardStopSequencesToEngine(ChatBackendKind::kEngine));
  EXPECT_FALSE(ShouldForwardStopSequencesToEngine(ChatBackendKind::kGenerator));

  EXPECT_TRUE(SupportsPerTurnSeed(ChatBackendKind::kEngine));
  EXPECT_FALSE(SupportsPerTurnSeed(ChatBackendKind::kGenerator));
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

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);

  EXPECT_EQ(params->GetSearchNumber("temperature"), 0.0);
  EXPECT_FALSE(params->GetSearchBool("do_sample"));
}

TEST_F(SearchOptionsTest, GreedyTurnDoesNotForwardIrrelevantTopP) {
  SearchOptions opts;
  opts.temperature = 0.0f;
  opts.top_p = 0.9f;
  auto params = MakeParams();
  const double model_top_p = params->GetSearchNumber("top_p");
  ASSERT_NE(model_top_p, 0.9);

  // Upstream rejects an explicitly set top_p strictly inside (0, 1) on a turn that selects the top logit, so a
  // request that only asked for temperature 0 must not be turned into a rejected combination.
  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);

  EXPECT_EQ(params->GetSearchNumber("top_p"), model_top_p);
}

TEST_F(SearchOptionsTest, SampledTurnForwardsTopPAndTopK) {
  SearchOptions opts;
  opts.temperature = 0.7f;
  opts.top_p = 0.9f;
  opts.top_k = 25;
  auto params = MakeParams();

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);

  EXPECT_FLOAT_EQ(static_cast<float>(params->GetSearchNumber("top_p")), 0.9f);
  EXPECT_EQ(params->GetSearchNumber("top_k"), 25);
}

TEST_F(SearchOptionsTest, AbsentDoSamplePreservesGeneratorSamplingDefault) {
  SearchOptions opts;
  auto params = MakeParams();

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);

  EXPECT_TRUE(params->GetSearchBool("do_sample"));
}

TEST_F(SearchOptionsTest, ExplicitDoSampleFalseIsForwarded) {
  SearchOptions opts;
  opts.do_sample = false;
  auto params = MakeParams();

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);

  EXPECT_FALSE(params->GetSearchBool("do_sample"));
}

TEST_F(SearchOptionsTest, OutOfRangeTopPOrNegativeTopKThrows) {
  SearchOptions bad_top_p;
  bad_top_p.top_p = 1.5f;
  auto params = MakeParams();
  EXPECT_THROW(ApplySearchOptions(bad_top_p, 10, GetConfig(), *params, ExecutionProvider::kDefault), fl::Exception);

  SearchOptions bad_top_k;
  bad_top_k.top_k = -1;
  auto other_params = MakeParams();
  EXPECT_THROW(ApplySearchOptions(bad_top_k, 10, GetConfig(), *other_params, ExecutionProvider::kDefault),
               fl::Exception);
}

TEST_F(SearchOptionsTest, TemperaturePositiveEnablesSampling) {
  SearchOptions opts;
  opts.temperature = 0.7f;
  auto params = MakeParams();

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);

  EXPECT_TRUE(params->GetSearchBool("do_sample"));
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
  opts.seed = 42;
  opts.do_sample = true;
  auto params = MakeParams();

  int max_length = ApplySearchOptions(opts, 20, GetConfig(), *params, ExecutionProvider::kDefault);
  EXPECT_EQ(max_length, 276);  // 20 + 256
}

TEST_F(SearchOptionsTest, EarlyStoppingRemainsSupportedByClassicGenerator) {
  SearchOptions opts;
  opts.early_stopping = true;
  auto params = MakeParams();

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);

  EXPECT_TRUE(params->GetSearchBool("early_stopping"));
}

TEST_F(SearchOptionsTest, ZeroPenaltiesDoNotOverrideModelDefaults) {
  SearchOptions opts;
  opts.frequency_penalty = 0.0f;
  opts.presence_penalty = 0.0f;
  auto params = MakeParams();
  const auto repetition_penalty = params->GetSearchNumber("repetition_penalty");
  const auto diversity_penalty = params->GetSearchNumber("diversity_penalty");

  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);

  EXPECT_EQ(params->GetSearchNumber("repetition_penalty"), repetition_penalty);
  EXPECT_EQ(params->GetSearchNumber("diversity_penalty"), diversity_penalty);
}

TEST_F(SearchOptionsTest, NonzeroPenaltiesAreRejected) {
  for (const auto& [frequency, presence] :
       {std::pair{0.5f, 0.0f}, std::pair{0.0f, 0.3f}, std::pair{-0.5f, 0.0f}, std::pair{0.0f, -0.3f}}) {
    SearchOptions opts;
    opts.frequency_penalty = frequency;
    opts.presence_penalty = presence;
    auto params = MakeParams();

    EXPECT_THROW(ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault), fl::Exception);
  }
}

TEST_F(SearchOptionsTest, NonpositiveMaxOutputTokensThrow) {
  for (int max_output_tokens : {0, -5}) {
    SearchOptions opts;
    opts.max_output_tokens = max_output_tokens;
    auto params = MakeParams();

    EXPECT_THROW(ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault),
                 fl::Exception)
        << "max_output_tokens=" << max_output_tokens;
  }
}

TEST_F(SearchOptionsTest, ExplicitDoSampleAndZeroTemperatureAreBothForwarded) {
  SearchOptions opts;
  opts.temperature = 0.0f;
  opts.do_sample = true;
  auto params = MakeParams();

  // The caller spelled greedy out itself with temperature 0, which upstream accepts alongside do_sample=true.
  ApplySearchOptions(opts, 10, GetConfig(), *params, ExecutionProvider::kDefault);

  EXPECT_TRUE(params->GetSearchBool("do_sample"));
  EXPECT_EQ(params->GetSearchNumber("temperature"), 0.0);
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
