// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "configuration.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/onnx_chat_engine.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/model_load_manager.h"
#include "internal_api/test_helpers.h"
#include "manager.h"
#include "utils/safe_getenv.h"

#include <gtest/gtest.h>
#include <ort_genai.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

constexpr const char* kDflash2ModelPathEnv = "FOUNDRY_LOCAL_TEST_DFLASH2_MODEL_PATH";
constexpr const char* kDflash2ModelIdEnv = "FOUNDRY_LOCAL_TEST_DFLASH2_MODEL_ID";
constexpr const char* kDefaultDflash2ModelId = "qualification/dflash2-cuda-gpu";

class OnnxChatEngineTestAccessor {
 public:
  static OnnxChatEngine::SpeculativeStatsSnapshot SnapshotSpeculativeStats(OnnxChatEngine& engine) {
    return engine.GetSpeculativeStatsSnapshotForTest();
  }
};

namespace {

std::filesystem::path ResolveConfiguredModelPath() {
  return std::filesystem::path(test::SafeGetEnv(kDflash2ModelPathEnv));
}

std::string ConfiguredModelId() {
  const auto configured = test::SafeGetEnv(kDflash2ModelIdEnv);
  return configured.empty() ? std::string(kDefaultDflash2ModelId) : configured;
}

std::vector<int32_t> EncodeUserPrompt(std::string prompt, GenAIModelInstance& model) {
  std::vector<TranscriptMessage> messages;
  messages.emplace_back(FOUNDRY_LOCAL_ROLE_USER, std::move(prompt));
  auto sequences = EncodePrompt(BuildChatPrompt(messages, model), model);
  const auto count = sequences->SequenceCount(0);
  const auto* data = sequences->SequenceData(0);
  return {data, data + count};
}

struct TurnRunResult {
  bool succeeded = false;
  std::string error;
  OnnxChatEngine::TurnResult turn_result;
  OnnxChatEngine::SpeculativeStatsSnapshot before;
  OnnxChatEngine::SpeculativeStatsSnapshot after;
};

OnnxChatEngine::SpeculativeStatsSnapshot Delta(const OnnxChatEngine::SpeculativeStatsSnapshot& before,
                                               const OnnxChatEngine::SpeculativeStatsSnapshot& after) {
  return {
      after.draft_tokens_proposed - before.draft_tokens_proposed,
      after.draft_tokens_evaluated - before.draft_tokens_evaluated,
      after.draft_tokens_accepted - before.draft_tokens_accepted,
      after.rounds - before.rounds,
      after.dflash2_failures - before.dflash2_failures,
      after.dflash2_disables - before.dflash2_disables,
      after.dflash2_admission_misses - before.dflash2_admission_misses,
  };
}

bool HasDraftActivity(const OnnxChatEngine::SpeculativeStatsSnapshot& delta) {
  return delta.draft_tokens_proposed > 0 || delta.draft_tokens_evaluated > 0 || delta.draft_tokens_accepted > 0;
}

void ExpectHealthyDraftActivity(const OnnxChatEngine::SpeculativeStatsSnapshot& delta) {
  EXPECT_GT(delta.draft_tokens_proposed, 0u);
  EXPECT_GT(delta.draft_tokens_evaluated, 0u);
  EXPECT_EQ(delta.dflash2_failures, 0u);
  EXPECT_EQ(delta.dflash2_disables, 0u);
}

void ExpectNoDraftActivity(const OnnxChatEngine::SpeculativeStatsSnapshot& delta) {
  EXPECT_EQ(delta.draft_tokens_proposed, 0u);
  EXPECT_EQ(delta.draft_tokens_evaluated, 0u);
  EXPECT_EQ(delta.draft_tokens_accepted, 0u);
  EXPECT_EQ(delta.dflash2_failures, 0u);
  EXPECT_EQ(delta.dflash2_disables, 0u);
  EXPECT_EQ(delta.dflash2_admission_misses, 0u);
}

TurnRunResult RunTurn(OnnxChatEngine& engine,
                      GenAIModelInstance& model,
                      const SearchOptions& options,
                      std::string prompt) {
  ToolCallContext tool_context;
  const auto input_ids = EncodeUserPrompt(std::move(prompt), model);
  auto conversation = engine.CreateConversation(options, tool_context, static_cast<int>(input_ids.size()));

  TurnRunResult result;
  result.before = OnnxChatEngineTestAccessor::SnapshotSpeculativeStats(engine);
  try {
    engine.BeginTurn(conversation, input_ids, options, tool_context, false);
    while (engine.WaitForToken(conversation).has_value()) {
    }
    result.turn_result = engine.GetTurnResult(conversation);
    result.succeeded = true;
  } catch (const std::exception& error) {
    result.error = error.what();
  }
  result.after = OnnxChatEngineTestAccessor::SnapshotSpeculativeStats(engine);
  engine.Close(conversation);
  return result;
}

class Dflash2EngineQualificationTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    model_path_ = ResolveConfiguredModelPath();
    if (model_path_.empty()) {
      return;
    }

    if (!std::filesystem::is_directory(model_path_)) {
      setup_error_ = "Configured dflash2 model path is not a directory: " + model_path_.string();
      return;
    }

    if (!std::filesystem::is_regular_file(model_path_ / "genai_config.json")) {
      setup_error_ = "Configured dflash2 model path does not contain genai_config.json: " + model_path_.string();
      return;
    }

    try {
      const auto test_root = std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR).parent_path() / "dflash2-qualification";
      std::filesystem::create_directories(test_root);

      Configuration config;
      config.app_name = "dflash2-qualification";
      config.app_data_dir = (test_root / "app-data").string();
      config.model_cache_dir = (test_root / "model-cache").string();
      config.logs_dir = (test_root / "logs").string();
      config.disable_nonessential_telemetry = true;

      manager_ = &Manager::Create(config);

      std::vector<std::string> ep_names = {"CUDAExecutionProvider"};
      const auto ep_result = manager_->DownloadAndRegisterEps(&ep_names, nullptr);
      if (!ep_result.success) {
        setup_error_ = "Failed to download and register CUDAExecutionProvider: " + ep_result.status;
        return;
      }

      load_manager_ = &manager_->GetModelLoadManager();
      const auto result = load_manager_->LoadModel(model_path_.string(), ConfiguredModelId());
      if (result.status != ModelLoadManager::LoadStatus::kSuccess) {
        setup_error_ = "Failed to load dflash2 model from " + model_path_.string();
        return;
      }

      model_ = result.model;
    } catch (const std::exception& error) {
      setup_error_ = error.what();
    }
  }

  static void TearDownTestSuite() {
    if (load_manager_ && model_) {
      load_manager_->UnloadModel(ConfiguredModelId());
    }

    model_ = nullptr;
    load_manager_ = nullptr;
    if (manager_ != nullptr) {
      Manager::Destroy();
    }
    manager_ = nullptr;
    setup_error_.clear();
    model_path_.clear();
  }

  void SetUp() override {
    if (model_path_.empty()) {
      GTEST_SKIP() << kDflash2ModelPathEnv << " is not set";
    }

    ASSERT_TRUE(setup_error_.empty()) << setup_error_;
    ASSERT_NE(model_, nullptr);
    ASSERT_EQ(model_->GetGenAIConfig().GetChatBackendKind(), ChatBackendKind::kEngine);
    ASSERT_NE(model_->GetChatEngine(), nullptr);
  }

  static inline Manager* manager_ = nullptr;
  static inline ModelLoadManager* load_manager_ = nullptr;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline std::string setup_error_;
  static inline std::filesystem::path model_path_;
};

TEST_F(Dflash2EngineQualificationTest, UsesDraftingForDefaultAndExplicitGreedyTurns) {
  auto& engine = *model_->GetChatEngine();

  SearchOptions omitted_sampling;
  omitted_sampling.max_output_tokens = 64;

  const auto omitted = RunTurn(engine, *model_, omitted_sampling, "List three ways to debug a failing unit test.");
  const auto omitted_delta = Delta(omitted.before, omitted.after);
  ASSERT_TRUE(omitted.succeeded) << omitted.error;
  EXPECT_GT(omitted.turn_result.generated_tokens, 0u);
  EXPECT_GT(omitted_delta.rounds, 0u);
  EXPECT_TRUE(HasDraftActivity(omitted_delta));
  ExpectHealthyDraftActivity(omitted_delta);

  SearchOptions explicit_greedy;
  explicit_greedy.max_output_tokens = 64;
  explicit_greedy.do_sample = false;

  const auto greedy = RunTurn(engine, *model_, explicit_greedy, "Explain how to narrow a regression to one commit.");
  const auto greedy_delta = Delta(greedy.before, greedy.after);
  ASSERT_TRUE(greedy.succeeded) << greedy.error;
  EXPECT_GT(greedy.turn_result.generated_tokens, 0u);
  EXPECT_GT(greedy_delta.rounds, 0u);
  EXPECT_TRUE(HasDraftActivity(greedy_delta));
  ExpectHealthyDraftActivity(greedy_delta);

  SearchOptions neutral_temperature;
  neutral_temperature.max_output_tokens = 64;
  neutral_temperature.temperature = 1.0f;

  const auto neutral =
      RunTurn(engine, *model_, neutral_temperature, "Describe a focused test for a small bug fix.");
  const auto neutral_delta = Delta(neutral.before, neutral.after);
  ASSERT_TRUE(neutral.succeeded) << neutral.error;
  EXPECT_GT(neutral.turn_result.generated_tokens, 0u);
  EXPECT_GT(neutral_delta.rounds, 0u);
  EXPECT_TRUE(HasDraftActivity(neutral_delta));
  ExpectHealthyDraftActivity(neutral_delta);
}

TEST_F(Dflash2EngineQualificationTest, UsesTargetOnlyGenerationForSampledTurns) {
  auto& engine = *model_->GetChatEngine();

  SearchOptions explicit_sample;
  explicit_sample.max_output_tokens = 64;
  explicit_sample.do_sample = true;
  const auto explicit_result =
      RunTurn(engine, *model_, explicit_sample, "Suggest a few different names for a helper function.");
  const auto explicit_delta = Delta(explicit_result.before, explicit_result.after);
  ASSERT_TRUE(explicit_result.succeeded) << explicit_result.error;
  EXPECT_GT(explicit_result.turn_result.generated_tokens, 0u);
  ExpectNoDraftActivity(explicit_delta);

  SearchOptions temperature_sample;
  temperature_sample.max_output_tokens = 64;
  temperature_sample.temperature = 0.7f;
  const auto temperature_result =
      RunTurn(engine, *model_, temperature_sample, "Write a creative variable name for a temporary buffer.");
  const auto temperature_delta = Delta(temperature_result.before, temperature_result.after);
  ASSERT_TRUE(temperature_result.succeeded) << temperature_result.error;
  EXPECT_GT(temperature_result.turn_result.generated_tokens, 0u);
  ExpectNoDraftActivity(temperature_delta);
}

}  // namespace
}  // namespace fl
