// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "exception.h"
#include "contracts/tool_definitions.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/chat/harmony_decoder.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/generative/genai_model_instance.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "internal_api/web_service_test_helpers.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "utils/temp_path.h"
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include "service/responses_handler.h"
#include "service/web_service.h"
#include "inferencing/session/session_manager.h"
#include <oatpp/core/data/stream/BufferStream.hpp>
#include <oatpp/web/protocol/http/incoming/SimpleBodyDecoder.hpp>
#endif
#include "inferencing/generative/openresponses/response_converter.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_plan.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

using namespace fl;

namespace {

const HarmonyProtocol kProtocol{
    10, 11, 12, 13, 14, 15, 16, {10, 17}};

constexpr const char* kTools = R"([
  {"type":"function","function":{
    "name":"lookup",
    "parameters":{
      "type":"object",
      "properties":{"city":{"type":"string"}},
      "required":["city"]
    }
  }},
  {"type":"function","function":{
    "name":"patch",
    "parameters":{
      "type":"object",
      "properties":{"input":{"type":"string"}},
      "required":["input"]
    }
  }}
])";

PreparedChatPrompt MakePrompt() {
  PreparedChatPrompt prompt;
  prompt.prompt = "prefix<|start|>assistant";
  prompt.token_ids = {99, kProtocol.start, 17};
  prompt.harmony_continuation = HarmonyContinuation{
      "assistant", kProtocol.assistant_prefix_tokens.size(),
      std::char_traits<char>::length(HarmonyProtocol::kAssistantPrefix)};
  return prompt;
}

void PushHeader(HarmonyDecoder& decoder, std::string recipient, bool json) {
  EXPECT_FALSE(decoder.Push(100, " to=functions.").malformed);
  EXPECT_FALSE(decoder.Push(101, std::move(recipient)).malformed);
  EXPECT_FALSE(decoder.Push(kProtocol.channel, {}).malformed);
  EXPECT_FALSE(decoder.Push(102, "commentary").malformed);
  if (json) {
    EXPECT_FALSE(decoder.Push(kProtocol.format, {}).malformed);
    EXPECT_FALSE(decoder.Push(103, "json").malformed);
  }
  EXPECT_FALSE(decoder.Push(kProtocol.message, {}).malformed);
}

const ParsedToolCall& OnlyCall(const HarmonyDecoder::Output& output) {
  EXPECT_FALSE(output.malformed) << output.error;
  EXPECT_EQ(output.events.size(), 1u);
  const auto* call = output.events.empty() ? nullptr : std::get_if<ParsedToolCall>(&output.events.front());
  EXPECT_NE(call, nullptr);
  static const ParsedToolCall empty;
  return call == nullptr ? empty : *call;
}

}  // namespace

TEST(HarmonyDecoderTest, UsesControlIdsWithEmptyDecodedTextAndAcceptsSplitJsonHeader) {
  auto prompt = MakePrompt();
  HarmonyDecoder decoder(kProtocol, ToolCallPlan::Create(kTools), prompt);

  PushHeader(decoder, "lookup", true);
  constexpr std::string_view arguments = R"({"city": "Paris"})";
  EXPECT_FALSE(decoder.Push(104, std::string(arguments)).malformed);
  const auto output = decoder.Finish(kProtocol.handoff, true);
  const auto& call = OnlyCall(output);

  EXPECT_EQ(call.name, "lookup");
  EXPECT_EQ(call.arguments, arguments);
  EXPECT_EQ(call.argument_source, arguments);
  EXPECT_FALSE(call.id.empty());
}

TEST(HarmonyDecoderTest, LiteralControlSpellingsCannotManufactureHeaderMetadata) {
  auto prompt = MakePrompt();
  HarmonyDecoder decoder(kProtocol, ToolCallPlan::Create(kTools), prompt);

  EXPECT_FALSE(decoder.Push(100, "<|channel|>commentary").malformed);
  const auto output = decoder.Push(kProtocol.message, {});

  EXPECT_TRUE(output.malformed);
  EXPECT_NE(output.error.find("exactly one channel"), std::string::npos);
}

TEST(HarmonyDecoderTest, TerminalMetadataCompletesToolAndFinalMessages) {
  auto tool_prompt = MakePrompt();
  HarmonyDecoder tool_decoder(kProtocol, ToolCallPlan::Create(kTools), tool_prompt);
  PushHeader(tool_decoder, "lookup", true);
  EXPECT_FALSE(tool_decoder.Push(104, R"({"city":"Paris"})").malformed);
  EXPECT_EQ(OnlyCall(tool_decoder.Finish(kProtocol.handoff, true)).name, "lookup");

  auto final_prompt = MakePrompt();
  HarmonyDecoder final_decoder(kProtocol, nullptr, final_prompt);
  EXPECT_FALSE(final_decoder.Push(kProtocol.channel, {}).malformed);
  EXPECT_FALSE(final_decoder.Push(105, "final").malformed);
  EXPECT_FALSE(final_decoder.Push(kProtocol.message, {}).malformed);
  EXPECT_FALSE(final_decoder.Push(106, "done").malformed);
  const auto final = final_decoder.Finish(kProtocol.answer, true);
  EXPECT_FALSE(final.malformed) << final.error;
  EXPECT_TRUE(final.events.empty());
}

TEST(HarmonyDecoderTest, InterruptedOrMalformedCallsFailWithoutPublishing) {
  auto interrupted_prompt = MakePrompt();
  HarmonyDecoder interrupted(kProtocol, ToolCallPlan::Create(kTools), interrupted_prompt);
  PushHeader(interrupted, "lookup", true);
  EXPECT_FALSE(interrupted.Push(104, R"({"city":"Paris"})").malformed);
  const auto interrupted_output = interrupted.Finish(std::nullopt, true);
  EXPECT_TRUE(interrupted_output.malformed);
  EXPECT_TRUE(interrupted_output.events.empty());

  auto unauthorized_prompt = MakePrompt();
  HarmonyDecoder unauthorized(kProtocol, ToolCallPlan::Create(kTools), unauthorized_prompt);
  PushHeader(unauthorized, "missing", true);
  EXPECT_FALSE(unauthorized.Push(104, R"({"city":"Paris"})").malformed);
  const auto unauthorized_output = unauthorized.Push(kProtocol.handoff, {});
  EXPECT_TRUE(unauthorized_output.malformed);
  EXPECT_TRUE(unauthorized_output.events.empty());
}

TEST(HarmonyDecoderTest, BudgetInterruptionDiscardsPartialAndCompleteUncommittedArguments) {
  for (const auto* body : {R"({"city":)", R"({"city":"Paris"})"}) {
    auto prompt = MakePrompt();
    HarmonyDecoder decoder(kProtocol, ToolCallPlan::Create(kTools), prompt);
    PushHeader(decoder, "lookup", true);
    ASSERT_FALSE(decoder.Push(104, body).malformed);
    const auto output = decoder.Finish(kProtocol.handoff, false);
    EXPECT_FALSE(output.malformed);
    EXPECT_TRUE(output.events.empty());
  }
}

TEST(HarmonyDecoderTest, SelectedRecipientCapturesARecoveryCheckpointBeforeItsHeader) {
  auto prompt = MakePrompt();
  HarmonyDecoder decoder(kProtocol, ToolCallPlan::Create(kTools), prompt);

  EXPECT_FALSE(decoder.Push(100, " to=func").malformed);
  EXPECT_TRUE(decoder.HasRecoveryCheckpoint());
  EXPECT_FALSE(decoder.Push(101, "tions.lookup").malformed);
  ASSERT_TRUE(decoder.HasRecoveryCheckpoint());

  const auto recovered = decoder.TakeRecoveryPrompt();
  EXPECT_EQ(recovered.prompt, "prefix");
  EXPECT_EQ(recovered.token_ids, (std::vector<int32_t>{99}));
}

TEST(HarmonyDecoderTest, NativeRawBodyUsesInputEnvelopeWithoutLosingSourceBytes) {
  auto prompt = MakePrompt();
  HarmonyDecoder decoder(kProtocol, ToolCallPlan::Create(kTools), prompt, {"patch"});

  PushHeader(decoder, "patch", false);
  const std::string body = "*** Begin Patch\n+literal <|call|>\n*** End Patch";
  EXPECT_FALSE(decoder.Push(104, body).malformed);
  const auto output = decoder.Push(kProtocol.handoff, {});
  const auto& call = OnlyCall(output);
  const auto expected = nlohmann::json{{"input", body}}.dump();

  EXPECT_EQ(call.name, "patch");
  EXPECT_EQ(call.arguments, expected);
  EXPECT_EQ(call.argument_source, expected);
}

TEST(HarmonyDecoderTest, PromptProjectionPreservesNativeJsonRawAndResultProvenance) {
  ToolCallContext context;
  context.harmony = kProtocol;
  context.tools_json = kTools;
  context.tool_kinds = {{"lookup", ToolKind::kFunction}, {"patch", ToolKind::kCustom}};
  context.custom_lark_grammars = {{"patch", "start: /[\\s\\S]+/"}};

  TranscriptMessage raw_assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
  raw_assistant.AppendToolCall(
      MakeGeneratedToolCall("call_raw", "patch", "raw <|call|> text",
                            ToolKind::kCustom, GeneratedCallEncoding::kNativeRaw)
          .call);
  TranscriptMessage json_assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
  const std::string json_arguments = R"({"path":"a\\b","text":"<|call|>"})";
  json_assistant.AppendToolCall(
      MakeGeneratedToolCall("call_json", "lookup", json_arguments,
                            ToolKind::kFunction, GeneratedCallEncoding::kNativeJson)
          .call);
  const std::vector<TranscriptMessage> messages{
      std::move(raw_assistant),
      TranscriptMessage::ToolResult("call_raw", "raw result"),
      std::move(json_assistant),
      TranscriptMessage::ToolResult("call_json", "json result"),
  };

  const auto projection = BuildHarmonyPromptProjection(messages, context, [](std::string value) {
    return value;
  });
  const auto projected = nlohmann::json::parse(projection.messages_json);
  ASSERT_EQ(projected.size(), 5u);
  EXPECT_EQ(projected[1]["tool_calls"][0]["function"]["arguments_json"], "raw <|call|> text");
  EXPECT_TRUE(projected[1]["tool_calls"][0]["function"]["native_freeform"]);
  EXPECT_EQ(projected[2]["content_json"], nlohmann::json("raw result").dump());
  EXPECT_EQ(projected[3]["tool_calls"][0]["function"]["arguments_json"], json_arguments);
  EXPECT_FALSE(projected[3]["tool_calls"][0]["function"]["native_freeform"]);
  EXPECT_EQ(projected[4]["content_json"], nlohmann::json("json result").dump());

  const auto projected_tools = nlohmann::json::parse(projection.tools_json);
  EXPECT_FALSE(projected_tools[0]["function"].contains("native_freeform"));
  EXPECT_TRUE(projected_tools[1]["function"]["native_freeform"].get<bool>());
}

TEST(HarmonyDecoderTest, StoredInputAndOutputReplayRetainHistoricalCustomBodyEncoding) {
  const std::string body = R"({"input":"literal <|call|> and a\\b"})";
  for (const auto encoding : {GeneratedCallEncoding::kStructured, GeneratedCallEncoding::kNativeJson,
                              GeneratedCallEncoding::kNativeRaw}) {
    for (const bool current_raw : {false, true}) {
      for (const bool stored_input : {false, true}) {
        SCOPED_TRACE(static_cast<int>(encoding));
        SCOPED_TRACE(current_raw);
        SCOPED_TRACE(stored_input);
        const auto call = nlohmann::json{{"type", "custom_tool_call"}, {"call_id", "call_history"}, {"name", "patch"}, {"input", body}};
        auto input = nlohmann::json::array({{{"role", "user"}, {"content", "patch"}}});
        if (stored_input) {
          input.push_back(call);
        }
        const auto output = stored_input ? nlohmann::json::array() : nlohmann::json::array({call});
        ResponseStore store;
        store.Store("history", {{"output", output}}, input, "harmony",
                    ToolCallEncodings{{"call_history", encoding}});
        const auto chain = store.BuildChainContext("history");
        ASSERT_TRUE(chain.has_value());
        responses::ResponseCreateParams params;
        params.model = "harmony";
        params.input = "continue";
        auto request = ResponseConverter::ToSessionRequest(params, &*chain);
        const auto replay = IngestRequestItems(request.items, request.item_segment_starts);
        std::size_t call_count = 0;
        for (const auto& message : replay.messages) {
          for (const auto* historical : message.ToolCalls()) {
            ++call_count;
            EXPECT_EQ(historical->generated_encoding, encoding);
            EXPECT_TRUE(historical->has_recorded_encoding);
            EXPECT_EQ(historical->kind, ToolKind::kCustom);
            EXPECT_EQ(historical->arguments, body);
          }
        }
        EXPECT_EQ(call_count, 1u);

        ToolCallContext context;
        context.harmony = kProtocol;
        context.tools_json = kTools;
        context.tool_kinds = {{"patch", ToolKind::kCustom}};
        if (current_raw) {
          context.custom_lark_grammars = {{"patch", "start: /[\\s\\S]+/"}};
        }
        const auto projected = nlohmann::json::parse(
            BuildHarmonyPromptProjection(replay.messages, context, [](std::string value) { return value; })
                .messages_json);
        call_count = 0;
        for (const auto& message : projected) {
          if (!message.contains("tool_calls")) {
            continue;
          }
          for (const auto& historical : message["tool_calls"]) {
            ++call_count;
            const auto& function = historical["function"];
            const bool raw = encoding == GeneratedCallEncoding::kNativeRaw;
            const auto expected = raw ? body : nlohmann::json{{"input", body}}.dump();
            EXPECT_EQ(function["native_freeform"], raw);
            EXPECT_EQ(function["arguments_json"], expected);
          }
        }
        EXPECT_EQ(call_count, 1u);
      }
    }
  }
}

TEST(HarmonyDecoderTest, UnannotatedCallerHistoryStillUsesTheCurrentCustomBodyDeclaration) {
  for (const bool current_raw : {false, true}) {
    ToolCallContext context;
    context.harmony = kProtocol;
    context.tools_json = kTools;
    context.tool_kinds = {{"patch", ToolKind::kCustom}};
    if (current_raw) {
      context.custom_lark_grammars = {{"patch", "start: /[\\s\\S]+/"}};
    }
    const std::string body = R"({"input":"caller text"})";
    TranscriptMessage assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
    assistant.AppendToolCall(MakeSuppliedToolCall("caller", "patch", body, ToolKind::kCustom));
    const auto projected = nlohmann::json::parse(
        BuildHarmonyPromptProjection({assistant}, context, [](std::string value) { return value; })
            .messages_json);
    const auto& function = projected.back()["tool_calls"][0]["function"];
    EXPECT_EQ(function["native_freeform"], current_raw);
    const auto expected = current_raw ? body : nlohmann::json{{"input", body}}.dump();
    EXPECT_EQ(function["arguments_json"], expected);
  }
}

namespace {

using Fragment = std::pair<int32_t, std::string>;

std::vector<Fragment> FinalAnswer(const HarmonyProtocol& protocol) {
  return {{protocol.channel, ""}, {3, "final"}, {protocol.message, ""}, {3, "healthy"}, {protocol.answer, ""}};
}

class HarmonyOutputGenerator final : public ChatGenerator {
 public:
  HarmonyOutputGenerator(std::vector<Fragment> fragments, HarmonyProtocol protocol, bool limited)
      : fragments_(std::move(fragments)), protocol_(std::move(protocol)), limited_(limited) {}
  bool IsDone() const override { return cancelled_ || position_ == fragments_.size(); }
  void GenerateNextToken() override {}
  std::string Decode() override { return fragments_.at(position_++).second; }
  std::optional<int32_t> CurrentTokenId() const override { return fragments_.at(position_).first; }
  int TokenCount() const override { return 4 + static_cast<int>(position_); }
  int PromptTokenCount() const override { return 4; }
  void Cancel() override { cancelled_ = true; }
  int AppendMessages(const std::vector<TranscriptMessage>&, const chat_internal::PreparedChatMessages&,
                     GenAIModelInstance&, const ToolCallContext&, const SearchOptions&) override {
    fragments_ = FinalAnswer(protocol_);
    position_ = 0;
    limited_ = false;
    cancelled_ = false;
    return 4;
  }
  std::optional<ChatTurnUsage> GetTurnUsage() const override {
    return ChatTurnUsage{4, static_cast<int>(position_),
                         limited_ ? FOUNDRY_LOCAL_FINISH_LENGTH : FOUNDRY_LOCAL_FINISH_STOP,
                         limited_ ? BackendTerminationCause::kOutputTokenLimit : BackendTerminationCause::kNaturalEnd};
  }

 private:
  std::vector<Fragment> fragments_;
  HarmonyProtocol protocol_;
  bool limited_;
  bool cancelled_ = false;
  size_t position_ = 0;
};

class HarmonySessionTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
#if !FOUNDRY_LOCAL_OGA_HAS_TERMINAL_TOKEN
    GTEST_SKIP() << "Harmony sessions require the sampled terminal-token API";
#endif
    directory_ = test::MakeUniqueTempPath("harmony_session_");
    std::filesystem::create_directory(directory_);
    const auto source = test::GetTestDataPath("tiny-paged-attention");
    for (const auto& file : std::filesystem::directory_iterator(source)) {
      if (file.is_regular_file()) std::filesystem::copy_file(file.path(), directory_ / file.path().filename());
    }
    const auto read = [](const std::filesystem::path& path) {
      std::ifstream input(path);
      return nlohmann::json::parse(input);
    };
    const auto write = [](const std::filesystem::path& path, const nlohmann::json& value) {
      std::ofstream output(path);
      output << value.dump();
      if (!output) throw std::runtime_error("Cannot write Harmony test fixture");
    };
    auto config = read(directory_ / "genai_config.json");
    config["model"]["type"] = "gptoss";
    config["model"]["eos_token_id"] = {114, 115};
    write(directory_ / "genai_config.json", config);
    auto tokenizer = read(directory_ / "tokenizer.json");
    int id = 110;
    for (const auto* control : {"<|start|>", "<|channel|>", "<|message|>", "<|end|>",
                                "<|call|>", "<|return|>", "<|constrain|>"}) {
      tokenizer["added_tokens"].push_back({{"id", id++}, {"content", control}, {"single_word", false}, {"lstrip", false}, {"rstrip", false}, {"normalized", false}, {"special", true}});
    }
    write(directory_ / "tokenizer.json", tokenizer);
    auto tokenizer_config = read(directory_ / "tokenizer_config.json");
    tokenizer_config["chat_template"] =
        R"({% for tool in tools %}{% if tool.function.native_freeform %}type {{ tool.function.name }} = (_: FREEFORM) => any;{% endif %}{% endfor %})"
        R"({% for message in messages %}{% if message.role == 'assistant' %})"
        R"({% if message.thinking %}<|start|>assistant<|channel|>analysis<|message|>{{ message.thinking }}<|end|>{% endif %})"
        R"({% for call in message.tool_calls %}<|start|>assistant to=functions.{{ call.function.name }}<|channel|>commentary{% if not call.function.native_freeform %} json{% endif %}<|message|>{{ call.function.arguments_json }}<|call|>{% endfor %})"
        R"({% if message.content %}<|start|>assistant<|channel|>final<|message|>{{ message.content }}<|return|>{% endif %})"
        R"({% elif message.role == 'tool' %}<|start|>functions.{{ message.name }} to=assistant<|channel|>commentary<|message|>{{ message.content_json }}<|end|>)"
        R"({% else %}<|start|>{{ message.role }}<|message|>{{ message.content }}<|end|>{% endif %}{% endfor %})"
        R"({% if add_generation_prompt %}<|start|>assistant{% endif %})";
    write(directory_ / "tokenizer_config.json", tokenizer_config);
    logger_ = std::make_unique<StderrLogger>();
    detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    loader_ = std::make_unique<ModelLoadManager>(*detector_, *logger_);
    const auto loaded = loader_->LoadModel(directory_.string(), "harmony-session:1", ExecutionProvider::kCPU);
    ASSERT_EQ(loaded.status, ModelLoadManager::LoadStatus::kSuccess);
    model_ = loaded.model;
    ASSERT_TRUE(model_->GetHarmonyProtocol());
  }
  static void TearDownTestSuite() {
    if (model_) {
      EXPECT_TRUE(loader_->UnloadModel("harmony-session:1"));
    }
    model_ = nullptr;
    loader_.reset();
    detector_.reset();
    logger_.reset();
    if (directory_.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    EXPECT_FALSE(error);
  }
  static Model CatalogModel() {
    auto info = test::MakeTestModelInfo("harmony-session");
    info.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT, 1);
    return Model::FromModelInfo(std::move(info), directory_, services_.download_manager, *loader_);
  }
  TextChatGeneratorFactory Factory(std::string body, std::shared_ptr<int> attempts) {
    const auto protocol = *model_->GetHarmonyProtocol();
    return [protocol, body = std::move(body), attempts](const auto&, const auto&, auto&, const auto&, bool) {
      const bool limited = (*attempts)++ == 0 && !body.empty();
      auto fragments = FinalAnswer(protocol);
      if (limited) {
        fragments = {{protocol.channel, ""}, {3, "commentary"}, {protocol.message, ""}, {3, "prefix"}, {protocol.end, ""}, {protocol.start, ""}, {3, "assistant to=functions.lookup"}, {protocol.channel, ""}, {3, "commentary json"}, {protocol.message, ""}, {3, body}};
      }
      return std::make_unique<HarmonyOutputGenerator>(std::move(fragments), protocol, limited);
    };
  }
  static Request UserRequest() {
    Request request;
    request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "continue"));
    return request;
  }
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
  struct HttpHarness {
    explicit HttpHarness(TelemetryLogger& telemetry)
        : sessions(*logger_), context{catalog, *logger_, directory_.string(), {}, *loader_, sessions, store, telemetry, threads} {
      catalog.AddModel(CatalogModel());
    }
    ~HttpHarness() { threads.JoinAll(); }
    void Seed(TextChatGeneratorFactory factory) {
      auto* catalog_model = catalog.GetModelVariant("harmony-session:1");
      auto session = std::make_unique<ChatSession>(*catalog_model, *model_, *logger_, context.telemetry,
                                                   ChatTranscript::CommitFaultInjector{}, std::move(factory));
      store.Store("seed", {{"output", nlohmann::json::array()}}, nlohmann::json::array(), catalog_model->Id());
      sessions.CheckIn("seed", std::move(session));
    }
    nlohmann::json Post(const nlohmann::json& body) {
      const auto encoded = body.dump();
      using Incoming = HttpRequestHandler::IncomingRequest;
      oatpp::web::protocol::http::Headers headers;
      headers.put("Content-Length", std::to_string(encoded.size()).c_str());
      auto stream = std::make_shared<oatpp::data::stream::BufferInputStream>(oatpp::String(encoded.c_str()));
      auto request = Incoming::createShared(
          nullptr, {"POST", "/v1/responses", "HTTP/1.1"}, headers, stream,
          std::make_shared<oatpp::web::protocol::http::incoming::SimpleBodyDecoder>());
      ResponsesHandler handler(context);
      auto response = handler.handle(request);
      std::string wire;
      char buffer[4096];
      oatpp::async::Action action;
      while (const auto count = response->getBody()->read(buffer, sizeof(buffer), action)) {
        if (count < 0) throw std::runtime_error("Cannot read Responses handler body");
        wire.append(buffer, static_cast<size_t>(count));
      }
      threads.JoinAll();
      EXPECT_EQ(response->getStatus().code, 200) << wire;
      if (!body.value("stream", false)) return nlohmann::json::parse(wire);
      nlohmann::json terminal;
      std::istringstream lines(wire);
      for (std::string line; std::getline(lines, line);) {
        if (!line.starts_with("data: ") || line == "data: [DONE]") continue;
        const auto event = nlohmann::json::parse(line.substr(6));
        EXPECT_NE(event.value("type", ""), "error") << event;
        const auto type = event.value("type", "");
        if (type == "response.completed" || type == "response.incomplete") terminal = event.at("response");
      }
      EXPECT_FALSE(terminal.is_null()) << wire;
      return terminal;
    }
    test::MockCatalog catalog;
    SessionManager sessions;
    ResponseStore store;
    StreamingThreadTracker threads;
    ServiceContext context;
  };
#endif
  static inline std::filesystem::path directory_;
  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> detector_;
  static inline std::unique_ptr<ModelLoadManager> loader_;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline test::FakeServiceBindings services_;
  TelemetryLogger telemetry_{"test", test::NullLog()};
};

}  // namespace

TEST_F(HarmonySessionTest, BudgetInterruptionKeepsLengthPrefixAndHealthyNextTurn) {
  for (const bool streaming : {false, true}) {
    for (const auto* body : {R"({"city":)", R"({"city":"Paris"})"}) {
      auto catalog = CatalogModel();
      auto attempts = std::make_shared<int>(0);
      ChatSession session(catalog, *model_, *logger_, telemetry_, {}, Factory(body, attempts));
      session.AddToolDefinition({"lookup", "", R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"]})", ToolKind::kFunction});
      std::string streamed;
      int calls = 0;
      if (streaming) session.SetStreamingCallback([&](flStreamingCallbackData event, void*) {
        auto* queue = reinterpret_cast<ItemQueue*>(event.item_queue);
        while (auto item = queue->TryPop()) {
          if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) ++calls;
          if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) streamed += static_cast<const TextItem&>(*item).text;
        }
        return 0;
      });
      auto request = UserRequest();
      Response response;
      session.ProcessRequest(request, response);
      EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_LENGTH);
      EXPECT_EQ(*attempts, 1);
      EXPECT_EQ(calls, 0);
      ASSERT_EQ(session.TurnCount(), 1u);
      EXPECT_EQ(session.Transcript().Messages().back().VisibleText(), "prefix");
      EXPECT_FALSE(session.Transcript().HasOutstandingCalls());
      if (streaming) {
        EXPECT_EQ(streamed, "prefix");
      }
      auto followup = UserRequest();
      Response healthy;
      session.ProcessRequest(followup, healthy);
      EXPECT_EQ(healthy.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
      EXPECT_EQ(session.Transcript().Messages().back().VisibleText(), "healthy");
    }
  }
}

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
TEST_F(HarmonySessionTest, ResponsesHandlerPublishesBudgetInterruptionAndContinuesWithoutRetry) {
  for (const bool streaming : {false, true}) {
    for (const auto* body : {R"({"city":)", R"({"city":"Paris"})"}) {
      HttpHarness http(telemetry_);
      auto attempts = std::make_shared<int>(0);
      http.Seed(Factory(body, attempts));
      nlohmann::json request = {
          {"model", "harmony-session:1"}, {"previous_response_id", "seed"}, {"input", "look up Paris"}, {"stream", streaming}, {"store", true}, {"tools", {{{"type", "function"}, {"name", "lookup"}, {"parameters", {{"type", "object"}, {"properties", {{"city", {{"type", "string"}}}}}, {"required", {"city"}}}}}}}};
      const auto interrupted = http.Post(request);
      ASSERT_EQ(interrupted.at("status"), "incomplete");
      EXPECT_EQ(interrupted.at("incomplete_details").at("reason"), "max_output_tokens");
      EXPECT_EQ(*attempts, 1);
      ASSERT_EQ(interrupted.at("output").size(), 1u);
      EXPECT_EQ(interrupted.at("output")[0].at("type"), "message");
      EXPECT_EQ(interrupted.at("output")[0].at("content")[0].at("text"), "prefix");
      request["previous_response_id"] = interrupted.at("id");
      request["input"] = "continue";
      const auto healthy = http.Post(request);
      EXPECT_EQ(healthy.at("status"), "completed");
      EXPECT_EQ(healthy.at("output")[0].at("content")[0].at("text"), "healthy");
      EXPECT_EQ(*attempts, 2);
    }
  }
}

TEST_F(HarmonySessionTest, ResponsesHandlerStoresCallerEncodingForColdReplayAfterDeclarationChanges) {
  const std::string body = "*** Begin Patch\n*** End Patch";
  for (const bool streaming : {false, true}) {
    for (const bool initially_raw : {false, true}) {
      HttpHarness http(telemetry_);
      http.Seed(Factory("", std::make_shared<int>(0)));
      const auto format = initially_raw
                              ? nlohmann::json{{"type", "grammar"}, {"syntax", "lark"}, {"definition", tools::kStockGhcpApplyPatchLarkGrammar}}
                              : nlohmann::json{{"type", "text"}};
      const auto published = http.Post({{"model", "harmony-session:1"}, {"previous_response_id", "seed"}, {"stream", streaming}, {"store", true}, {"tools", {{{"type", "custom"}, {"name", "apply_patch"}, {"format", format}}}}, {"input", {{{"type", "custom_tool_call"}, {"call_id", "caller_patch"}, {"name", "apply_patch"}, {"input", body}}, {{"type", "custom_tool_call_output"}, {"call_id", "caller_patch"}, {"output", "applied"}}, {{"role", "user"}, {"content", "continue"}}}}});
      ASSERT_EQ(published.at("status"), "completed");
      const auto id = published.at("id").get<std::string>();
      auto warm = http.sessions.CheckOut(id);
      ASSERT_NE(warm, nullptr);
      const auto chain = http.store.BuildChainContext(id);
      ASSERT_TRUE(chain);
      responses::ResponseCreateParams params;
      params.model = "harmony-session";
      params.input = "continue";
      auto replay_request = ResponseConverter::ToSessionRequest(params, &*chain);
      auto replay = IngestRequestItems(replay_request.items, replay_request.item_segment_starts);
      for (const bool current_raw : {false, true}) {
        ToolCallContext context;
        context.harmony = *model_->GetHarmonyProtocol();
        context.tool_kinds = {{"apply_patch", ToolKind::kCustom}};
        if (current_raw) {
          context.custom_lark_grammars = {{"apply_patch", std::string(tools::kStockGhcpApplyPatchLarkGrammar)}};
        }
        const auto project = [&](const auto& messages) {
          const auto projection = nlohmann::json::parse(
              BuildHarmonyPromptProjection(messages, context, [](std::string value) { return value; }).messages_json);
          auto history = nlohmann::json::array();
          for (const auto& message : projection) {
            if (message.contains("tool_calls") || message["role"] == "tool") history.push_back(message);
          }
          return history;
        };
        const auto warm_projection = project(warm->Transcript().Messages());
        const auto cold_projection = project(replay.messages);
        EXPECT_EQ(warm_projection, cold_projection);
        size_t calls = 0;
        const auto check_call = [&](const auto& projection) {
          for (const auto& message : projection) {
            if (!message.contains("tool_calls")) continue;
            for (const auto& call : message["tool_calls"]) {
              ++calls;
              EXPECT_EQ(call["function"]["native_freeform"], initially_raw);
              const auto expected = initially_raw ? body : nlohmann::json{{"input", body}}.dump();
              EXPECT_EQ(call["function"]["arguments_json"], expected);
            }
          }
        };
        check_call(warm_projection);
        check_call(cold_projection);
        EXPECT_EQ(calls, 2u);
      }
      EXPECT_EQ(http.sessions.CheckOut(id), nullptr);
    }
  }
}
#endif

TEST(HarmonyGrammarTest, ChoicesKeepAuthoritativeHeadersBoundariesAndAllowedRecipients) {
  for (const bool reasoning : {false, true}) {
    for (const std::string choice : {"auto", "required", "forced", "none"}) {
      SCOPED_TRACE(reasoning);
      SCOPED_TRACE(choice);
      ToolCallContext context;
      context.harmony = kProtocol;
      context.supports_reasoning = reasoning;
      context.text_output = choice == "auto" || choice == "none";
      context.tool_output = choice != "none";
      context.eos_token_ids = {99, kProtocol.handoff, 99};
      if (context.tool_output) {
        const auto forced = choice == "forced"
                                ? std::optional<ForcedToolChoice>{{"lookup", ToolKind::kFunction}}
                                : std::nullopt;
        context.tool_plan = ToolCallPlan::Create(kTools, true, forced);
      }
      const auto grammar = BuildLarkGrammar(context);
      const std::string mode = choice == "none"   ? "commentary* final"
                               : choice == "auto" ? "commentary* (final | tool)"
                                                  : "tool";
      EXPECT_TRUE(grammar.starts_with("start: " + std::string(reasoning ? "reasoning* " : "") + mode + "\n"));
      EXPECT_NE(grammar.find("plain_token: <[^10,11,12,13,14,15,16,99]>"), std::string::npos);
      EXPECT_NE(grammar.find("final: <[11]> \"final\" <[12]> plain_token* <[15]>"), std::string::npos);
      if (context.tool_output) {
        EXPECT_NE(grammar.find("metadata <[12]> arguments <[14]>"), std::string::npos);
        EXPECT_NE(grammar.find("\" to=\" \"functions.lookup\" <[11]> \"commentary\""), std::string::npos);
        EXPECT_NE(grammar.find("<[11]> \"commentary to=\" \"functions.lookup\""), std::string::npos);
        EXPECT_EQ(grammar.find("\"functions.patch\"") != std::string::npos, choice != "forced");
        EXPECT_NE(grammar.find("arguments:"), std::string::npos);
      } else {
        EXPECT_EQ(grammar.find("tool:"), std::string::npos);
        EXPECT_EQ(grammar.find("arguments:"), std::string::npos);
      }
    }
  }
}

TEST(HarmonyGrammarTest, NativePatchBodiesUseTheirGrammarAndUnsupportedFormatsFailExplicitly) {
  ToolCallContext context;
  context.harmony = kProtocol;
  context.tool_output = true;
  context.text_output = false;
  EXPECT_THROW(BuildLarkGrammar(context), fl::Exception);
  context.tool_plan = ToolCallPlan::Create(R"([{"type":"function","function":{
    "name":"apply_patch","parameters":{"type":"object","properties":{"input":{"type":"string"}}}}}])");
  context.tool_kinds = {{"apply_patch", ToolKind::kCustom}};
  context.custom_lark_grammars = {{"apply_patch", std::string(tools::kStockGhcpApplyPatchLarkGrammar)}};
  const auto grammar = BuildLarkGrammar(context);
  EXPECT_NE(grammar.find("tool_0: header_0 <[12]> native_patch <[14]>"), std::string::npos);
  EXPECT_NE(grammar.find("native_patch:"), std::string::npos);
  context.custom_lark_grammars["apply_patch"] = "start: /[\\s\\S]+/";
  EXPECT_THROW(BuildLarkGrammar(context), fl::Exception);
}

TEST(HarmonyDecoderTest, PromptProjectionRejectsNonLeadingInstructionRoles) {
  for (const auto* tools : {"", kTools}) {
    ToolCallContext context;
    context.harmony = kProtocol;
    context.tools_json = tools;
    for (const auto role : {FOUNDRY_LOCAL_ROLE_SYSTEM, FOUNDRY_LOCAL_ROLE_DEVELOPER}) {
      const auto protect = [](std::string value) { return value; };
      EXPECT_NO_THROW(BuildHarmonyPromptProjection(
          {{role, "Follow these instructions."}, {FOUNDRY_LOCAL_ROLE_USER, "Hello"}}, context, protect));
      try {
        BuildHarmonyPromptProjection(
            {{FOUNDRY_LOCAL_ROLE_USER, "Hello"}, {role, "Do not omit this instruction."}, {FOUNDRY_LOCAL_ROLE_USER, "Continue"}},
            context, protect);
        FAIL() << "Expected non-leading instructions to be rejected";
      } catch (const fl::Exception& error) {
        EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
      }
    }
  }
}

TEST(HarmonyDecoderTest, PromptProjectionRejectsHeaderSeparatorsInToolAndHistoryNames) {
  for (const char separator : std::string(" \t\r\n\v\f\x01\x02")) {
    const std::string name = "lookup" + std::string(1, separator) + "city";
    for (const bool history : {false, true}) {
      ToolCallContext context;
      context.harmony = kProtocol;
      context.tools_json = history ? kTools : nlohmann::json::array({{{"type", "function"}, {"function", {{"name", name}, {"parameters", {{"type", "object"}}}}}}}).dump();
      std::vector<TranscriptMessage> messages{{FOUNDRY_LOCAL_ROLE_USER, "Hello"}};
      if (history) {
        TranscriptMessage assistant(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
        assistant.AppendToolCall(MakeSuppliedToolCall("call_bad", name, "{}"));
        messages.push_back(std::move(assistant));
      }
      try {
        BuildHarmonyPromptProjection(messages, context, [](std::string value) { return value; });
        FAIL() << "Expected an unrepresentable Harmony recipient to be rejected";
      } catch (const fl::Exception& error) {
        EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
      }
    }
  }
}
