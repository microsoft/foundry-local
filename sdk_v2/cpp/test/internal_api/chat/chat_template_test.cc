// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for BuildChatPrompt and EncodePrompt using a real tokenizer.
// Validates message formatting, template application, and token encoding.

#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_session.h"
#include "exception.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/model_load_manager.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"
#include "model.h"
#include "items/text_item.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "utils/temp_path.h"
#include "telemetry/telemetry_logger.h"

#include <ort_genai.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace fl;

// ---------------------------------------------------------------------------
// Test fixture: loads the shared test model once per suite
// ---------------------------------------------------------------------------

class ChatTemplateTest : public ::testing::Test {
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

  GenAIModelInstance& GetModel() { return *model_; }

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
};

#if FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS
class ChatTemplateKwargsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto source = test::GetTestDataPath("tiny-paged-attention");
    for (const auto* file : {"genai_config.json", "decoder.onnx", "tokenizer.json", "tokenizer_config.json"}) {
      std::filesystem::copy_file(source / file, model_dir_.path() / file);
    }

    const auto config_path = model_dir_.path() / "tokenizer_config.json";
    nlohmann::json config;
    {
      std::ifstream input(config_path);
      input >> config;
    }

    config["chat_template"] =
        R"({% if enable_thinking is not defined %}[default])"
        R"({% elif enable_thinking %}[thinking]{% else %}[no-thinking]{% endif %})"
        R"({% if label is defined %}{{ label }}|{{ count + 1 }}|)"
        R"({% if nested.enabled %}on{% else %}off{% endif %}|{{ values[1] }}{% endif %})" +
        config.at("chat_template").get<std::string>();
    {
      std::ofstream output(config_path);
      output << config.dump();
      output.close();
      ASSERT_TRUE(output.good()) << "Failed to write tokenizer config: " << config_path;
    }

    const auto result = load_manager_.LoadModel(model_dir_.string(), "chat-template-kwargs");
    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess);
    model_ = result.model;
  }

  void TearDown() override {
    if (model_) {
      load_manager_.UnloadModel("chat-template-kwargs");
    }
  }

  GenAIModelInstance& GetModel() { return *model_; }

  test::TempPath model_dir_ = test::TempPath::CreateTempDir("fl-chat-template-kwargs-");
  StderrLogger logger_;
  test::CpuOnlyEpDetector ep_detector_;
  ModelLoadManager load_manager_{ep_detector_, logger_};
  GenAIModelInstance* model_ = nullptr;

  const Model& GetCatalogModel() {
    static test::FakeServiceBindings services;
    static Model model = [] {
      ModelInfo info;
      info.task = "chat-completion";
      return Model::FromModelInfo(std::move(info), "", services.download_manager, services.model_load_manager);
    }();
    return model;
  }

  void SetSessionDefaults(ChatSession& session, const char* kwargs) {
    KeyValuePairs options;
    options.Add("max_output_tokens", "4");
    options.Add("temperature", "0");
    if (kwargs) {
      options.Add("chat_template_kwargs", kwargs);
    }

    session.SetSessionOptions(options);
  }

  TelemetryLogger telemetry_{"chat-template-kwargs-test", test::NullLog()};
};
#endif

// ---------------------------------------------------------------------------
// BuildChatPrompt tests
// ---------------------------------------------------------------------------

TEST_F(ChatTemplateTest, SingleUserMessage) {
  std::vector<TranscriptMessage> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  EXPECT_FALSE(prompt.empty());
  // The prompt should contain the user message content
  EXPECT_NE(prompt.find("Hello!"), std::string::npos)
      << "Prompt should contain user message. Got: " << prompt;
}

TEST_F(ChatTemplateTest, SystemAndUserMessages) {
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a helpful assistant."},
      {FOUNDRY_LOCAL_ROLE_USER, "What is 2+2?"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  EXPECT_FALSE(prompt.empty());
  EXPECT_NE(prompt.find("helpful assistant"), std::string::npos);
  EXPECT_NE(prompt.find("2+2"), std::string::npos);
}

TEST_F(ChatTemplateTest, MultiTurnConversation) {
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a math tutor."},
      {FOUNDRY_LOCAL_ROLE_USER, "What is 2+2?"},
      {FOUNDRY_LOCAL_ROLE_ASSISTANT, "4"},
      {FOUNDRY_LOCAL_ROLE_USER, "What about 3+3?"}};
  std::string prompt = BuildChatPrompt(messages, GetModel());
  EXPECT_FALSE(prompt.empty());
  // Multi-turn should contain all messages
  EXPECT_NE(prompt.find("math tutor"), std::string::npos);
  EXPECT_NE(prompt.find("2+2"), std::string::npos);
  EXPECT_NE(prompt.find("3+3"), std::string::npos);
}

TEST(ChatTemplateUnitTest, EmptyAssistantMessageRendersAsEmptyContent) {
  MessageItem empty_assistant;
  empty_assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;

  EXPECT_EQ(RenderMessageForPrompt(empty_assistant), "");
}

TEST_F(ChatTemplateTest, EmptyMessagesThrows) {
  std::vector<TranscriptMessage> messages;
  EXPECT_THROW(BuildChatPrompt(messages, GetModel()), fl::Exception);
}

TEST_F(ChatTemplateTest, PromptEndsWithAssistantPrefix) {
  // When add_generation_prompt=true, the template should end with the
  // assistant turn prefix so the model continues generating.
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  // Qwen2.5 uses <|im_start|>assistant format
  EXPECT_NE(prompt.find("assistant"), std::string::npos)
      << "Prompt should end with assistant prefix for generation. Got: " << prompt;
}

#if FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS
TEST_F(ChatTemplateKwargsTest, TypedKwargsChangePromptAndOmissionClearsPriorState) {
  std::vector<TranscriptMessage> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};
  ToolCallContext default_context;
  ToolCallContext thinking_context;
  thinking_context.template_kwargs_json = R"({"enable_thinking":true})";
  ToolCallContext no_thinking_context;
  no_thinking_context.template_kwargs_json = R"({"enable_thinking":false})";
  ToolCallContext typed_context;
  typed_context.template_kwargs_json =
      R"({"enable_thinking":false,"label":"typed","count":2,"nested":{"enabled":false},"values":["one","two"]})";

  std::string default_prompt = BuildChatPrompt(messages, GetModel(), default_context);
  std::string thinking_prompt = BuildChatPrompt(messages, GetModel(), thinking_context);
  std::string no_thinking_prompt = BuildChatPrompt(messages, GetModel(), no_thinking_context);
  std::string typed_prompt = BuildChatPrompt(messages, GetModel(), typed_context);
  std::string default_prompt_after_kwargs =
      BuildChatPrompt(messages, GetModel(), default_context);

  EXPECT_EQ(default_prompt, "[default][user]Hello![assistant]");
  EXPECT_EQ(thinking_prompt, "[thinking][user]Hello![assistant]");
  EXPECT_EQ(no_thinking_prompt, "[no-thinking][user]Hello![assistant]");
  EXPECT_EQ(typed_prompt, "[no-thinking]typed|3|off|two[user]Hello![assistant]");
  EXPECT_EQ(default_prompt_after_kwargs, default_prompt)
      << "Omitting kwargs must clear tokenizer state from the previous render";
}

TEST_F(ChatTemplateKwargsTest, SessionDefaultsAreOverriddenOrClearedByRequestOptions) {
  struct Case {
    const char* defaults;
    const char* request;
    const char* prefix;
  };
  const Case cases[] = {
      {R"({"enable_thinking":false})", nullptr, "[no-thinking]"},
      {R"({"enable_thinking":false})", R"({"enable_thinking":true})", "[thinking]"},
      {R"({"enable_thinking":true})", "{}", "[default]"},
      {R"({"enable_thinking":true})", "", "[default]"},
      {R"({"enable_thinking":true,"label":"inherited"})", R"({"enable_thinking":false})", "[no-thinking]"},
      {nullptr, nullptr, "[default]"},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.prefix);
    ChatSession session(GetCatalogModel(), GetModel(), logger_, telemetry_);
    SetSessionDefaults(session, test_case.defaults);

    Request request;
    request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Hello!"));
    if (test_case.request) {
      request.options.Add("chat_template_kwargs", test_case.request);
    }

    Response response;
    session.ProcessRequest(request, response);
    ASSERT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_LENGTH);
    const std::string expected_prompt = std::string(test_case.prefix) + "[user]Hello![assistant]";
    EXPECT_EQ(response.usage.prompt_tokens, static_cast<int64_t>(expected_prompt.size()));
  }
}

TEST_F(ChatTemplateKwargsTest, JsonPayloadOverridesRequestOptionsThenSessionDefaults) {
  struct Case {
    const char* defaults;
    const char* request;
    const char* payload;
    const char* prefix;
  };
  const Case cases[] = {
      {R"({"enable_thinking":false})", nullptr, nullptr, "[no-thinking]"},
      {R"({"enable_thinking":false})", R"({"enable_thinking":true})", nullptr, "[thinking]"},
      {R"({"enable_thinking":true})", R"({"enable_thinking":true})", R"({"enable_thinking":false})", "[no-thinking]"},
      {R"({"enable_thinking":false})", R"({"enable_thinking":true})", "{}", "[default]"},
      {R"({"enable_thinking":false})", nullptr, "null", "[no-thinking]"},
      {R"({"enable_thinking":true})", "{}", nullptr, "[default]"},
      {nullptr, nullptr, nullptr, "[default]"},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.prefix);
    ChatSession session(GetCatalogModel(), GetModel(), logger_, telemetry_);
    SetSessionDefaults(session, test_case.defaults);

    nlohmann::json payload = {
        {"model", "chat-template-kwargs"},
        {"messages", {{{"role", "user"}, {"content", "Hello!"}}}},
    };
    if (test_case.payload) {
      payload["chat_template_kwargs"] = nlohmann::json::parse(test_case.payload);
    }

    Request request;
    request.AddOwnedItem(std::make_unique<TextItem>(payload.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    if (test_case.request) {
      request.options.Add("chat_template_kwargs", test_case.request);
    }

    Response response;
    session.ProcessRequest(request, response);
    ASSERT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_LENGTH);
    const std::string expected_prompt = std::string(test_case.prefix) + "[user]Hello![assistant]";
    EXPECT_EQ(response.usage.prompt_tokens, static_cast<int64_t>(expected_prompt.size()));
    ASSERT_EQ(response.items.size(), 1u);
    ASSERT_EQ(response.items.front()->type, FOUNDRY_LOCAL_ITEM_TEXT);
    const auto& text = static_cast<const TextItem&>(*response.items.front());
    const auto completion = nlohmann::json::parse(text.text);
    EXPECT_EQ(completion.at("usage").at("prompt_tokens"), expected_prompt.size());
    EXPECT_EQ(session.TurnCount(), 0u);
  }
}
#else
TEST_F(ChatTemplateTest, TemplateKwargsRequireSupportedGenAI) {
  std::vector<TranscriptMessage> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};
  ToolCallContext empty_context;
  empty_context.template_kwargs_json = "{}";

  EXPECT_EQ(BuildChatPrompt(messages, GetModel(), empty_context),
            BuildChatPrompt(messages, GetModel()))
      << "An empty kwargs object should remain a no-op with an older GenAI dependency";

  try {
    (void)BuildChatPrompt(messages, GetModel(), "", R"({"enable_thinking":false})");
    FAIL() << "Expected chat_template_kwargs to be rejected by an older GenAI dependency";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_NE(std::string(e.what()).find("requires a build with tokenizer kwargs support enabled"),
              std::string::npos);
  }
}
#endif

// ---------------------------------------------------------------------------
// EncodePrompt tests
// ---------------------------------------------------------------------------

TEST_F(ChatTemplateTest, EncodeProducesTokens) {
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_USER, "Hello!"}};

  std::string prompt = BuildChatPrompt(messages, GetModel());
  auto sequences = EncodePrompt(prompt, GetModel());

  ASSERT_NE(sequences, nullptr);
  size_t token_count = sequences->SequenceCount(0);
  EXPECT_GT(token_count, 0u) << "Encoded prompt should have at least 1 token";
}

TEST_F(ChatTemplateTest, LongerMessageProducesMoreTokens) {
  std::vector<TranscriptMessage> short_msgs = {
      {FOUNDRY_LOCAL_ROLE_USER, "Hi"}};
  std::vector<TranscriptMessage> long_msgs = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are a detailed technical writer who explains everything thoroughly."},
      {FOUNDRY_LOCAL_ROLE_USER,
       "Explain the theory of relativity in detail, covering both special and general relativity."}};
  std::string short_prompt = BuildChatPrompt(short_msgs, GetModel());
  std::string long_prompt = BuildChatPrompt(long_msgs, GetModel());

  auto short_seq = EncodePrompt(short_prompt, GetModel());
  auto long_seq = EncodePrompt(long_prompt, GetModel());

  EXPECT_GT(long_seq->SequenceCount(0), short_seq->SequenceCount(0))
      << "Longer message should produce more tokens";
}

TEST_F(ChatTemplateTest, EmptyStringEncodesSuccessfully) {
  // Even an empty string should encode without crashing
  auto sequences = EncodePrompt("", GetModel());
  ASSERT_NE(sequences, nullptr);
}
