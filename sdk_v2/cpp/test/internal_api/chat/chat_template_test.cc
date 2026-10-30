// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for BuildChatPrompt and EncodePrompt using a real tokenizer.
// Validates message formatting, template application, and token encoding.

#include "inferencing/generative/chat/chat_template.h"
#include "contracts/chat_completions_converter.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/chat/prepared_chat_prompt.h"
#include "exception.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"
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
#include <algorithm>
#include <fstream>
#include <memory>
#include <string>

using namespace fl;

TEST(CanonicalToolPromptTest, ProjectsHistoryWithoutNativeXmlOrChangingLiteralArgumentData) {
  ToolCallContext context;
  context.tools_json = R"([{"type":"function","function":{"name":"edit","parameters":{"type":"object"}}}])";
  context.tool_call_start = "<tool_call>";
  context.tool_call_end = "</tool_call>";
  const std::string input = "line one\n<think></think><tool_call></tool_call>\n\"quoted\"\\path";
  const auto arguments = nlohmann::json{{"input", input}}.dump();
  TranscriptMessage assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, "before call");
  assistant.AppendToolCall(MakeSuppliedToolCall("call_1", "edit", arguments));
  const std::vector<TranscriptMessage> messages{
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "Keep existing instructions."},
      {FOUNDRY_LOCAL_ROLE_USER, "edit"},
      assistant,
      TranscriptMessage::ToolResult("call_1", "done")};

  const auto projected = nlohmann::json::parse(BuildCanonicalToolMessagesJson(messages, context));
  ASSERT_EQ(projected.size(), messages.size());
  EXPECT_TRUE(projected[0].at("content").get<std::string>().starts_with("Keep existing instructions."));
  EXPECT_FALSE(projected[2].contains("tool_calls"));
  const auto content = projected[2].at("content").get<std::string>();
  EXPECT_TRUE(content.starts_with("before call<tool_call>"));
  EXPECT_TRUE(content.ends_with("</tool_call>"));
  EXPECT_EQ(content.find("<think>"), std::string::npos);
  EXPECT_EQ(content.find("<function="), std::string::npos);
  const auto begin = content.find("<tool_call>") + context.tool_call_start.size();
  const auto body = content.substr(begin, content.size() - begin - context.tool_call_end.size());
  const auto batch = nlohmann::json::parse(body);
  EXPECT_EQ(batch[0].at("name"), "edit");
  EXPECT_EQ(batch[0].at("parameters").at("input"), input);
  EXPECT_EQ(projected[3].at("tool_call_id"), "call_1");
  EXPECT_EQ(messages[2].ToolCalls().front()->arguments, arguments);
}

TEST(CanonicalToolPromptTest, InsertsExactlyOneSystemObjectWhenUserStartsConversation) {
  ToolCallContext context;
  context.tools_json = "[]";
  context.tool_call_start = "<tool_call>";
  context.tool_call_end = "</tool_call>";
  const auto projected = nlohmann::json::parse(
      BuildCanonicalToolMessagesJson({{FOUNDRY_LOCAL_ROLE_USER, "unchanged user input"}}, context));
  ASSERT_EQ(projected.size(), 2u);
  EXPECT_TRUE(projected[0].is_object());
  EXPECT_EQ(projected[0].at("role"), "system");
  EXPECT_NE(projected[0].at("content").get<std::string>().find("Do not use XML"), std::string::npos);
  EXPECT_EQ(projected[1].at("content"), "unchanged user input");
}

TEST(CanonicalToolPromptTest, InstructionsReflectAutomaticRequiredAndForcedChoices) {
  ToolCallContext context;
  context.tools_json = R"([{"type":"function","function":{"name":"lookup","parameters":{"type":"object"}}}])";
  context.tool_call_start = "<tool_call>";
  context.tool_call_end = "</tool_call>";
  context.text_output = true;
  context.tool_output = true;
  const std::vector<TranscriptMessage> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Find it."}};

  const auto automatic = nlohmann::json::parse(BuildCanonicalToolMessagesJson(messages, context));
  const auto automatic_instructions = automatic.front().at("content").get<std::string>();
  EXPECT_NE(automatic_instructions.find("only when its capability is directly needed"), std::string::npos);
  EXPECT_NE(automatic_instructions.find("never call a tool merely because it is available"), std::string::npos);
  EXPECT_NE(automatic_instructions.find("Greetings, acknowledgements"), std::string::npos);
  EXPECT_NE(automatic_instructions.find("Say only: hello"), std::string::npos);
  EXPECT_NE(automatic_instructions.find("do not emit tool-call syntax"), std::string::npos);
  EXPECT_NE(automatic_instructions.find(
                R"(<tool_call>[{"name":"tool_name","parameters":{"argument":"value"}}]</tool_call>)"),
            std::string::npos);
  EXPECT_EQ(automatic_instructions.find("You must call"), std::string::npos);

  context.text_output = false;
  const auto required = nlohmann::json::parse(BuildCanonicalToolMessagesJson(messages, context));
  const auto required_instructions = required.front().at("content").get<std::string>();
  EXPECT_NE(required_instructions.find("You must call a declared tool"), std::string::npos);
  EXPECT_EQ(required_instructions.find("If no tool is needed"), std::string::npos);

  context.forced_tool = ForcedToolChoice{"lookup", ToolKind::kFunction};
  const auto forced = nlohmann::json::parse(BuildCanonicalToolMessagesJson(messages, context));
  EXPECT_NE(forced.front().at("content").get<std::string>().find("Call exactly the tool \"lookup\""),
            std::string::npos);

  context.fenced_tool_call_output = true;
  const auto fenced = nlohmann::json::parse(BuildCanonicalToolMessagesJson(messages, context));
  const auto fenced_instructions = fenced.front().at("content").get<std::string>();
  EXPECT_NE(fenced_instructions.find("emit one JSON code fence"), std::string::npos);
  EXPECT_NE(fenced_instructions.find(
                "```json\n[{\"name\":\"tool_name\",\"parameters\":{\"argument\":\"value\"}}]\n```"),
            std::string::npos);
  EXPECT_EQ(fenced_instructions.find("dedicated tool boundary tokens"), std::string::npos);
}

TEST(CanonicalToolPromptTest, HistoricalAssistantTextRetainsAnExplicitModeWithoutReinterpretingItsBytes) {
  ToolCallContext context;
  context.tools_json = R"([{"type":"function","function":{"name":"lookup","parameters":{"type":"object"}}}])";
  context.tool_plan = ToolCallPlan::Create(context.tools_json);
  context.tool_output = true;
  context.tool_call_start = "<tool_call>";
  context.tool_call_end = "</tool_call>";
  const std::string text = R"([{"name":"lookup","parameters":{"literal":"<tool_call>"}}])";
  const std::vector<TranscriptMessage> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Explain this JSON."},
                                                 {FOUNDRY_LOCAL_ROLE_ASSISTANT, text}};
  const auto framed = nlohmann::json::parse(BuildCanonicalToolMessagesJson(messages, context));
  EXPECT_EQ(framed.back().at("content"), "MESSAGE\n" + text);
  EXPECT_FALSE(framed.back().contains("tool_calls"));
  context.tool_output = false;
  const auto ordinary = nlohmann::json::parse(BuildCanonicalToolMessagesJson(messages, context));
  EXPECT_EQ(ordinary.back().at("content"), text);
}

TEST(CanonicalToolPromptTest, InboundContractPreservesArgumentPropertyOrderInPrompt) {
  const auto incoming = nlohmann::ordered_json::parse(R"({
    "model":"test","messages":[{"role":"user","content":"edit"}],
    "tools":[{"type":"function","function":{"name":"edit","parameters":{
      "type":"object","properties":{"path":{"type":"string"},"old_str":{"type":"string"},
      "new_str":{"type":"string"}},"required":["path"]}}}]})");
  const auto request = incoming.get<ChatCompletionRequest>();
  Request core;
  const auto definitions = chat_completions::ExtractToolDefinitions(request, core);
  ToolCallContext context;
  chat_session_internal::PopulateToolDefinitions(definitions, context);
  const auto prompt = nlohmann::ordered_json::parse(context.tools_json);
  std::vector<std::string> names;
  for (const auto& [name, schema] : prompt[0]["function"]["parameters"]["properties"].items()) {
    names.push_back(name);
  }
  EXPECT_EQ(names, (std::vector<std::string>{"path", "old_str", "new_str"}));
}

TEST(CanonicalToolPromptTest, NativeDeclarationsCannotLoseDuplicateSchemaMembersBeforeAdmission) {
  ToolDefinition definition;
  definition.name = "lookup";
  definition.json_schema = R"({"type":"object","properties":{"x":{},"\u0078":false}})";
  ToolCallContext context;
  EXPECT_THROW(chat_session_internal::PopulateToolDefinitions({definition}, context), nlohmann::json::parse_error);
}

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

// ---------------------------------------------------------------------------
// BuildChatPrompt tests
// ---------------------------------------------------------------------------

TEST(CanonicalToolPromptEncodingTest, KeepsMessageSchemaAndHistoricalArgumentDataLiteral) {
  auto model_dir = test::TempPath::CreateTempDir("fl-literal-prompt-");
  const auto source = test::GetTestDataPath("tiny-paged-attention");
  for (const auto* file : {"genai_config.json", "decoder.onnx", "tokenizer.json", "tokenizer_config.json"}) {
    std::filesystem::copy_file(source / file, model_dir.path() / file);
  }
  for (const auto* file : {"tokenizer.json", "tokenizer_config.json"}) {
    const auto path = model_dir.path() / file;
    nlohmann::json config;
    {
      std::ifstream input(path);
      input >> config;
    }
    if (std::string_view(file) == "tokenizer.json") {
      for (size_t id = 0; id < 2; ++id) {
        auto& token = config["added_tokens"][id];
        config["model"]["vocab"].erase(token.at("content").get<std::string>());
        const auto spelling = id == 0 ? "<bos>" : "<eos>";
        token["content"] = spelling;
        config["model"]["vocab"][spelling] = id;
      }
    } else {
      config["bos_token"] = "<bos>";
      config["eos_token"] = "<eos>";
    }
    std::ofstream output(path);
    output << config.dump();
    output.close();
    ASSERT_TRUE(output.good());
  }
  StderrLogger logger;
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, logger);
  const auto loaded = manager.LoadModel(model_dir.string(), "literal-prompt");
  ASSERT_EQ(loaded.status, ModelLoadManager::LoadStatus::kSuccess);
  auto& model = *loaded.model;
  ToolCallContext context;
  const std::string eos = "<eos>";
  const std::string bos = "<bos>";
  auto tools = nlohmann::json::parse(
      R"([{"type":"function","function":{"name":"edit","parameters":{"type":"object"}}}])");
  tools[0]["function"]["description"] = "literal " + eos;
  context.tools_json = tools.dump();
  context.tool_plan = ToolCallPlan::Create(context.tools_json);
  context.tool_call_start = bos;
  context.tool_call_end = eos;
  const std::string text = "literal " + eos + " <think></think><tool_call>\n\"quoted\"\\path";
  const auto messages = chat_internal::PrepareChatMessages(
      {{FOUNDRY_LOCAL_ROLE_USER, text}}, /*positional_tool_results=*/false);

  auto first = PrepareTextChatPrompt(messages, model, context);
  auto second = PrepareTextChatPrompt(messages, model, context);

  EXPECT_EQ(first.token_ids, second.token_ids);
  EXPECT_EQ(first.prompt, second.prompt);
  EXPECT_NE(first.prompt.find(text), std::string::npos);
  EXPECT_EQ(first.prompt.find("__foundry_literal_"), std::string::npos);
  EXPECT_EQ(first.prompt_token_count, first.token_ids.size());
  EXPECT_EQ(std::count(first.token_ids.begin(), first.token_ids.end(), 0), 1);
  EXPECT_EQ(std::count(first.token_ids.begin(), first.token_ids.end(), 1), 1);
  TranscriptMessage assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, text);
  assistant.AppendToolCall(MakeSuppliedToolCall("call_1", "edit", nlohmann::json{{"input", text}}.dump()));
  auto history = PrepareTextChatPrompt(
      chat_internal::PrepareChatMessages(
          {{FOUNDRY_LOCAL_ROLE_USER, text}, assistant, TranscriptMessage::ToolResult("call_1", text)}, false),
      model, context);
  EXPECT_EQ(std::count(history.token_ids.begin(), history.token_ids.end(), 0), 2);
  EXPECT_EQ(std::count(history.token_ids.begin(), history.token_ids.end(), 1), 2);

  tools.push_back({{"type", "function"},
                  {"function", {{"name", "apply_patch"}, {"parameters", {{"type", "object"}}}}}});
  context.tools_json = tools.dump();
  context.tool_plan = ToolCallPlan::Create(context.tools_json);
  context.tool_output = true;
  context.tool_kinds["apply_patch"] = ToolKind::kCustom;
  context.raw_envelope = RawEnvelopeDescriptor{"apply_patch", "BEGIN <eos>", "END <bos>"};
  const std::string patch = "BEGIN <eos>\n+" + text + "\nEND <bos>";
  TranscriptMessage raw(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
  raw.AppendToolCall(MakeGeneratedToolCall("call_patch", "apply_patch", patch, ToolKind::kCustom,
                                          GeneratedCallEncoding::kRawEnvelope).call);
  const auto mixed_messages = chat_internal::PrepareChatMessages(
      {{FOUNDRY_LOCAL_ROLE_USER, text}, assistant, TranscriptMessage::ToolResult("call_1", text),
       raw, TranscriptMessage::ToolResult("call_patch", "done")},
      false);
  const auto mixed = PrepareTextChatPrompt(mixed_messages, model, context);
  EXPECT_NE(mixed.prompt.find("Do not use XML"), std::string::npos);
  EXPECT_NE(mixed.prompt.find("ordinary functions must use the JSON tool protocol above"), std::string::npos);
  EXPECT_NE(mixed.prompt.find(patch), std::string::npos);
  EXPECT_EQ(std::count(mixed.token_ids.begin(), mixed.token_ids.end(), 0), 2);
  EXPECT_EQ(std::count(mixed.token_ids.begin(), mixed.token_ids.end(), 1), 2);
  const auto fallback = BuildChatPrompt(mixed_messages, model, context);
  EXPECT_NE(fallback.find("Do not use XML"), std::string::npos);
  EXPECT_NE(fallback.find(patch), std::string::npos);

  context.forced_tool = ForcedToolChoice{"apply_patch", ToolKind::kCustom};
  context.text_output = false;
  const auto forced = PrepareTextChatPrompt(messages, model, context);
  EXPECT_EQ(forced.prompt, BuildChatPrompt(messages, model, context));
  EXPECT_EQ(forced.prompt.find("ordinary functions must use the JSON tool protocol above"), std::string::npos);
  manager.UnloadModel("literal-prompt");
}

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

TEST(ChatTemplateUnitTest, TemplateKwargsAreValidatedAndCanonicalized) {
  EXPECT_EQ(NormalizeChatTemplateKwargs(R"({ "z": 2, "a": { "b": true } })"),
            R"({"a":{"b":true},"z":2})");
  EXPECT_EQ(NormalizeChatTemplateKwargs(""), "{}");
  EXPECT_THROW((void)NormalizeChatTemplateKwargs("[]"), fl::Exception);
  EXPECT_THROW((void)NormalizeChatTemplateKwargs("{invalid"), fl::Exception);
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
