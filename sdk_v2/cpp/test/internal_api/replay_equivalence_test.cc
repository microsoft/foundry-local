// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Warm / cold replay equivalence.
//
// A Responses continuation is served two ways: from a live ChatSession that still holds the conversation in its
// transcript (warm), or by reconstructing the whole `previous_response_id` chain from the store after the session
// cache dropped it (cold). Both must hand the chat template the same messages, otherwise the model sees a different
// conversation depending on cache luck.
//
// The warm side is simulated by committing turns to a ChatTranscript exactly as ChatSession does: a turn's input
// messages followed by the single assistant message the generation produced. The cold side goes through the real
// store → ResponseStore::BuildChainContext → ResponseConverter::ToSessionRequest → BuildTranscriptMessages path.
// No model is involved on either side.

#include "contracts/responses.h"
#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/openresponses/response_converter.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "inferencing/session/request.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace fl;
using namespace fl::responses;
using json = nlohmann::json;

namespace {

/// One completed turn, described once and replayed both ways.
struct ReplayTurn {
  json input_items;                            // what the hop's request stored
  json output_items;                           // what the hop's response stored
  std::vector<TranscriptMessage> live_inputs;  // what a live session ingested for that turn
  TranscriptMessage live_output;               // the assistant message the live session committed
};

TranscriptMessage UserMessage(std::string text) {
  return TranscriptMessage(FOUNDRY_LOCAL_ROLE_USER, std::move(text));
}

json UserInputItem(const std::string& text) {
  return json::array({{{"type", "message"}, {"role", "user"}, {"content", text}}});
}

json OutputMessage(const std::string& text) {
  return {{"type", "message"},
          {"role", "assistant"},
          {"content", json::array({{{"type", "output_text"}, {"text", text}}})}};
}

json OutputFunctionCall(const std::string& call_id, const std::string& name, const std::string& arguments) {
  return {{"type", "function_call"}, {"call_id", call_id}, {"name", name}, {"arguments", arguments}};
}

json OutputReasoning(const std::string& text) {
  return {{"type", "reasoning"},
          {"id", "rs_1"},
          {"summary", json::array({{{"type", "summary_text"}, {"text", text}}})}};
}

TranscriptToolCall SuppliedCall(const std::string& call_id, const std::string& name, const std::string& arguments) {
  return MakeSuppliedToolCall(call_id, name, arguments);
}

/// The messages a live session would hand the template for the next turn: the request's system prefix, everything
/// the transcript committed, then that turn's own input. Mirrors ChatSession exactly — the prefix is request state
/// and is never committed, and each turn's reply merges with the same rule CommitTurn applies.
std::vector<TranscriptMessage> WarmMessages(const std::vector<ReplayTurn>& turns,
                                            const std::vector<TranscriptMessage>& next_inputs,
                                            const std::string& instructions = {}) {
  ChatTranscript transcript;
  for (const auto& turn : turns) {
    transcript.CommitTurn(turn.live_inputs, turn.live_output, {});
  }

  auto messages = transcript.Messages();
  messages.insert(messages.end(), next_inputs.begin(), next_inputs.end());
  return WithSystemPrompt(instructions, std::move(messages));
}

/// Store `turns` as a chain and hand back the reconstructed context plus the store that owns it.
ResponseChainContext StoredChainContext(const std::vector<ReplayTurn>& turns) {
  ResponseStore store;
  std::string previous_id;

  for (size_t i = 0; i < turns.size(); ++i) {
    const std::string id = "resp_" + std::to_string(i + 1);

    json response;
    response["id"] = id;
    response["previous_response_id"] = previous_id.empty() ? json(nullptr) : json(previous_id);
    response["output"] = turns[i].output_items;
    // Instructions are request-scoped; a stored hop never carries them into replay.
    response["instructions"] = "ignored — never replayed";

    store.Store(id, std::move(response), turns[i].input_items);
    previous_id = id;
  }

  auto context = store.BuildChainContext(previous_id);
  EXPECT_TRUE(context.has_value());
  return context.value_or(ResponseChainContext{});
}

/// The messages the cold path builds: store the chain, reconstruct it, convert it, ingest it with the replay
/// segments the converter recorded, and apply the request's own system prefix — exactly what ChatSession does.
std::vector<TranscriptMessage> ColdMessages(
    const std::vector<ReplayTurn>& turns, const ResponseCreateParams& next_params,
    const std::unordered_map<std::string, ToolKind>& tool_kinds = {}) {
  auto context = StoredChainContext(turns);
  auto request = ResponseConverter::ToSessionRequest(next_params, &context);

  auto ingest = IngestRequestItems(request.items, request.item_segment_starts, tool_kinds);
  const char* prefix = request.options.Find(kSystemPromptOption);
  return WithSystemPrompt(prefix != nullptr ? prefix : "", std::move(ingest.messages));
}

/// The error code a ChatSession would report for this turn's input, or nullopt when it is accepted. Mirrors the
/// validation ChatSession runs before it touches a generator.
std::optional<flErrorCode> InputRejection(const std::vector<TranscriptMessage>& messages) {
  try {
    ChatTranscript transcript;
    transcript.ValidateInputs(messages);
  } catch (const fl::Exception& ex) {
    return ex.code();
  }

  return std::nullopt;
}

/// Assert warm and cold agree, with the next turn being a plain user message.
void ExpectWarmAndColdAgree(const std::vector<ReplayTurn>& turns, const std::string& next_user_text,
                            const std::string& instructions = {}) {
  ResponseCreateParams params;
  params.model = "test-model";
  params.input = next_user_text;
  if (!instructions.empty()) {
    params.instructions = instructions;
  }

  const auto cold_messages = ColdMessages(turns, params);

  const std::string warm = BuildChatMessagesJson(WarmMessages(turns, {UserMessage(next_user_text)}, instructions));
  const std::string cold = BuildChatMessagesJson(cold_messages);

  EXPECT_EQ(cold, warm);

  // The warm side committed these turns to a real transcript, so it has already been validated. Whatever the cold
  // side rebuilds has to be acceptable too: a conversation a live session can hold must not become one a rebuilt
  // session refuses to continue.
  EXPECT_EQ(InputRejection(cold_messages), std::nullopt);
}

/// A text-only turn: user asks, model answers in text.
ReplayTurn TextOnlyTurn() {
  ReplayTurn turn;
  turn.input_items = UserInputItem("Hi");
  turn.output_items = json::array({OutputMessage("Hello there.")});
  turn.live_inputs = {UserMessage("Hi")};
  turn.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  turn.live_output.AppendText("Hello there.");
  return turn;
}

}  // namespace

TEST(ReplayEquivalenceTest, TextOnlyTurn) {
  ExpectWarmAndColdAgree({TextOnlyTurn()}, "And again?");
}

TEST(ReplayEquivalenceTest, MoreThanTwentyStoredHopsRemainColdReplayEquivalent) {
  std::vector<ReplayTurn> turns(ResponseStore::kDefaultCapacity + 5, TextOnlyTurn());
  for (size_t i = 0; i < turns.size(); ++i) {
    const std::string suffix = std::to_string(i + 1);
    turns[i].input_items = UserInputItem("Input " + suffix);
    turns[i].output_items = json::array({OutputMessage("Output " + suffix)});
    turns[i].live_inputs = {UserMessage("Input " + suffix)};
    turns[i].live_output = TranscriptMessage(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
    turns[i].live_output.AppendText("Output " + suffix);
  }

  ExpectWarmAndColdAgree(turns, "Continue after a cache miss.");
}

TEST(ReplayEquivalenceTest, ToolCallAndResultAcrossTheCompactionBoundaryRemainColdReplayEquivalent) {
  std::vector<ReplayTurn> turns(ResponseStore::kDefaultCapacity + 2, TextOnlyTurn());
  for (size_t i = 0; i < turns.size(); ++i) {
    const std::string suffix = std::to_string(i + 1);
    turns[i].input_items = UserInputItem("Input " + suffix);
    turns[i].output_items = json::array({OutputMessage("Output " + suffix)});
    turns[i].live_inputs = {UserMessage("Input " + suffix)};
    turns[i].live_output = TranscriptMessage(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Output " + suffix);
  }

  turns[1].output_items =
      json::array({OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})")});
  turns[1].live_output = TranscriptMessage(FOUNDRY_LOCAL_ROLE_ASSISTANT, "");
  turns[1].live_output.AppendToolCall(SuppliedCall("call_1", "get_weather", R"({"city":"Seattle"})"));

  turns[2].input_items =
      json::array({{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "sunny"}}});
  turns[2].live_inputs = {TranscriptMessage::ToolResult("call_1", "sunny")};

  ExpectWarmAndColdAgree(turns, "Continue after the compacted tool exchange.");
}

TEST(ReplayEquivalenceTest, TextThenCall) {
  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather in Seattle?");
  turn.output_items = json::array(
      {OutputMessage("Let me check."), OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})")});
  turn.live_inputs = {UserMessage("Weather in Seattle?")};
  turn.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  turn.live_output.AppendText("Let me check.");
  turn.live_output.AppendToolCall(SuppliedCall("call_1", "get_weather", R"({"city":"Seattle"})"));

  ExpectWarmAndColdAgree({turn}, "Thanks.");
}

TEST(ReplayEquivalenceTest, TextThenCallThenTextKeepsTheProducedEventOrderAndIsThenRejected) {
  // The record never reorders: replay rebuilds the turn with its events in the order the model produced them. The
  // chat-template schema cannot express that order, so the turn is rejected rather than projected as if the trailing
  // text had come first.
  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather in Seattle?");
  turn.output_items = json::array({OutputMessage("Let me check."),
                                   OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})"),
                                   OutputMessage(" One moment.")});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  auto messages = ColdMessages({turn}, params);

  ASSERT_EQ(messages.size(), 3u);
  const auto& assistant = messages[1];
  EXPECT_EQ(assistant.role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  ASSERT_EQ(assistant.entries.size(), 3u);
  EXPECT_EQ(assistant.entries[0].kind, TranscriptEntry::Kind::kText);
  EXPECT_EQ(assistant.entries[0].text, "Let me check.");
  EXPECT_EQ(assistant.entries[1].kind, TranscriptEntry::Kind::kToolCall);
  EXPECT_EQ(assistant.entries[1].tool_call.call_id, "call_1");
  EXPECT_EQ(assistant.entries[2].kind, TranscriptEntry::Kind::kText);
  EXPECT_EQ(assistant.entries[2].text, " One moment.");

  EXPECT_EQ(InputRejection(messages), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(ReplayEquivalenceTest, ATurnThatKeptTalkingAfterACallIsRejectedNotReordered) {
  // Warm and cold reject identically: a live session can never commit this shape (generation stops at the call),
  // and a caller replaying it is told so instead of having its text quietly moved in front of the call.
  TranscriptMessage live_output;
  live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  live_output.AppendText("Let me check.");
  live_output.AppendToolCall(SuppliedCall("call_1", "get_weather", R"({"city":"Seattle"})"));
  live_output.AppendText(" One moment.");

  ChatTranscript transcript;
  EXPECT_THROW(transcript.CommitTurn({UserMessage("Weather in Seattle?")}, live_output, {}), fl::Exception);
  EXPECT_TRUE(transcript.Empty());
  EXPECT_EQ(transcript.TurnCount(), 0u);

  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather in Seattle?");
  turn.output_items = json::array({OutputMessage("Let me check."),
                                   OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})"),
                                   OutputMessage(" One moment.")});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  EXPECT_EQ(InputRejection(ColdMessages({turn}, params)), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(ReplayEquivalenceTest, WhitespaceBetweenCallsIsNotTextAfterACall) {
  // Models separate consecutive call blocks with a newline. It must not end a parallel-call turn, but it is dropped
  // because retaining it as visible content would move it before both calls during projection.
  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather and time?");
  turn.output_items = json::array({OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})"),
                                   OutputMessage("\n"),
                                   OutputFunctionCall("call_2", "get_time", R"({})")});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  auto messages = ColdMessages({turn}, params);
  EXPECT_EQ(InputRejection(messages), std::nullopt);
  ASSERT_GE(messages.size(), 2u);
  EXPECT_TRUE(messages[1].VisibleText().empty());
  EXPECT_EQ(messages[1].ToolCalls().size(), 2u);
}

TEST(ReplayEquivalenceTest, CallThenTextIsRejectedTheSameWayAsTextCallText) {
  // No leading text at all: the call still closes the turn's visible output.
  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather in Seattle?");
  turn.output_items = json::array({OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})"),
                                   OutputMessage("Checking now.")});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  EXPECT_EQ(InputRejection(ColdMessages({turn}, params)), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

TEST(ReplayEquivalenceTest, TwoParallelCalls) {
  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather and time?");
  turn.output_items = json::array({OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})"),
                                   OutputFunctionCall("call_2", "get_time", R"({})")});
  turn.live_inputs = {UserMessage("Weather and time?")};
  turn.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  turn.live_output.AppendToolCall(SuppliedCall("call_1", "get_weather", R"({"city":"Seattle"})"));
  turn.live_output.AppendToolCall(SuppliedCall("call_2", "get_time", R"({})"));

  ExpectWarmAndColdAgree({turn}, "Anything else?");
}

TEST(ReplayEquivalenceTest, CallOnly) {
  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather in Seattle?");
  turn.output_items = json::array({OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})")});
  turn.live_inputs = {UserMessage("Weather in Seattle?")};
  turn.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  turn.live_output.AppendToolCall(SuppliedCall("call_1", "get_weather", R"({"city":"Seattle"})"));

  ExpectWarmAndColdAgree({turn}, "Thanks.");
}

TEST(ReplayEquivalenceTest, ReasoningOnlyTurnKeepsItsAssistantBoundary) {
  ReplayTurn turn;
  turn.input_items = UserInputItem("Think about it.");
  turn.output_items = json::array({OutputReasoning("private scratchpad")});
  turn.live_inputs = {UserMessage("Think about it.")};
  turn.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  turn.live_output.AppendReasoning("private scratchpad");

  ExpectWarmAndColdAgree({turn}, "Well?");

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Well?");
  auto cold = ColdMessages({turn}, params);

  // The boundary is there, and none of the private text came back with it.
  ASSERT_EQ(cold.size(), 3u);
  EXPECT_EQ(cold[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(cold[1].entries.empty());
  EXPECT_EQ(cold[1].ReasoningText(), "");
}

TEST(ReplayEquivalenceTest, EmptyAssistantOutputKeepsItsBoundary) {
  ReplayTurn turn;
  turn.input_items = UserInputItem("Say nothing.");
  turn.output_items = json::array();
  turn.live_inputs = {UserMessage("Say nothing.")};
  turn.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;

  ExpectWarmAndColdAgree({turn}, "Still there?");

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Still there?");
  auto cold = ColdMessages({turn}, params);

  ASSERT_EQ(cold.size(), 3u);
  EXPECT_EQ(cold[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(cold[1].entries.empty());
}

TEST(ReplayEquivalenceTest, ToolResultContinuation) {
  ReplayTurn first;
  first.input_items = UserInputItem("Weather in Seattle?");
  first.output_items = json::array({OutputMessage("Let me check."),
                                    OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})")});
  first.live_inputs = {UserMessage("Weather in Seattle?")};
  first.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  first.live_output.AppendText("Let me check.");
  first.live_output.AppendToolCall(SuppliedCall("call_1", "get_weather", R"({"city":"Seattle"})"));

  ReplayTurn second;
  second.input_items =
      json::array({{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "sunny"}}});
  second.output_items = json::array({OutputMessage("It is sunny.")});
  second.live_inputs = {TranscriptMessage::ToolResult("call_1", "sunny")};
  second.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  second.live_output.AppendText("It is sunny.");

  ExpectWarmAndColdAgree({first, second}, "And tomorrow?");
}

TEST(ReplayEquivalenceTest, CustomTextAndJsonLookingPayloadsRemainWarmColdEquivalentThroughToolResult) {
  for (const auto& payload : {std::string("print('hi')\n"), std::string(R"({"input":"text","z":1})")}) {
    ReplayTurn first;
    first.input_items = UserInputItem("Run it.");
    first.output_items =
        json::array({OutputFunctionCall("call_1", "run_python", payload)});
    first.live_inputs = {UserMessage("Run it.")};
    first.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
    first.live_output.AppendToolCall(
        MakeSuppliedToolCall("call_1", "run_python", payload, ToolKind::kCustom));

    ReplayTurn second;
    second.input_items =
        json::array({{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "done"}}});
    second.output_items = json::array({OutputMessage("Finished.")});
    second.live_inputs = {TranscriptMessage::ToolResult("call_1", "done")};
    second.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
    second.live_output.AppendText("Finished.");

    ResponseCreateParams params;
    params.model = "test-model";
    params.input = "Continue.";
    const std::unordered_map<std::string, ToolKind> kinds = {
        {"run_python", ToolKind::kCustom}};

    const auto warm = BuildChatMessagesJson(WarmMessages({first, second}, {UserMessage("Continue.")}));
    const auto cold = BuildChatMessagesJson(ColdMessages({first, second}, params, kinds));
    EXPECT_EQ(cold, warm) << payload;
  }
}

TEST(ReplayEquivalenceTest, ReasoningOnlyTurnBetweenTwoRealTurnsDoesNotCollapseTheConversation) {
  ReplayTurn middle;
  middle.input_items = UserInputItem("Think about it.");
  middle.output_items = json::array({OutputReasoning("private")});
  middle.live_inputs = {UserMessage("Think about it.")};
  middle.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  middle.live_output.AppendReasoning("private");

  ReplayTurn last = TextOnlyTurn();

  ExpectWarmAndColdAgree({TextOnlyTurn(), middle, last}, "Done?");
}

TEST(ReplayEquivalenceTest, EmptyOutputTurnDoesNotLeaveTwoUserTurnsAdjacent) {
  ReplayTurn empty_turn;
  empty_turn.input_items = UserInputItem("Say nothing.");
  empty_turn.output_items = json::array();
  empty_turn.live_inputs = {UserMessage("Say nothing.")};
  empty_turn.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Still there?");

  auto cold = ColdMessages({empty_turn}, params);

  ASSERT_EQ(cold.size(), 3u);
  EXPECT_EQ(cold[0].role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(cold[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(cold[2].role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(BuildChatMessagesJson(cold),
            R"([{"role":"user","content":"Say nothing."},)"
            R"({"role":"assistant","content":""},)"
            R"({"role":"user","content":"Still there?"}])");
}

// ========================================================================
// Typed stateless replay — a caller resending the conversation in `input`
// reaches the same messages as chain reconstruction.
// ========================================================================

TEST(ReplayEquivalenceTest, StatelessReasoningItemReplaysTheSameBoundaryAsAStoredChain) {
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "user", "content": [{"type": "input_text", "text": "Think about it."}]},
      {"type": "reasoning", "id": "rs_1", "summary": [{"type": "summary_text", "text": "private"}]},
      {"role": "user", "content": [{"type": "input_text", "text": "Well?"}]}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ResponseConverter::ToSessionRequest(params);
  auto stateless = BuildTranscriptMessages(request.items);

  ReplayTurn turn;
  turn.input_items = UserInputItem("Think about it.");
  turn.output_items = json::array({OutputReasoning("private")});

  ResponseCreateParams chained;
  chained.model = "test-model";
  chained.input = std::string("Well?");

  EXPECT_EQ(BuildChatMessagesJson(stateless), BuildChatMessagesJson(ColdMessages({turn}, chained)));
}

TEST(ReplayEquivalenceTest, StatelessTextCallTextRebuildsAndIsRejectedExactlyLikeTheStoredChain) {
  auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "user", "content": [{"type": "input_text", "text": "Weather in Seattle?"}]},
      {"role": "assistant", "content": [{"type": "output_text", "text": "Let me check."}]},
      {"type": "function_call", "call_id": "call_1", "name": "get_weather",
       "arguments": "{\"city\":\"Seattle\"}"},
      {"role": "assistant", "content": [{"type": "output_text", "text": " One moment."}]},
      {"role": "user", "content": [{"type": "input_text", "text": "Thanks."}]}
    ]
  })");

  auto params = body.get<ResponseCreateParams>();
  auto request = ResponseConverter::ToSessionRequest(params);
  auto stateless = BuildTranscriptMessages(request.items);

  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather in Seattle?");
  turn.output_items = json::array({OutputMessage("Let me check."),
                                   OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})"),
                                   OutputMessage(" One moment.")});

  ResponseCreateParams chained;
  chained.model = "test-model";
  chained.input = std::string("Thanks.");

  auto cold = ColdMessages({turn}, chained);

  // Same rebuilt messages, same verdict: the shape is refused on both paths rather than reordered on either.
  EXPECT_EQ(stateless.size(), cold.size());
  EXPECT_EQ(InputRejection(stateless), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(InputRejection(cold), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
}

// ========================================================================
// Instruction scoping across a replayed chain.
// ========================================================================

TEST(ReplayEquivalenceTest, InstructionsComeFromTheCurrentRequestOnlyAndCallerSystemMessagesSurvive) {
  ResponseStore store;

  json response;
  response["id"] = "resp_1";
  response["previous_response_id"] = nullptr;
  response["instructions"] = "Be terse.";
  response["output"] = json::array({OutputMessage("ok")});

  // Leading item is the synthesized instructions message; the second is a caller system message with the same text.
  store.Store("resp_1", response,
              json::array({{{"type", "message"}, {"role", "system"}, {"content", "Be terse."}},
                           {{"type", "message"}, {"role", "system"}, {"content", "Be terse."}},
                           {{"type", "message"}, {"role", "user"}, {"content", "hello"}}}));

  auto context = store.BuildChainContext("resp_1");
  ASSERT_TRUE(context.has_value());

  ResponseCreateParams params;
  params.model = "test-model";
  params.instructions = "Be terse.";
  params.input = std::string("again");

  auto request = ResponseConverter::ToSessionRequest(params, &(*context));
  auto messages = BuildTranscriptMessages(request.items);

  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"system","content":"Be terse."},)"
            R"({"role":"system","content":"Be terse."},)"
            R"({"role":"user","content":"hello"},)"
            R"({"role":"assistant","content":"ok"},)"
            R"({"role":"user","content":"again"}])");
}

// ========================================================================
// Assistant prefill — a caller supplies assistant content the model
// continues. One assistant turn, committed and replayed as one message.
// ========================================================================

TEST(ReplayEquivalenceTest, GeneratedReplyContinuesATrailingAssistantInputMessage) {
  ChatTranscript transcript;

  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendText("Sure, ");

  TranscriptMessage reply;
  reply.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  reply.AppendText("here it is.");

  transcript.CommitTurn({UserMessage("Finish this."), prefill}, reply, {});

  ASSERT_EQ(transcript.MessageCount(), 2u) << "the reply must not become a second adjacent assistant message";
  EXPECT_EQ(transcript.Messages()[1].VisibleText(), "Sure, here it is.");
  EXPECT_EQ(BuildChatMessagesJson(transcript.Messages()),
            R"([{"role":"user","content":"Finish this."},)"
            R"({"role":"assistant","content":"Sure, here it is."}])");
}

TEST(ReplayEquivalenceTest, AssistantPrefillReplaysAsOneMessage) {
  ReplayTurn turn;
  turn.input_items = json::array({{{"type", "message"}, {"role", "user"}, {"content", "Finish this."}},
                                  {{"type", "message"}, {"role", "assistant"}, {"content", "Sure, "}}});
  turn.output_items = json::array({OutputMessage("here it is.")});

  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendText("Sure, ");
  turn.live_inputs = {UserMessage("Finish this."), prefill};
  turn.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  turn.live_output.AppendText("here it is.");

  ExpectWarmAndColdAgree({turn}, "Thanks.");
}

TEST(ReplayEquivalenceTest, GeneratedReplyDoesNotMergeIntoAReplayedEarlierTurn) {
  // The reply may only continue an input message from this request's own segment. A chain whose last replayed
  // message is an earlier turn's assistant output must stay separate from what the model produces now.
  ReplayTurn turn = TextOnlyTurn();
  auto context = StoredChainContext({turn});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::vector<InputItem>{};  // no new input: the chain is the whole request

  auto request = ResponseConverter::ToSessionRequest(params, &context);
  auto ingest = IngestRequestItems(request.items, request.item_segment_starts);

  TranscriptMessage reply;
  reply.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  reply.AppendText("New answer.");

  ChatTranscript transcript;
  transcript.CommitTurn(ingest.messages, reply, {}, ingest.last_segment_start);

  EXPECT_EQ(BuildChatMessagesJson(transcript.Messages()),
            R"([{"role":"user","content":"Hi"},)"
            R"({"role":"assistant","content":"Hello there."},)"
            R"({"role":"assistant","content":"New answer."}])");
}

// ========================================================================
// Hop boundaries — two recorded turns never collapse into one message.
// ========================================================================

TEST(ReplayEquivalenceTest, AssistantLedNextHopStaysASeparateTurn) {
  ReplayTurn first;
  first.input_items = UserInputItem("a");
  first.output_items = json::array({OutputMessage("A")});
  first.live_inputs = {UserMessage("a")};
  first.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  first.live_output.AppendText("A");

  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendText("B-prefill");

  ReplayTurn second;
  second.input_items = json::array({{{"type", "message"}, {"role", "assistant"}, {"content", "B-prefill"}},
                                    {{"type", "message"}, {"role", "user"}, {"content", "b"}}});
  second.output_items = json::array({OutputMessage("B")});
  second.live_inputs = {prefill, UserMessage("b")};
  second.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  second.live_output.AppendText("B");

  ExpectWarmAndColdAgree({first, second}, "c");

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("c");

  EXPECT_EQ(BuildChatMessagesJson(ColdMessages({first, second}, params)),
            R"([{"role":"user","content":"a"},)"
            R"({"role":"assistant","content":"A"},)"
            R"({"role":"assistant","content":"B-prefill"},)"
            R"({"role":"user","content":"b"},)"
            R"({"role":"assistant","content":"B"},)"
            R"({"role":"user","content":"c"}])");
}

TEST(ReplayEquivalenceTest, AnEmptyHopKeepsItsOwnBoundary) {
  // A hop that stored no input items and produced no output — instructions-only, or a turn truncated before it said
  // anything. It still contributes exactly one assistant boundary, and the hop after it does not merge into it.
  ReplayTurn empty_hop;
  empty_hop.input_items = json::array();
  empty_hop.output_items = json::array();

  ReplayTurn next;
  next.input_items = UserInputItem("b");
  next.output_items = json::array({OutputMessage("B")});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("c");

  EXPECT_EQ(BuildChatMessagesJson(ColdMessages({empty_hop, next}, params)),
            R"([{"role":"assistant","content":""},)"
            R"({"role":"user","content":"b"},)"
            R"({"role":"assistant","content":"B"},)"
            R"({"role":"user","content":"c"}])");
}

TEST(ReplayEquivalenceTest, AnInstructionsOnlyMiddleHopKeepsTheTurnsAroundItApart) {
  // The middle hop carried nothing but `instructions`, which is request state and is never stored as an item. Its
  // boundary is all that remains of it, and it has to keep the assistant turn before it apart from the one after.
  ReplayTurn first;
  first.input_items = UserInputItem("a");
  first.output_items = json::array({OutputMessage("A")});

  ReplayTurn instructions_only;
  instructions_only.input_items = json::array();
  instructions_only.output_items = json::array({OutputMessage("A2")});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("c");

  EXPECT_EQ(BuildChatMessagesJson(ColdMessages({first, instructions_only}, params)),
            R"([{"role":"user","content":"a"},)"
            R"({"role":"assistant","content":"A"},)"
            R"({"role":"assistant","content":"A2"},)"
            R"({"role":"user","content":"c"}])");
}

TEST(ReplayEquivalenceTest, ADeveloperMessageMapsToSystemWarmAndCold) {
  // `developer` is the Responses API's name for `system`, and no package chat template knows the newer name. Both
  // paths must land on the same role or the same conversation would render differently depending on cache luck.
  ReplayTurn turn;
  turn.input_items = json::array({{{"type", "message"}, {"role", "developer"}, {"content", "Be terse."}},
                                  {{"type", "message"}, {"role", "user"}, {"content", "Hi"}}});
  turn.output_items = json::array({OutputMessage("Hello.")});

  ResponseCreateParams cold_params;
  cold_params.model = "test-model";
  cold_params.input = std::string("Again?");

  const std::string cold = BuildChatMessagesJson(ColdMessages({turn}, cold_params));

  auto stateless_body = nlohmann::json::parse(R"({
    "model": "test-model",
    "input": [
      {"role": "developer", "content": [{"type": "input_text", "text": "Be terse."}]},
      {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
      {"role": "assistant", "content": [{"type": "output_text", "text": "Hello."}]},
      {"role": "user", "content": [{"type": "input_text", "text": "Again?"}]}
    ]
  })");

  auto stateless_params = stateless_body.get<ResponseCreateParams>();
  auto stateless_request = ResponseConverter::ToSessionRequest(stateless_params);
  const std::string stateless = BuildChatMessagesJson(BuildTranscriptMessages(stateless_request.items));

  EXPECT_EQ(cold, stateless);
  EXPECT_EQ(cold,
            R"([{"role":"system","content":"Be terse."},)"
            R"({"role":"user","content":"Hi"},)"
            R"({"role":"assistant","content":"Hello."},)"
            R"({"role":"user","content":"Again?"}])");
}

TEST(ReplayEquivalenceTest, TwoAdjacentEmptyHopsStayTwoTurns) {
  ReplayTurn empty_hop;
  empty_hop.input_items = json::array();
  empty_hop.output_items = json::array();

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("c");

  EXPECT_EQ(BuildChatMessagesJson(ColdMessages({empty_hop, empty_hop}, params)),
            R"([{"role":"assistant","content":""},)"
            R"({"role":"assistant","content":""},)"
            R"({"role":"user","content":"c"}])");
}

TEST(ReplayEquivalenceTest, TextThenCallProjectsToExactlyThisJson) {
  // The representable shape, anchored absolutely: one assistant turn, its text in `content`, its call in
  // `tool_calls`, and the template sees the events in the order they happened.
  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather in Seattle?");
  turn.output_items = json::array(
      {OutputMessage("Let me check."), OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})")});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  EXPECT_EQ(BuildChatMessagesJson(ColdMessages({turn}, params)),
            R"([{"role":"user","content":"Weather in Seattle?"},)"
            R"({"role":"assistant","content":"Let me check.","tool_calls":[)"
            R"({"id":"call_1","type":"function",)"
            R"("function":{"name":"get_weather","arguments":{"city":"Seattle"}}}]},)"
            R"({"role":"user","content":"Thanks."}])");
}

TEST(ReplayEquivalenceTest, ParallelCallsProjectToExactlyThisJson) {
  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather and time?");
  turn.output_items = json::array({OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})"),
                                   OutputFunctionCall("call_2", "get_time", R"({})")});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  EXPECT_EQ(BuildChatMessagesJson(ColdMessages({turn}, params)),
            R"([{"role":"user","content":"Weather and time?"},)"
            R"({"role":"assistant","content":"","tool_calls":[)"
            R"({"id":"call_1","type":"function",)"
            R"("function":{"name":"get_weather","arguments":{"city":"Seattle"}}},)"
            R"({"id":"call_2","type":"function",)"
            R"("function":{"name":"get_time","arguments":{}}}]},)"
            R"({"role":"user","content":"Thanks."}])");
}

// ========================================================================
// Reasoning parity — never projected, on any message.
// ========================================================================

TEST(ReplayEquivalenceTest, ReasoningAlongsideACallIsNotReplayedWarmOrCold) {
  ReplayTurn turn;
  turn.input_items = UserInputItem("Weather in Seattle?");
  turn.output_items = json::array({OutputReasoning("private"),
                                   OutputMessage("Let me check."),
                                   OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})")});
  turn.live_inputs = {UserMessage("Weather in Seattle?")};
  turn.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  turn.live_output.AppendReasoning("private");
  turn.live_output.AppendText("Let me check.");
  turn.live_output.AppendToolCall(SuppliedCall("call_1", "get_weather", R"({"city":"Seattle"})"));

  // The live record still holds the reasoning; the prompt never shows it.
  EXPECT_EQ(turn.live_output.ReasoningText(), "private");
  ExpectWarmAndColdAgree({turn}, "Thanks.");

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");
  const std::string projected = BuildChatMessagesJson(ColdMessages({turn}, params));
  EXPECT_EQ(projected.find("private"), std::string::npos) << projected;
  EXPECT_EQ(projected.find("reasoning_content"), std::string::npos) << projected;
}

TEST(ReplayEquivalenceTest, ReasoningOnlyTurnFollowedByAToolExchangeStaysAligned) {
  ReplayTurn thinking;
  thinking.input_items = UserInputItem("Think first.");
  thinking.output_items = json::array({OutputReasoning("private")});
  thinking.live_inputs = {UserMessage("Think first.")};
  thinking.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  thinking.live_output.AppendReasoning("private");

  ReplayTurn calling;
  calling.input_items = UserInputItem("Now check the weather.");
  calling.output_items = json::array({OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})")});
  calling.live_inputs = {UserMessage("Now check the weather.")};
  calling.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  calling.live_output.AppendToolCall(SuppliedCall("call_1", "get_weather", R"({"city":"Seattle"})"));

  ExpectWarmAndColdAgree({thinking, calling}, "Thanks.");
}

// ========================================================================
// Instructions — request-scoped, never accumulated, never replayed.
// ========================================================================

TEST(ReplayEquivalenceTest, UnchangedInstructionsAppearExactlyOnceWarmAndCold) {
  ReplayTurn first = TextOnlyTurn();

  ReplayTurn second;
  second.input_items = UserInputItem("Again?");
  second.output_items = json::array({OutputMessage("Yes.")});
  second.live_inputs = {UserMessage("Again?")};
  second.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  second.live_output.AppendText("Yes.");

  ExpectWarmAndColdAgree({first, second}, "And now?", "Be terse.");

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("And now?");
  params.instructions = "Be terse.";

  auto cold = ColdMessages({first, second}, params);
  const auto system_count = std::count_if(cold.begin(), cold.end(), [](const TranscriptMessage& message) {
    return message.role == FOUNDRY_LOCAL_ROLE_SYSTEM;
  });

  EXPECT_EQ(system_count, 1) << "instructions must not stack up one copy per hop";
  EXPECT_EQ(cold.front().role, FOUNDRY_LOCAL_ROLE_SYSTEM);
  EXPECT_EQ(cold.front().VisibleText(), "Be terse.");
}

TEST(ReplayEquivalenceTest, ChangedInstructionsUseOnlyTheCurrentRequestsValue) {
  ReplayTurn turn = TextOnlyTurn();

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("And now?");
  params.instructions = "Answer in French.";

  auto cold = ColdMessages({turn}, params);

  ASSERT_FALSE(cold.empty());
  EXPECT_EQ(cold.front().role, FOUNDRY_LOCAL_ROLE_SYSTEM);
  EXPECT_EQ(cold.front().VisibleText(), "Answer in French.");
  EXPECT_EQ(BuildChatMessagesJson(cold),
            R"([{"role":"system","content":"Answer in French."},)"
            R"({"role":"user","content":"Hi"},)"
            R"({"role":"assistant","content":"Hello there."},)"
            R"({"role":"user","content":"And now?"}])");

  // Warm agrees: the prefix comes from request state, so a changed value simply replaces the old one.
  EXPECT_EQ(BuildChatMessagesJson(WarmMessages({turn}, {UserMessage("And now?")}, "Answer in French.")),
            BuildChatMessagesJson(cold));
}

TEST(ReplayEquivalenceTest, DroppedInstructionsLeaveNoSystemMessageBehind) {
  ReplayTurn turn = TextOnlyTurn();

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("And now?");
  // No instructions on this request, even though earlier hops had them.

  auto cold = ColdMessages({turn}, params);
  EXPECT_TRUE(std::none_of(cold.begin(), cold.end(), [](const TranscriptMessage& message) {
    return message.role == FOUNDRY_LOCAL_ROLE_SYSTEM;
  }));
}

TEST(ReplayEquivalenceTest, CallerSystemMessagesSurviveAlongsideInstructions) {
  ReplayTurn turn;
  turn.input_items = json::array({{{"type", "message"}, {"role", "system"}, {"content", "Be terse."}},
                                  {{"type", "message"}, {"role", "user"}, {"content", "Hi"}}});
  turn.output_items = json::array({OutputMessage("Hello.")});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Again?");
  params.instructions = "Be terse.";

  EXPECT_EQ(BuildChatMessagesJson(ColdMessages({turn}, params)),
            R"([{"role":"system","content":"Be terse."},)"
            R"({"role":"system","content":"Be terse."},)"
            R"({"role":"user","content":"Hi"},)"
            R"({"role":"assistant","content":"Hello."},)"
            R"({"role":"user","content":"Again?"}])");
}

// ========================================================================
// Tool correlation across hops.
// ========================================================================

TEST(ReplayEquivalenceTest, ParallelCallResultsMayComeBackOutOfOrder) {
  ReplayTurn calling;
  calling.input_items = UserInputItem("Weather and time?");
  calling.output_items = json::array({OutputFunctionCall("call_1", "get_weather", R"({"city":"Seattle"})"),
                                      OutputFunctionCall("call_2", "get_time", R"({})")});
  calling.live_inputs = {UserMessage("Weather and time?")};
  calling.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  calling.live_output.AppendToolCall(SuppliedCall("call_1", "get_weather", R"({"city":"Seattle"})"));
  calling.live_output.AppendToolCall(SuppliedCall("call_2", "get_time", R"({})"));

  ReplayTurn answering;
  answering.input_items = json::array({{{"type", "function_call_output"}, {"call_id", "call_2"}, {"output", "10am"}},
                                       {{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "sunny"}}});
  answering.output_items = json::array({OutputMessage("Sunny at 10am.")});
  answering.live_inputs = {TranscriptMessage::ToolResult("call_2", "10am"),
                           TranscriptMessage::ToolResult("call_1", "sunny")};
  answering.live_output.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  answering.live_output.AppendText("Sunny at 10am.");

  ExpectWarmAndColdAgree({calling, answering}, "Thanks.");

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Thanks.");

  auto cold = ColdMessages({calling, answering}, params);

  // The results correlate even though they answer in the opposite order, and every call is settled.
  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateInputs(cold));
  EXPECT_EQ(BuildChatMessagesJson(cold),
            R"([{"role":"user","content":"Weather and time?"},)"
            R"({"role":"assistant","content":"","tool_calls":[)"
            R"({"id":"call_1","type":"function","function":{"name":"get_weather","arguments":{"city":"Seattle"}}},)"
            R"({"id":"call_2","type":"function","function":{"name":"get_time","arguments":{}}}]},)"
            R"({"role":"tool","content":"10am","tool_call_id":"call_2"},)"
            R"({"role":"tool","content":"sunny","tool_call_id":"call_1"},)"
            R"({"role":"assistant","content":"Sunny at 10am."},)"
            R"({"role":"user","content":"Thanks."}])");
}

// ========================================================================
// Malformed stored hops must not throw or vanish.
// ========================================================================

TEST(ReplayEquivalenceTest, MalformedHopItemsAreSkippedWithoutLosingTheTurnBoundary) {
  ReplayTurn turn;
  turn.input_items = json::array({json::array({1, 2}),
                                  7,
                                  "text",
                                  json::object(),
                                  {{"type", "message"}, {"role", "user"}, {"content", "Hi"}}});
  turn.output_items = json::array({json::array({3}), 42, {{"type", "message"}, {"role", "assistant"}}});

  ResponseCreateParams params;
  params.model = "test-model";
  params.input = std::string("Again?");

  std::vector<TranscriptMessage> cold;
  ASSERT_NO_THROW(cold = ColdMessages({turn}, params));

  EXPECT_EQ(BuildChatMessagesJson(cold),
            R"([{"role":"user","content":"Hi"},)"
            R"({"role":"assistant","content":""},)"
            R"({"role":"user","content":"Again?"}])");
}

// ========================================================================
// Real request JSON round-trip: what the handler stores is what replays.
// ========================================================================

TEST(ReplayEquivalenceTest, StoredRequestJsonRoundTripsThroughReplay) {
  const auto body = nlohmann::json::parse(R"({
    "model": "test-model",
    "instructions": "Be terse.",
    "store": true,
    "input": [
      {"role": "system", "content": "Answer in French."},
      {"role": "user", "content": [{"type": "input_text", "text": "Weather in Seattle?"}]}
    ]
  })");

  // Exactly what the handler stores for that request.
  auto stored_items = ResponseConverter::ToInputItems(body);
  ASSERT_EQ(stored_items.size(), 2u) << "instructions are not stored; the caller's two items are";
  EXPECT_EQ(stored_items[0]["role"], "system");
  EXPECT_EQ(stored_items[1]["role"], "user");

  ResponseStore store;
  json response;
  response["id"] = "resp_1";
  response["previous_response_id"] = nullptr;
  response["instructions"] = "Be terse.";
  response["output"] = json::array({OutputMessage("Il fait beau.")});
  store.Store("resp_1", response, stored_items);

  // /input_items still reports only what the caller sent.
  auto reported = store.GetInputItems("resp_1");
  ASSERT_TRUE(reported.has_value());
  EXPECT_EQ(*reported, stored_items);

  auto context = store.BuildChainContext("resp_1");
  ASSERT_TRUE(context.has_value());

  const auto next_body = nlohmann::json::parse(R"({
    "model": "test-model",
    "instructions": "Be terse.",
    "previous_response_id": "resp_1",
    "input": "Et demain?"
  })");
  auto next_params = next_body.get<ResponseCreateParams>();

  auto request = ResponseConverter::ToSessionRequest(next_params, &(*context));
  auto ingest = IngestRequestItems(request.items, request.item_segment_starts);
  const char* prefix = request.options.Find(kSystemPromptOption);
  ASSERT_NE(prefix, nullptr);

  EXPECT_EQ(BuildChatMessagesJson(WithSystemPrompt(prefix, std::move(ingest.messages))),
            R"([{"role":"system","content":"Be terse."},)"
            R"({"role":"system","content":"Answer in French."},)"
            R"({"role":"user","content":"Weather in Seattle?"},)"
            R"({"role":"assistant","content":"Il fait beau."},)"
            R"({"role":"user","content":"Et demain?"}])");
}
