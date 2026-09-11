// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Model-free tests for the authoritative chat transcript: item ingestion, tool-call correlation, turn commit /
// undo semantics, and the JSON projection handed to the chat template.

#include "inferencing/generative/chat/chat_transcript.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/session/request.h"
#include "inferencing/session/tool_registry.h"
#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace fl;

namespace {

TranscriptToolCall MakeCall(std::string call_id, std::string name, std::string arguments) {
  return MakeSuppliedToolCall(std::move(call_id), std::move(name), std::move(arguments));
}

TranscriptMessage MakeAssistant(std::string text, std::vector<TranscriptToolCall> calls) {
  TranscriptMessage assistant;
  assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  assistant.AppendText(std::move(text));
  for (auto& call : calls) {
    assistant.AppendToolCall(std::move(call));
  }

  return assistant;
}

TranscriptMessage UserMessage(std::string text) {
  return {FOUNDRY_LOCAL_ROLE_USER, std::move(text)};
}

/// Commit a plain "user asks, assistant answers" turn.
void CommitTextTurn(ChatTranscript& transcript, const std::string& question, const std::string& answer) {
  transcript.CommitTurn({UserMessage(question)}, MakeAssistant(answer, {}), {});
}

}  // namespace

// ===========================================================================
// Template projection — BuildChatMessagesJson
// ===========================================================================

TEST(ChatTemplateProjectionTest, PlainConversationProjectsRoleAndContent) {
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are helpful."},
      {FOUNDRY_LOCAL_ROLE_USER, "Hello"},
      {FOUNDRY_LOCAL_ROLE_ASSISTANT, "Hi"}};

  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"system","content":"You are helpful."},)"
            R"({"role":"user","content":"Hello"},)"
            R"({"role":"assistant","content":"Hi"}])");
}

TEST(ChatTemplateProjectionTest, ToolExchangeProjectsCallIdsArgumentsAndResultIds) {
  std::vector<TranscriptMessage> messages = {
      {FOUNDRY_LOCAL_ROLE_SYSTEM, "You are helpful."},
      {FOUNDRY_LOCAL_ROLE_USER, "Weather in Seattle?"},
      MakeAssistant("Let me check.", {MakeCall("call_1", "get_weather", R"({"city":"Seattle"})")}),
      TranscriptMessage::ToolResult("call_1", "sunny"),
      {FOUNDRY_LOCAL_ROLE_ASSISTANT, "It is sunny."}};

  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"system","content":"You are helpful."},)"
            R"({"role":"user","content":"Weather in Seattle?"},)"
            R"({"role":"assistant","content":"Let me check.","tool_calls":[{"id":"call_1","type":"function",)"
            R"("function":{"name":"get_weather","arguments":{"city":"Seattle"}}}]},)"
            R"({"role":"tool","content":"sunny","tool_call_id":"call_1"},)"
            R"({"role":"assistant","content":"It is sunny."}])");
}

TEST(ChatTemplateProjectionTest, MultipleToolCallsKeepEmissionOrder) {
  std::vector<TranscriptMessage> messages = {
      MakeAssistant("", {MakeCall("call_1", "first", R"({"a":1})"), MakeCall("call_2", "second", R"({"b":2})")})};

  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"assistant","content":"","tool_calls":[)"
            R"({"id":"call_1","type":"function","function":{"name":"first","arguments":{"a":1}}},)"
            R"({"id":"call_2","type":"function","function":{"name":"second","arguments":{"b":2}}}]}])");
}

TEST(ChatTemplateProjectionTest, ReasoningIsNotProjectedEvenAlongsideToolCalls) {
  // Reasoning is the model's private scratchpad. A conversation rebuilt from storage cannot reproduce it, so
  // projecting it here would make a live session and a rebuilt one send different prompts. It stays on the
  // transcript and is still surfaced to the caller as a typed output item.
  TranscriptMessage assistant;
  assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  assistant.AppendReasoning("The user wants weather.");
  assistant.AppendText("Checking.");
  assistant.AppendToolCall(MakeCall("call_1", "get_weather", R"({"city":"Seattle"})"));

  EXPECT_EQ(assistant.ReasoningText(), "The user wants weather.") << "the record keeps it";
  EXPECT_EQ(BuildChatMessagesJson({assistant}),
            R"([{"role":"assistant","content":"Checking.",)"
            R"("tool_calls":[{"id":"call_1","type":"function",)"
            R"("function":{"name":"get_weather","arguments":{"city":"Seattle"}}}]}])");
}

TEST(ChatTemplateProjectionTest, ReasoningWithoutToolCallsIsNotReplayed) {
  TranscriptMessage assistant;
  assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  assistant.AppendReasoning("Thinking out loud.");
  assistant.AppendText("The answer is 4.");

  EXPECT_EQ(BuildChatMessagesJson({assistant}), R"([{"role":"assistant","content":"The answer is 4."}])");
}

TEST(ChatTemplateProjectionTest, EmptyArgumentsBecomeEmptyObject) {
  std::vector<TranscriptMessage> messages = {MakeAssistant("", {MakeCall("call_1", "now", "")})};

  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"assistant","content":"","tool_calls":)"
            R"([{"id":"call_1","type":"function","function":{"name":"now","arguments":{}}}]}])");
}

TEST(ChatTemplateProjectionTest, GeneratedUnusableArgumentsProjectAsAnEmptyObject) {
  // The model emitted bytes that are not a JSON object. Projection must still render — the turn was already streamed
  // to the caller — using the normalized form while the raw bytes stay on the transcript.
  auto generated = MakeGeneratedToolCall("call_1", "get_weather", R"({"city":)");
  ASSERT_FALSE(generated.arguments_usable);

  TranscriptMessage assistant;
  assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  assistant.AppendToolCall(generated.call);

  EXPECT_EQ(BuildChatMessagesJson({assistant}),
            R"([{"role":"assistant","content":"","tool_calls":)"
            R"([{"id":"call_1","type":"function","function":{"name":"get_weather","arguments":{}}}]}])");
  EXPECT_EQ(assistant.ToolCalls()[0]->arguments, R"({"city":)");
}

TEST(ChatTemplateProjectionTest, EmptyToolResultKeepsCallIdAndEmptyContent) {
  std::vector<TranscriptMessage> messages = {TranscriptMessage::ToolResult("call_1", "")};

  EXPECT_EQ(BuildChatMessagesJson(messages), R"([{"role":"tool","content":"","tool_call_id":"call_1"}])");
}

TEST(ChatTemplateProjectionTest, ParticipantNameIsProjected) {
  std::vector<TranscriptMessage> messages = {{FOUNDRY_LOCAL_ROLE_USER, "Hello", "alice"}};

  EXPECT_EQ(BuildChatMessagesJson(messages), R"([{"role":"user","content":"Hello","name":"alice"}])");
}

// ===========================================================================
// Item ingestion — BuildTranscriptMessages
// ===========================================================================

TEST(TranscriptIngestTest, ToolCallItemsFoldIntoAdjacentAssistantMessage) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather?"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Let me check."));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({"city":"Seattle"})"));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_2", "get_time", R"({})"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(messages[1].VisibleText(), "Let me check.");

  auto calls = messages[1].ToolCalls();
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0]->call_id, "call_1");
  EXPECT_EQ(calls[0]->name, "get_weather");
  EXPECT_EQ(calls[0]->arguments, R"({"city":"Seattle"})");
  EXPECT_EQ(calls[1]->call_id, "call_2");
  EXPECT_EQ(calls[1]->kind, ToolKind::kFunction);
}

TEST(TranscriptIngestTest, ToolCallItemWithoutAdjacentAssistantStartsNewMessage) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather?"));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({})"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(messages[1].VisibleText(), "");
  ASSERT_EQ(messages[1].ToolCalls().size(), 1u);
  EXPECT_EQ(messages[1].ToolCalls()[0]->call_id, "call_1");
}

TEST(TranscriptIngestTest, ToolResultKeepsCallIdAndEmptyResult) {
  Request request;
  request.AddOwnedItem(std::make_unique<ToolResultItem>("call_1", ""));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].role, FOUNDRY_LOCAL_ROLE_TOOL);
  EXPECT_EQ(messages[0].tool_call_id, "call_1");
  EXPECT_EQ(messages[0].VisibleText(), "");
}

TEST(TranscriptIngestTest, TypedTextPartsKeepVisibleAndReasoningApart) {
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("thinking", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  parts.push_back(std::make_unique<TextItem>("answer", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));

  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].ReasoningText(), "thinking");
  EXPECT_EQ(messages[0].VisibleText(), "answer");
}

TEST(TranscriptIngestTest, AssistantMessageAfterAToolCallContinuesTheSameTurn) {
  // A replayed `text -> call -> text` turn arrives as three items. The live session recorded it as one assistant
  // message, so ingestion has to rebuild one message with the events still in order.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather?"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Let me check."));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({"city":"Seattle"})"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, " One moment."));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 2u);
  const auto& assistant = messages[1];
  ASSERT_EQ(assistant.entries.size(), 3u);
  EXPECT_EQ(assistant.entries[0].kind, TranscriptEntry::Kind::kText);
  EXPECT_EQ(assistant.entries[0].text, "Let me check.");
  EXPECT_EQ(assistant.entries[1].kind, TranscriptEntry::Kind::kToolCall);
  EXPECT_EQ(assistant.entries[1].tool_call.call_id, "call_1");
  EXPECT_EQ(assistant.entries[2].kind, TranscriptEntry::Kind::kText);
  EXPECT_EQ(assistant.entries[2].text, " One moment.");
  EXPECT_EQ(assistant.VisibleText(), "Let me check. One moment.");

  // Ingestion rebuilds it faithfully; validation is what refuses it, because no chat template can say that the
  // trailing text came after the call.
  EXPECT_TRUE(assistant.HasVisibleTextAfterToolCall());
  ChatTranscript transcript;
  EXPECT_THROW(transcript.ValidateInputs(messages), fl::Exception);
}

TEST(TranscriptIngestTest, WhitespaceAfterAToolCallIsNotTextAfterACall) {
  // A newline between two call blocks makes no claim about order and must not reject the turn.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Weather and time?"));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({"city":"Seattle"})"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "\n"));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_2", "get_time", "{}"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_FALSE(messages[1].HasVisibleTextAfterToolCall());
  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateInputs(messages));
}

TEST(ChatTranscriptTest, TextAfterAToolCallIsRejectedWithInvalidArgument) {
  TranscriptMessage assistant;
  assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  assistant.AppendText("Let me check.");
  assistant.AppendToolCall(MakeSuppliedToolCall("call_1", "get_weather", "{}"));
  assistant.AppendText(" One moment.");

  try {
    ValidateRenderableTurn(assistant);
    FAIL() << "expected the ordering invariant to reject the turn";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("visible text after a tool call"), std::string::npos) << ex.what();
  }

  // Nothing about the turn is committed, and the call never becomes answerable.
  ChatTranscript transcript;
  EXPECT_THROW(transcript.CommitTurn({}, assistant, {}), fl::Exception);
  EXPECT_TRUE(transcript.Empty());
  EXPECT_FALSE(transcript.HasOutstandingCalls());
}

TEST(ChatTranscriptTest, AReplyThatWouldContinueAPrefilledCallIsRejected) {
  // Prefill and reply are each renderable on their own; merged they are not, so the check has to run on the merge.
  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendText("Let me check.");
  prefill.AppendToolCall(MakeSuppliedToolCall("call_1", "get_weather", "{}"));

  TranscriptMessage reply;
  reply.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  reply.AppendText(" One moment.");

  ChatTranscript transcript;
  EXPECT_THROW(transcript.CommitTurn({prefill}, reply, {}), fl::Exception);
  EXPECT_TRUE(transcript.Empty());
}

TEST(ChatTranscriptTest, CommitFailureBeforePublishPreservesEveryObservableState) {
  bool fail_before_publish = false;
  ChatTranscript transcript([&fail_before_publish](ChatTranscript::CommitPhase phase) {
    if (fail_before_publish && phase == ChatTranscript::CommitPhase::kBeforePublish) {
      throw std::runtime_error("injected commit failure");
    }
  });
  transcript.CommitTurn({UserMessage("Weather?")},
                        MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {3, 7});

  const auto messages_before = BuildChatMessagesJson(transcript.Messages());
  const auto turns_before = transcript.Turns();
  const auto outstanding_before = transcript.OutstandingCallCount();
  auto next_prompt_before = transcript.Messages();
  next_prompt_before.push_back(TranscriptMessage::ToolResult("call_1", "sunny"));
  const auto rendered_next_prompt_before = BuildChatMessagesJson(next_prompt_before);

  fail_before_publish = true;
  EXPECT_THROW(
      transcript.CommitTurn({TranscriptMessage::ToolResult("call_1", "sunny")},
                            MakeAssistant("It is sunny.", {MakeCall("call_2", "get_time", "{}")}), {7, 12}),
      std::runtime_error);

  EXPECT_EQ(BuildChatMessagesJson(transcript.Messages()), messages_before);
  ASSERT_EQ(transcript.Turns().size(), turns_before.size());
  EXPECT_EQ(transcript.Turns()[0].message_start, turns_before[0].message_start);
  EXPECT_EQ(transcript.Turns()[0].tokens.pre_turn, turns_before[0].tokens.pre_turn);
  EXPECT_EQ(transcript.Turns()[0].tokens.post_turn, turns_before[0].tokens.post_turn);
  EXPECT_EQ(transcript.OutstandingCallCount(), outstanding_before);
  EXPECT_TRUE(transcript.IsOutstanding("call_1"));
  EXPECT_FALSE(transcript.IsOutstanding("call_2"));

  auto next_prompt_after = transcript.Messages();
  next_prompt_after.push_back(TranscriptMessage::ToolResult("call_1", "sunny"));
  EXPECT_EQ(BuildChatMessagesJson(next_prompt_after), rendered_next_prompt_before);
}

// ===========================================================================
// AssistantTurnGuard — the generation-side half of the ordering invariant.
//
// ChatSession applies this while a turn is produced, so post-call text is never streamed to the caller and never
// reaches the transcript. These tests are the only model-free way to pin that behaviour.
// ===========================================================================

TEST(AssistantTurnGuardTest, TextBeforeAnyCallIsEmitted) {
  AssistantTurnGuard guard;

  EXPECT_EQ(guard.OfferVisibleText("Let me check."), TextDisposition::kEmit);
  EXPECT_FALSE(guard.TurnEnded());
}

TEST(AssistantTurnGuardTest, TextAfterACallEndsTheTurnOnce) {
  AssistantTurnGuard guard;

  EXPECT_EQ(guard.OfferVisibleText("Let me check."), TextDisposition::kEmit);
  guard.RecordToolCall();

  // The first post-call text is the one the caller is told about; everything after it is already accounted for.
  EXPECT_EQ(guard.OfferVisibleText(" One moment."), TextDisposition::kEndTurn);
  EXPECT_TRUE(guard.TurnEnded());
  EXPECT_EQ(guard.OfferVisibleText(" and more"), TextDisposition::kDropped);
  EXPECT_EQ(guard.OfferVisibleText("\n"), TextDisposition::kDropped);
  EXPECT_TRUE(guard.TurnEnded());
}

TEST(AssistantTurnGuardTest, WhitespaceBetweenCallsIsDroppedWithoutEndingTheTurn) {
  // Models separate consecutive call blocks with a newline. It is not visible content, and keeping it would move it
  // before the calls during template projection.
  AssistantTurnGuard guard;

  guard.RecordToolCall();
  EXPECT_EQ(guard.OfferVisibleText("\n"), TextDisposition::kDropped);
  EXPECT_EQ(guard.OfferVisibleText("  \t "), TextDisposition::kDropped);
  guard.RecordToolCall();
  EXPECT_FALSE(guard.TurnEnded());
}

TEST(AssistantTurnGuardTest, AnEmptyTextEventNeverEndsTheTurn) {
  AssistantTurnGuard guard;

  guard.RecordToolCall();
  EXPECT_EQ(guard.OfferVisibleText(""), TextDisposition::kDropped);
  EXPECT_FALSE(guard.TurnEnded());
}

TEST(AssistantTurnGuardTest, APrefilledCallClosesVisibleTextBeforeTheFirstToken) {
  // The reply merges into the prefill, so the two are one assistant turn and the prefill's call already closed it.
  AssistantTurnGuard guard(/*calls_already_issued=*/true);

  EXPECT_EQ(guard.OfferVisibleText("continuing"), TextDisposition::kEndTurn);
  EXPECT_TRUE(guard.TurnEnded());
}

TEST(AssistantTurnGuardTest, ParallelCallsWithNoTextAreUnaffected) {
  AssistantTurnGuard guard;

  guard.RecordToolCall();
  guard.RecordToolCall();
  EXPECT_FALSE(guard.TurnEnded());
}

TEST(AssistantTurnGuardTest, WhatTheGuardAllowsIsExactlyWhatTheTranscriptAccepts) {
  // The two halves of the invariant must agree: anything the guard lets through has to commit.
  AssistantTurnGuard guard;

  TranscriptMessage assistant;
  assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;

  const std::vector<std::string> stream{"Let me check.", "", "\n"};
  for (const auto& text : stream) {
    if (guard.OfferVisibleText(text) == TextDisposition::kEmit) {
      assistant.AppendText(text);
    }
  }

  guard.RecordToolCall();
  assistant.AppendToolCall(MakeSuppliedToolCall("call_1", "get_weather", "{}"));

  // The model keeps talking; the guard drops it, so the message stays renderable.
  if (guard.OfferVisibleText(" One moment.") == TextDisposition::kEmit) {
    assistant.AppendText(" One moment.");
  }

  EXPECT_TRUE(guard.TurnEnded());
  EXPECT_FALSE(assistant.HasVisibleTextAfterToolCall());
  EXPECT_NO_THROW(ValidateRenderableTurn(assistant));
  EXPECT_EQ(assistant.VisibleText(), "Let me check.\n");
}

TEST(ChatTranscriptTest, AnEmptyReplyAfterAPrefilledCallCommitsAsTheOneTurn) {
  // What the guard produces when a caller prefills an unanswered call and asks for more: the text is dropped, so the
  // reply is empty and merges away. The call stays outstanding, waiting for the result it actually needs.
  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendToolCall(MakeSuppliedToolCall("call_1", "get_weather", "{}"));

  TranscriptMessage empty_reply;
  empty_reply.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;

  ChatTranscript transcript;
  ASSERT_NO_THROW(transcript.CommitTurn({UserMessage("Weather?"), prefill}, empty_reply, {}));

  ASSERT_EQ(transcript.MessageCount(), 2u);
  EXPECT_EQ(transcript.Messages()[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(transcript.Messages()[1].ToolCalls().size(), 1u);
  EXPECT_TRUE(transcript.IsOutstanding("call_1"));
}

TEST(ChatTranscriptTest, AssistantPrefillForReplyReportsTheMessageTheReplyWouldContinue) {
  TranscriptMessage user(FOUNDRY_LOCAL_ROLE_USER, "Weather?");
  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendText("Let me check.");

  const std::vector<TranscriptMessage> inputs{user, prefill};

  EXPECT_EQ(AssistantPrefillForReply(inputs, 0), &inputs.back());
  // The floor is the boundary of the current replay segment: an earlier hop's assistant message is off limits.
  EXPECT_EQ(AssistantPrefillForReply(inputs, inputs.size()), nullptr);
  // A trailing user message is not a prefill.
  EXPECT_EQ(AssistantPrefillForReply({prefill, user}, 0), nullptr);
}

TEST(TranscriptIngestTest, ConsecutiveAssistantMessagesBecomeOneTurn) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "one"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, " two"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].VisibleText(), "one two");
}

TEST(TranscriptIngestTest, ADifferentParticipantNameStartsANewAssistantMessage) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "one", "alice"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "two", "bob"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].name, "alice");
  EXPECT_EQ(messages[1].name, "bob");
}

// ===========================================================================
// TurnCanGenerate — one decision for "does this turn have anything to say".
//
// ChatSession asks this once, so an empty `input` behaves the same whether the conversation is held in a warm
// session or was replayed into the request from storage, and a turn with nothing at all is a client error rather
// than a service failure.
// ===========================================================================

TEST(TurnCanGenerateTest, MessageContentIsEnough) {
  EXPECT_TRUE(TurnCanGenerate({UserMessage("Hi")}, {}));
}

TEST(TurnCanGenerateTest, MediaWithNoTextIsContent) {
  // The direct SDK regression: an image-only message has no text part, so it ingests as a message with no entries.
  // The bytes are the question and reach the generator directly, so the turn most certainly can generate.
  TranscriptMessage image_only;
  image_only.role = FOUNDRY_LOCAL_ROLE_USER;
  ASSERT_TRUE(image_only.entries.empty());

  EXPECT_FALSE(TurnCanGenerate({image_only}, {}));
  EXPECT_TRUE(TurnCanGenerate({image_only}, {.media = true}));
}

TEST(TurnCanGenerateTest, AudioOnlyIsContentForTheSameReason) {
  TranscriptMessage audio_only;
  audio_only.role = FOUNDRY_LOCAL_ROLE_USER;

  EXPECT_TRUE(TurnCanGenerate({audio_only}, {.media = true}));
}

TEST(TurnCanGenerateTest, InstructionsAloneMayGenerate) {
  // A new conversation whose whole request is `instructions`. The system prompt is content: the model can act on it.
  EXPECT_TRUE(TurnCanGenerate({}, {.system_prefix = true}));
}

TEST(TurnCanGenerateTest, AnEmptyInputContinuesAConversation) {
  // previous_response_id with an empty `input`, both ways round: warm sees history in the transcript, cold sees the
  // replayed chain in its inputs. Both must be answerable, or the same request would depend on cache luck.
  EXPECT_TRUE(TurnCanGenerate({}, {.history = true}));
  EXPECT_TRUE(TurnCanGenerate({UserMessage("a"), MakeAssistant("A", {})}, {}));
}

TEST(TurnCanGenerateTest, ReplayedAssistantBoundaryCountsAsConversationHistory) {
  TranscriptMessage boundary;
  boundary.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;

  EXPECT_TRUE(TurnCanGenerate({boundary}, {}));
}

TEST(TurnCanGenerateTest, ReasoningAloneIsNotRespondableContent) {
  TranscriptMessage reasoning;
  reasoning.role = FOUNDRY_LOCAL_ROLE_USER;
  reasoning.AppendReasoning("private");

  EXPECT_FALSE(CarriesRespondableContent({reasoning}));
}

TEST(TurnCanGenerateTest, NothingAtAllCannotGenerate) {
  EXPECT_FALSE(TurnCanGenerate({}, {}));
}

// ===========================================================================
// Consecutive assistant messages — intentional continuation semantics.
// ===========================================================================

TEST(TranscriptIngestTest, TwoAssistantMessagesJoinLiterallyWithNoSeparatorInserted) {
  // Deliberate: the fragments are two halves of one utterance, exactly as a live session records the text a model
  // emits in two chunks. Inserting a space here would put text in the conversation that nobody produced, and would
  // make a replayed turn differ from the turn it replays. The caller owns the spacing.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Let me"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, " check."));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].VisibleText(), "Let me check.");
  // One text entry, because the two fragments coalesced into the run they belong to.
  ASSERT_EQ(messages[0].entries.size(), 1u);
  EXPECT_EQ(messages[0].entries[0].kind, TranscriptEntry::Kind::kText);
}

TEST(TranscriptIngestTest, JoiningInsertsNothingEvenWhenTheFragmentsHaveNoSpacing) {
  // The exact concatenation, pinned: "one" + "two" is "onetwo", never "one two".
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "one"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "two"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].VisibleText(), "onetwo");
}

TEST(ChatTranscriptTest, AGeneratedReplyJoinsAnAssistantPrefillByTheSameLiteralRule) {
  // The commit side of the same decision: a caller prefills "The answer is" and the model continues " 42.". One
  // assistant turn, one message, no separator — which is also what replay rebuilds for that turn.
  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendText("The answer is");

  TranscriptMessage reply;
  reply.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  reply.AppendText(" 42.");

  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("What is it?"), prefill}, reply, {});

  ASSERT_EQ(transcript.MessageCount(), 2u);
  EXPECT_EQ(transcript.Messages()[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(transcript.Messages()[1].VisibleText(), "The answer is 42.");
  ASSERT_EQ(transcript.Messages()[1].entries.size(), 1u);
}

TEST(TranscriptIngestTest, AnUnnamedContinuationAdoptsTheOpenTurnsName) {
  // Participant-name merge semantics, stated explicitly: an unnamed assistant message continues the open turn and
  // the turn keeps the name it already had.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "one", "alice"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, " two"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].name, "alice");
  EXPECT_EQ(messages[0].VisibleText(), "one two");
}

TEST(TranscriptIngestTest, AssistantTurnsSeparatedByAnotherRoleStayApart) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "one"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "and?"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "two"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[0].VisibleText(), "one");
  EXPECT_EQ(messages[2].VisibleText(), "two");
}

TEST(TranscriptIngestTest, AnAssistantMessageWithEmptyTextIsAnEmptyTurnBoundary) {
  // How replay records a turn whose output was reasoning-only or truncated before any visible text.
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>(std::string{}));

  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Say nothing."));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Still there?"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_TRUE(messages[1].entries.empty());
}

TEST(TranscriptIngestTest, AnEmptyBoundaryMergesIntoTheAssistantOutputThatFollowsIt) {
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>(std::string{}));

  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts)));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Visible answer"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].VisibleText(), "Visible answer");
}

// ===========================================================================
// Replay segments — the merge rule stops at a recorded-turn boundary
// ===========================================================================

TEST(TranscriptIngestTest, AssistantItemsInDifferentSegmentsStayApart) {
  Request request;
  request.BeginItemSegment();
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "turn one"));
  request.BeginItemSegment();
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "turn two"));

  auto ingest = IngestRequestItems(request.items, request.item_segment_starts);

  ASSERT_EQ(ingest.messages.size(), 2u);
  EXPECT_EQ(ingest.messages[0].VisibleText(), "turn one");
  EXPECT_EQ(ingest.messages[1].VisibleText(), "turn two");
  EXPECT_EQ(ingest.last_segment_start, 1u);
}

TEST(TranscriptIngestTest, AssistantItemsInsideOneSegmentStillMerge) {
  Request request;
  request.BeginItemSegment();
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "one"));
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({})"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, " two"));

  auto ingest = IngestRequestItems(request.items, request.item_segment_starts);

  ASSERT_EQ(ingest.messages.size(), 1u);
  ASSERT_EQ(ingest.messages[0].entries.size(), 3u);
  EXPECT_EQ(ingest.messages[0].VisibleText(), "one two");
  EXPECT_EQ(ingest.messages[0].entries[1].kind, TranscriptEntry::Kind::kToolCall);
}

TEST(TranscriptIngestTest, AToolCallOpeningASegmentDoesNotFoldIntoTheSegmentBefore) {
  Request request;
  request.BeginItemSegment();
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "earlier turn"));
  request.BeginItemSegment();
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({})"));

  auto ingest = IngestRequestItems(request.items, request.item_segment_starts);

  ASSERT_EQ(ingest.messages.size(), 2u);
  EXPECT_EQ(ingest.messages[0].VisibleText(), "earlier turn");
  EXPECT_FALSE(ingest.messages[0].HasToolCalls());
  ASSERT_EQ(ingest.messages[1].ToolCalls().size(), 1u);
  EXPECT_EQ(ingest.messages[1].ToolCalls()[0]->call_id, "call_1");
}

TEST(TranscriptIngestTest, ASegmentThatContributesNothingStillMovesTheBoundary) {
  Request request;
  request.BeginItemSegment();
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "earlier turn"));
  request.BeginItemSegment();  // empty segment
  request.BeginItemSegment();
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "later turn"));

  auto ingest = IngestRequestItems(request.items, request.item_segment_starts);

  ASSERT_EQ(ingest.messages.size(), 2u);
  EXPECT_EQ(ingest.last_segment_start, 1u);
}

TEST(TranscriptIngestTest, ATrailingEmptySegmentReportsItselfAsTheLastSegment) {
  // The current request contributed no items at all: nothing generated now may merge into the replayed history.
  Request request;
  request.BeginItemSegment();
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "earlier turn"));
  request.BeginItemSegment();

  auto ingest = IngestRequestItems(request.items, request.item_segment_starts);

  ASSERT_EQ(ingest.messages.size(), 1u);
  EXPECT_EQ(ingest.last_segment_start, 1u);
}

TEST(TranscriptIngestTest, NoSegmentsMeansAdjacencyGrouping) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "one"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, " two"));

  auto ingest = IngestRequestItems(request.items, request.item_segment_starts);

  ASSERT_EQ(ingest.messages.size(), 1u);
  EXPECT_EQ(ingest.messages[0].VisibleText(), "one two");
  EXPECT_EQ(ingest.last_segment_start, 0u);
}

// ===========================================================================
// CommitTurn — the reply continues a trailing assistant input
// ===========================================================================

TEST(ChatTranscriptTest, ReplyMergesIntoATrailingAssistantInputMessage) {
  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendText("Sure, ");

  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Finish this."), prefill}, MakeAssistant("here it is.", {}), {});

  ASSERT_EQ(transcript.MessageCount(), 2u);
  EXPECT_EQ(transcript.Messages()[1].VisibleText(), "Sure, here it is.");
  EXPECT_EQ(transcript.TurnCount(), 1u);
}

TEST(ChatTranscriptTest, ReplyMergedIntoAPrefillKeepsItsToolCalls) {
  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendText("Checking. ");

  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?"), prefill},
                        MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});

  ASSERT_EQ(transcript.MessageCount(), 2u);
  EXPECT_TRUE(transcript.IsOutstanding("call_1"));
  ASSERT_EQ(transcript.Messages()[1].ToolCalls().size(), 1u);
  EXPECT_EQ(transcript.Messages()[1].VisibleText(), "Checking. ");
}

TEST(ChatTranscriptTest, ReplyDoesNotMergeAcrossTheReplyMergeFloor) {
  TranscriptMessage replayed;
  replayed.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  replayed.AppendText("earlier turn");

  ChatTranscript transcript;
  // The first input message is replayed history, so the reply must not continue it.
  transcript.CommitTurn({replayed}, MakeAssistant("new answer", {}), {}, /*reply_merge_floor=*/1);

  ASSERT_EQ(transcript.MessageCount(), 2u);
  EXPECT_EQ(transcript.Messages()[0].VisibleText(), "earlier turn");
  EXPECT_EQ(transcript.Messages()[1].VisibleText(), "new answer");
}

TEST(ChatTranscriptTest, UndoRemovesATurnWhoseReplyWasMergedIntoItsInput) {
  TranscriptMessage prefill;
  prefill.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  prefill.AppendText("Sure, ");

  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("First.")}, MakeAssistant("One.", {}), {});
  transcript.CommitTurn({UserMessage("Finish this."), prefill}, MakeAssistant("here it is.", {}), {});

  ASSERT_EQ(transcript.MessageCount(), 4u);
  transcript.UndoTurns(1);

  ASSERT_EQ(transcript.MessageCount(), 2u);
  EXPECT_EQ(transcript.Messages()[1].VisibleText(), "One.");
}

// ===========================================================================
// CarriesToolActivity / CarriesPriorTurnHistory
// ===========================================================================

TEST(TranscriptIngestTest, CarriesToolActivityDetectsCallsAndResults) {
  EXPECT_FALSE(CarriesToolActivity({}));
  EXPECT_FALSE(CarriesToolActivity({UserMessage("Hi")}));
  EXPECT_FALSE(CarriesToolActivity({MakeAssistant("plain answer", {})}));
  EXPECT_TRUE(CarriesToolActivity({MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")})}));
  EXPECT_TRUE(CarriesToolActivity({TranscriptMessage::ToolResult("call_1", "")}));
}

TEST(TranscriptIngestTest, CarriesPriorTurnHistoryDetectsAnyAssistantOrToolMessage) {
  EXPECT_FALSE(CarriesPriorTurnHistory({}));
  EXPECT_FALSE(CarriesPriorTurnHistory({UserMessage("Hi")}));
  EXPECT_FALSE(CarriesPriorTurnHistory({TranscriptMessage(FOUNDRY_LOCAL_ROLE_SYSTEM, "Be terse.")}));
  EXPECT_TRUE(CarriesPriorTurnHistory({MakeAssistant("plain answer", {})}));
  EXPECT_TRUE(CarriesPriorTurnHistory({TranscriptMessage::ToolResult("call_1", "")}));

  // An assistant boundary with no entries is still a prior turn.
  TranscriptMessage boundary;
  boundary.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  EXPECT_TRUE(CarriesPriorTurnHistory({boundary}));
}

// ===========================================================================
// WithSystemPrompt
// ===========================================================================

TEST(TranscriptIngestTest, WithSystemPromptPrependsExactlyOneSystemMessage) {
  auto messages = WithSystemPrompt("Be terse.", {UserMessage("Hi")});

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].role, FOUNDRY_LOCAL_ROLE_SYSTEM);
  EXPECT_EQ(messages[0].VisibleText(), "Be terse.");
  EXPECT_EQ(messages[1].VisibleText(), "Hi");
}

TEST(TranscriptIngestTest, WithSystemPromptIsANoOpWhenThereIsNoPrefix) {
  auto messages = WithSystemPrompt("", {UserMessage("Hi")});

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].VisibleText(), "Hi");
}

TEST(TranscriptIngestTest, ATopLevelImageItemIsRejectedRatherThanDropped) {
  // Media only reaches the model as a content part of a message. A bare top-level item would contribute nothing and
  // the model would be asked about an image it never saw.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "What is this?"));
  request.AddOwnedItem(std::make_unique<ImageItem>(std::vector<std::uint8_t>{1, 2, 3}, "image/png"));

  try {
    BuildTranscriptMessages(request.items);
    FAIL() << "expected a top-level image item to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("content parts of a message item"), std::string::npos) << ex.what();
  }
}

TEST(TranscriptIngestTest, ATopLevelAudioItemIsRejectedRatherThanDropped) {
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Transcribe this."));
  request.AddOwnedItem(std::make_unique<AudioItem>(std::vector<std::uint8_t>{1, 2, 3}, "wav"));

  EXPECT_THROW(BuildTranscriptMessages(request.items), fl::Exception);
}

TEST(TranscriptIngestTest, ATopLevelTextItemIsRejectedRatherThanDropped) {
  // The same silent-drop hole as a top-level media item: text only reaches the model inside a message.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Hello"));
  request.AddOwnedItem(std::make_unique<TextItem>("and this bit too"));

  try {
    BuildTranscriptMessages(request.items);
    FAIL() << "expected a top-level text item to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("message, tool call, and tool result"), std::string::npos) << ex.what();
  }
}

TEST(TranscriptIngestTest, MediaPartsInsideAMessageAreStillSkipped) {
  // The message itself is ingested for its text; the media bytes travel to the generator separately.
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("What is this?"));
  parts.push_back(std::make_unique<ImageItem>(std::vector<std::uint8_t>{1, 2, 3}, "image/png"));

  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, std::move(parts)));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].VisibleText(), "What is this?");
}

TEST(TranscriptIngestTest, CarriesRespondableContentDistinguishesABoundaryFromRealInput) {
  TranscriptMessage boundary;
  boundary.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;

  // A reasoning item rebuilds as exactly this: a turn happened, but there is nothing to respond to.
  EXPECT_FALSE(CarriesRespondableContent({boundary}));
  EXPECT_TRUE(CarriesRespondableContent({boundary, UserMessage("Hi")}));
  EXPECT_TRUE(CarriesRespondableContent({TranscriptMessage::ToolResult("call_1", "sunny")}));

  TranscriptMessage with_call;
  with_call.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  with_call.AppendToolCall(MakeSuppliedToolCall("call_1", "get_weather", "{}"));
  EXPECT_TRUE(CarriesRespondableContent({with_call}));
}

TEST(TranscriptIngestTest, AnUnnamedOpenTurnTakesTheNameOfItsContinuation) {
  // The mirror case: the open turn had no name, so the named continuation supplies one rather than starting a
  // second assistant message no template could render.
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, "one"));
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, " two", "alice"));

  auto messages = BuildTranscriptMessages(request.items);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].name, "alice");
  EXPECT_EQ(messages[0].VisibleText(), "one two");
}

// ===========================================================================
// Outstanding-call correlation
// ===========================================================================

TEST(ChatTranscriptTest, CommittedCallBecomesOutstanding) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});

  EXPECT_EQ(transcript.MessageCount(), 2u);
  EXPECT_EQ(transcript.TurnCount(), 1u);
  EXPECT_TRUE(transcript.IsOutstanding("call_1"));
  EXPECT_EQ(transcript.OutstandingCallCount(), 1u);
}

TEST(ChatTranscriptTest, ResultConsumesOutstandingCall) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});
  transcript.CommitTurn({TranscriptMessage::ToolResult("call_1", "sunny")}, MakeAssistant("It is sunny.", {}), {});

  EXPECT_FALSE(transcript.HasOutstandingCalls());
  EXPECT_EQ(transcript.MessageCount(), 4u);
}

TEST(ChatTranscriptTest, ResultsMayArriveOutOfOrder) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Both please")},
                        MakeAssistant("", {MakeCall("call_1", "first", "{}"), MakeCall("call_2", "second", "{}")}),
                        {});

  transcript.CommitTurn({TranscriptMessage::ToolResult("call_2", "second done"),
                         TranscriptMessage::ToolResult("call_1", "first done")},
                        MakeAssistant("Done.", {}), {});

  EXPECT_EQ(transcript.OutstandingCallCount(), 0u);
}

TEST(ChatTranscriptTest, EmptyResultStringIsAccepted) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});
  transcript.CommitTurn({TranscriptMessage::ToolResult("call_1", "")}, MakeAssistant("No data.", {}), {});

  EXPECT_FALSE(transcript.IsOutstanding("call_1"));
  EXPECT_EQ(transcript.Messages()[2].VisibleText(), "");
}

TEST(ChatTranscriptTest, ResultWithoutCallIdIsRejected) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});

  try {
    transcript.ValidateInputs({TranscriptMessage::ToolResult("", "sunny")});
    FAIL() << "expected a missing call id to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

TEST(ChatTranscriptTest, ResultForUnknownCallIsRejected) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});

  try {
    transcript.ValidateInputs({TranscriptMessage::ToolResult("call_missing", "sunny")});
    FAIL() << "expected an unknown call id to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

TEST(ChatTranscriptTest, SecondResultForTheSameCallIsRejected) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});
  transcript.CommitTurn({TranscriptMessage::ToolResult("call_1", "sunny")}, MakeAssistant("It is sunny.", {}), {});

  try {
    transcript.ValidateInputs({TranscriptMessage::ToolResult("call_1", "sunny again")});
    FAIL() << "expected an already answered call id to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

TEST(ChatTranscriptTest, TwoResultsForTheSameCallInOneBatchAreRejected) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});

  EXPECT_THROW(transcript.ValidateInputs({TranscriptMessage::ToolResult("call_1", "sunny"),
                                          TranscriptMessage::ToolResult("call_1", "sunny")}),
               fl::Exception);
}

TEST(ChatTranscriptTest, ReplayedCallIdMustBeNonEmptyAndUnique) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});

  EXPECT_THROW(transcript.ValidateInputs({MakeAssistant("", {MakeCall("", "get_weather", "{}")})}), fl::Exception);
  EXPECT_THROW(transcript.ValidateInputs({MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")})}),
               fl::Exception);
  EXPECT_THROW(transcript.ValidateInputs({MakeAssistant("", {MakeCall("call_9", "a", "{}"),
                                                             MakeCall("call_9", "b", "{}")})}),
               fl::Exception);
}

TEST(ChatTranscriptTest, ReplayedCallCanBeAnsweredWithinTheSameBatch) {
  ChatTranscript transcript;
  std::vector<TranscriptMessage> inputs = {UserMessage("Weather?"),
                                           MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}),
                                           TranscriptMessage::ToolResult("call_1", "sunny")};

  EXPECT_NO_THROW(transcript.ValidateInputs(inputs));

  transcript.CommitTurn(std::move(inputs), MakeAssistant("It is sunny.", {}), {});
  EXPECT_EQ(transcript.MessageCount(), 4u);
  EXPECT_FALSE(transcript.HasOutstandingCalls());
}

TEST(ChatTranscriptTest, ResultBeforeItsCallInTheSameBatchIsRejected) {
  ChatTranscript transcript;
  std::vector<TranscriptMessage> inputs = {TranscriptMessage::ToolResult("call_1", "sunny"),
                                           MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")})};

  EXPECT_THROW(transcript.ValidateInputs(inputs), fl::Exception);
}

// ===========================================================================
// Atomicity — a rejected turn must leave nothing behind
// ===========================================================================

TEST(ChatTranscriptTest, RejectedTurnLeavesTranscriptUnchanged) {
  ChatTranscript transcript;
  CommitTextTurn(transcript, "Hello", "Hi");

  EXPECT_THROW(transcript.CommitTurn({TranscriptMessage::ToolResult("call_missing", "sunny")},
                                     MakeAssistant("ignored", {}), {}),
               fl::Exception);

  EXPECT_EQ(transcript.MessageCount(), 2u);
  EXPECT_EQ(transcript.TurnCount(), 1u);
  EXPECT_FALSE(transcript.HasOutstandingCalls());
}

TEST(ChatTranscriptTest, FailedGenerationCommitsNoOutstandingCall) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});

  // A generation that re-uses an already issued call ID is rejected, and the rejection must not register the call.
  auto duplicate = MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")});
  EXPECT_THROW(transcript.ValidateGeneratedOutput(duplicate), fl::Exception);
  EXPECT_THROW(transcript.CommitTurn({UserMessage("Again?")}, duplicate, {}), fl::Exception);

  EXPECT_EQ(transcript.MessageCount(), 2u);
  EXPECT_EQ(transcript.TurnCount(), 1u);
  EXPECT_EQ(transcript.OutstandingCallCount(), 1u);
}

// Generation has already been streamed to the caller by the time a turn is committed, so unusable model arguments
// must never fail the request. Each shape the model can get wrong is checked end to end: the turn commits, the raw
// bytes survive, and the conversation still projects on every later turn.
class GeneratedArgumentsTest : public ::testing::TestWithParam<std::string> {};

TEST_P(GeneratedArgumentsTest, UnusableGeneratedArgumentsCommitAndStayRenderable) {
  const std::string& raw = GetParam();

  auto generated = MakeGeneratedToolCall("call_1", "get_weather", raw);
  EXPECT_FALSE(generated.arguments_usable) << "raw: " << raw;
  EXPECT_EQ(generated.call.arguments, raw);
  EXPECT_TRUE(generated.call.normalized_arguments.is_object());
  EXPECT_TRUE(generated.call.normalized_arguments.empty());

  TranscriptMessage assistant;
  assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
  assistant.AppendToolCall(std::move(generated.call));

  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateGeneratedOutput(assistant));
  EXPECT_NO_THROW(transcript.CommitTurn({UserMessage("Weather?")}, assistant, {}));

  EXPECT_EQ(transcript.MessageCount(), 2u);
  EXPECT_TRUE(transcript.IsOutstanding("call_1"));

  // Raw bytes are what the authoritative record reports.
  ASSERT_EQ(transcript.Messages()[1].ToolCalls().size(), 1u);
  EXPECT_EQ(transcript.Messages()[1].ToolCalls()[0]->arguments, raw);

  // The next turn rebuilds from this transcript, so projection must succeed.
  EXPECT_NO_THROW(BuildChatMessagesJson(transcript.Messages()));

  // And answering the call still works, so the conversation can continue.
  EXPECT_NO_THROW(transcript.ValidateInputs({TranscriptMessage::ToolResult("call_1", "sunny")}));
}

INSTANTIATE_TEST_SUITE_P(UnusableShapes, GeneratedArgumentsTest,
                         ::testing::Values(std::string("null"),
                                           std::string(R"("Seattle")"),
                                           std::string("[1,2]"),
                                           std::string("42"),
                                           std::string(R"({"city":)")));

TEST(ChatTranscriptTest, UsableGeneratedArgumentsKeepRawAndNormalizedInSync) {
  auto generated = MakeGeneratedToolCall("call_1", "get_weather", R"({"city":"Seattle"})");

  EXPECT_TRUE(generated.arguments_usable);
  EXPECT_EQ(generated.call.arguments, R"({"city":"Seattle"})");
  EXPECT_EQ(generated.call.normalized_arguments.dump(), R"({"city":"Seattle"})");
}

TEST(ChatTranscriptTest, GeneratedCallWithoutArgumentsNormalizesToAnEmptyObject) {
  auto generated = MakeGeneratedToolCall("call_1", "now", "");

  EXPECT_TRUE(generated.arguments_usable);
  EXPECT_EQ(generated.call.arguments, "");
  EXPECT_TRUE(generated.call.normalized_arguments.is_object());
}

TEST(ChatTranscriptTest, SuppliedCallWithUnusableArgumentsIsRejected) {
  // Caller-supplied replay stays strict: a client sending arguments that are not a JSON object gets an explicit
  // error instead of a silently altered conversation.
  for (const std::string& raw : {std::string("null"), std::string(R"("Seattle")"), std::string("[1,2]"),
                                 std::string(R"({"city":)")}) {
    try {
      MakeSuppliedToolCall("call_1", "get_weather", raw);
      FAIL() << "expected supplied arguments to be rejected: " << raw;
    } catch (const fl::Exception& ex) {
      EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) << "raw: " << raw;
    }
  }
}

TEST(TranscriptIngestTest, SuppliedToolCallItemWithUnusableArgumentsIsRejected) {
  Request request;
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", "[1,2]"));

  EXPECT_THROW(BuildTranscriptMessages(request.items), fl::Exception);
}

TEST(ChatTranscriptTest, CommittedTranscriptAlwaysProjects) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "")}), {});
  transcript.CommitTurn({TranscriptMessage::ToolResult("call_1", "sunny")}, MakeAssistant("It is sunny.", {}), {});

  EXPECT_NO_THROW(BuildChatMessagesJson(transcript.Messages()));
}

// ===========================================================================
// Undo
// ===========================================================================

TEST(ChatTranscriptTest, UndoRemovesTurnAndItsOutstandingCalls) {
  ChatTranscript transcript;
  CommitTextTurn(transcript, "Hello", "Hi");
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}),
                        {10, 24});

  auto tokens = transcript.UndoTurns(1);

  ASSERT_TRUE(tokens.pre_turn.has_value());
  EXPECT_EQ(*tokens.pre_turn, 10);
  EXPECT_EQ(tokens.post_turn, 24);
  EXPECT_EQ(transcript.MessageCount(), 2u);
  EXPECT_EQ(transcript.TurnCount(), 1u);
  EXPECT_FALSE(transcript.HasOutstandingCalls());
}

TEST(ChatTranscriptTest, UndoneCallIdIsNoLongerAnswerable) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});
  transcript.UndoTurns(1);

  EXPECT_THROW(transcript.ValidateInputs({TranscriptMessage::ToolResult("call_1", "sunny")}), fl::Exception);
}

TEST(ChatTranscriptTest, UndoRestoresAConsumedCallToOutstanding) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});
  transcript.CommitTurn({TranscriptMessage::ToolResult("call_1", "sunny")}, MakeAssistant("It is sunny.", {}), {});
  ASSERT_FALSE(transcript.HasOutstandingCalls());

  transcript.UndoTurns(1);

  EXPECT_TRUE(transcript.IsOutstanding("call_1"));
  EXPECT_EQ(transcript.MessageCount(), 2u);
}

TEST(ChatTranscriptTest, UndoOfARebuiltTurnReportsNoRewindPoint) {
  ChatTranscript transcript;
  CommitTextTurn(transcript, "Hello", "Hi");
  // A turn that rebuilt its generator has no pre-turn boundary — its input is part of the generator's prompt.
  transcript.CommitTurn({UserMessage("Weather?")}, MakeAssistant("Sunny.", {}), {std::nullopt, 24});

  auto tokens = transcript.UndoTurns(1);

  EXPECT_FALSE(tokens.pre_turn.has_value());
  EXPECT_EQ(transcript.TurnCount(), 1u);
}

TEST(ChatTranscriptTest, UndoAcrossARebuiltTurnReportsNoRewindPoint) {
  ChatTranscript transcript;
  // Turn 1 appended to an existing generator, turn 2 rebuilt it, turn 3 appended to the rebuilt generator.
  transcript.CommitTurn({UserMessage("one")}, MakeAssistant("1", {}), {5, 10});
  transcript.CommitTurn({UserMessage("two")}, MakeAssistant("2", {}), {std::nullopt, 30});
  transcript.CommitTurn({UserMessage("three")}, MakeAssistant("3", {}), {30, 40});

  // Undoing only the last turn lands on a boundary that belongs to the current generator.
  auto one_turn = transcript.UndoTurns(1);
  ASSERT_TRUE(one_turn.pre_turn.has_value());
  EXPECT_EQ(*one_turn.pre_turn, 30);
  EXPECT_EQ(transcript.TurnCount(), 2u);

  // Undoing back past the rebuild must not hand back turn 1's offset: the rebuild reset the generator's token scale,
  // so rewinding there would splice the KV cache at a stale position.
  transcript.CommitTurn({UserMessage("three again")}, MakeAssistant("3", {}), {30, 40});
  auto across_rebuild = transcript.UndoTurns(2);
  EXPECT_FALSE(across_rebuild.pre_turn.has_value());
  EXPECT_EQ(transcript.TurnCount(), 1u);
  EXPECT_EQ(transcript.MessageCount(), 2u);
}

TEST(ChatTranscriptTest, UndoOfAppendedTurnsOnlyKeepsTheEarliestBoundary) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("one")}, MakeAssistant("1", {}), {5, 10});
  transcript.CommitTurn({UserMessage("two")}, MakeAssistant("2", {}), {10, 20});
  transcript.CommitTurn({UserMessage("three")}, MakeAssistant("3", {}), {20, 30});

  auto tokens = transcript.UndoTurns(2);

  ASSERT_TRUE(tokens.pre_turn.has_value());
  EXPECT_EQ(*tokens.pre_turn, 10);
  EXPECT_EQ(transcript.TurnCount(), 1u);
}

TEST(ChatTranscriptTest, UndoingMoreTurnsThanExistIsRejected) {
  ChatTranscript transcript;
  CommitTextTurn(transcript, "Hello", "Hi");

  try {
    transcript.UndoTurns(2);
    FAIL() << "expected undo beyond the turn count to be rejected";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  }

  EXPECT_EQ(transcript.TurnCount(), 1u);
}

TEST(ChatTranscriptTest, UndoFailureBeforePublishPreservesMessagesTurnsCallsAndPrompt) {
  bool fail_undo_before_publish = false;
  ChatTranscript transcript([&fail_undo_before_publish](ChatTranscript::CommitPhase phase) {
    if (fail_undo_before_publish && phase == ChatTranscript::CommitPhase::kUndoBeforePublish) {
      throw std::runtime_error("injected undo failure");
    }
  });
  transcript.CommitTurn({UserMessage("Weather?")},
                        MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {3, 7});
  transcript.CommitTurn({TranscriptMessage::ToolResult("call_1", "sunny")},
                        MakeAssistant("", {MakeCall("call_2", "get_time", "{}")}), {7, 12});

  const auto messages_before = BuildChatMessagesJson(transcript.Messages());
  const auto turns_before = transcript.Turns();
  ASSERT_FALSE(transcript.IsOutstanding("call_1"));
  ASSERT_TRUE(transcript.IsOutstanding("call_2"));
  EXPECT_NO_THROW(transcript.ValidateInputs({TranscriptMessage::ToolResult("call_2", "noon")}));

  fail_undo_before_publish = true;
  EXPECT_THROW(transcript.UndoTurns(1), std::runtime_error);

  EXPECT_EQ(BuildChatMessagesJson(transcript.Messages()), messages_before);
  ASSERT_EQ(transcript.Turns().size(), turns_before.size());
  for (size_t i = 0; i < turns_before.size(); ++i) {
    EXPECT_EQ(transcript.Turns()[i].message_start, turns_before[i].message_start);
    EXPECT_EQ(transcript.Turns()[i].tokens.pre_turn, turns_before[i].tokens.pre_turn);
    EXPECT_EQ(transcript.Turns()[i].tokens.post_turn, turns_before[i].tokens.post_turn);
  }
  EXPECT_FALSE(transcript.IsOutstanding("call_1"));
  EXPECT_TRUE(transcript.IsOutstanding("call_2"));
  EXPECT_NO_THROW(transcript.ValidateInputs({TranscriptMessage::ToolResult("call_2", "noon")}));
  EXPECT_THROW(
      transcript.ValidateGeneratedOutput(
          MakeAssistant("", {MakeCall("call_1", "duplicate", "{}")})),
      fl::Exception);
  EXPECT_THROW(
      transcript.ValidateGeneratedOutput(
          MakeAssistant("", {MakeCall("call_2", "duplicate", "{}")})),
      fl::Exception);
}

// ===========================================================================
// Value semantics and full-history continuation
// ===========================================================================

TEST(ChatTranscriptTest, CopyAndMoveKeepIndependentState) {
  ChatTranscript original;
  original.CommitTurn({UserMessage("Weather?")}, MakeAssistant("", {MakeCall("call_1", "get_weather", "{}")}), {});

  ChatTranscript copy = original;
  copy.CommitTurn({TranscriptMessage::ToolResult("call_1", "sunny")}, MakeAssistant("It is sunny.", {}), {});

  EXPECT_EQ(copy.MessageCount(), 4u);
  EXPECT_FALSE(copy.HasOutstandingCalls());

  // The copy's extra turn must not have touched the original's messages or call state.
  EXPECT_EQ(original.MessageCount(), 2u);
  EXPECT_TRUE(original.IsOutstanding("call_1"));

  ChatTranscript moved = std::move(original);
  EXPECT_EQ(moved.MessageCount(), 2u);
  EXPECT_TRUE(moved.IsOutstanding("call_1"));
  EXPECT_NO_THROW(moved.ValidateInputs({TranscriptMessage::ToolResult("call_1", "sunny")}));
}

TEST(ChatTranscriptTest, FullHistoryContinuationProjectsTheWholeConversation) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Weather in Seattle?")},
                        MakeAssistant("", {MakeCall("call_1", "get_weather", R"({"city":"Seattle"})")}), {});
  transcript.CommitTurn({TranscriptMessage::ToolResult("call_1", "sunny")}, MakeAssistant("It is sunny.", {}), {});

  // What a rebuilt generator would see: the committed transcript plus the new turn's input.
  std::vector<TranscriptMessage> all_messages = transcript.Messages();
  all_messages.push_back(UserMessage("And tomorrow?"));

  EXPECT_EQ(BuildChatMessagesJson(all_messages),
            R"([{"role":"user","content":"Weather in Seattle?"},)"
            R"({"role":"assistant","content":"","tool_calls":[{"id":"call_1","type":"function",)"
            R"("function":{"name":"get_weather","arguments":{"city":"Seattle"}}}]},)"
            R"({"role":"tool","content":"sunny","tool_call_id":"call_1"},)"
            R"({"role":"assistant","content":"It is sunny."},)"
            R"({"role":"user","content":"And tomorrow?"}])");
}

// ===========================================================================
// Custom tools — kind-aware normalization
//
// A custom tool's payload is free-form text. It crosses the public API boundary already unwrapped from the
// synthesized `{"input": ...}` shape the model is prompted with, so the transcript stores that text as the raw
// arguments and rewraps it for template projection. That keeps the record, the response, and every later prompt in
// agreement, and makes a replayed call normalize exactly the way the generated one did.
// ===========================================================================

TEST(TranscriptCustomToolTest, GeneratedCustomCallKeepsRawTextAndWrapsForProjection) {
  auto generated = MakeGeneratedToolCall("call_1", "run_python", "print('hi')\n", ToolKind::kCustom);

  EXPECT_TRUE(generated.arguments_usable);
  EXPECT_EQ(generated.call.kind, ToolKind::kCustom);
  EXPECT_EQ(generated.call.arguments, "print('hi')\n");
  EXPECT_EQ(generated.call.normalized_arguments, nlohmann::ordered_json({{"input", "print('hi')\n"}}));
}

TEST(TranscriptCustomToolTest, CustomPayloadIsNeverAModelDefect) {
  // Text that is not JSON at all, and text that is JSON but not an object, are both valid custom payloads.
  for (const auto& payload : {std::string("not json {{"), std::string("\"a bare string\""), std::string("42")}) {
    auto generated = MakeGeneratedToolCall("call_1", "run_python", payload, ToolKind::kCustom);

    EXPECT_TRUE(generated.arguments_usable) << payload;
    EXPECT_EQ(generated.call.arguments, payload);
    EXPECT_EQ(generated.call.normalized_arguments, nlohmann::ordered_json({{"input", payload}}));
  }
}

TEST(TranscriptCustomToolTest, SuppliedCustomCallAcceptsTextThatIsNotAJsonObject) {
  // The regression this guards: a caller replaying the text payload the session handed it must not be rejected the
  // way a malformed function call is.
  auto call = MakeSuppliedToolCall("call_1", "run_python", "print('hi')", ToolKind::kCustom);

  EXPECT_EQ(call.kind, ToolKind::kCustom);
  EXPECT_EQ(call.arguments, "print('hi')");
  EXPECT_EQ(call.normalized_arguments, nlohmann::ordered_json({{"input", "print('hi')"}}));
}

TEST(TranscriptCustomToolTest, SuppliedAndGeneratedCustomCallsRejectEmbeddedNul) {
  const std::string payload{"before\0after", 12};

  try {
    (void)MakeSuppliedToolCall("call_1", "run_python", payload, ToolKind::kCustom);
    FAIL() << "expected supplied custom payload rejection";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }

  try {
    (void)MakeGeneratedToolCall("call_1", "run_python", payload, ToolKind::kCustom);
    FAIL() << "expected generated custom payload rejection";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  }
}

TEST(TranscriptCustomToolTest, SuppliedAndGeneratedCustomCallsRejectInvalidUtf8) {
  const std::string payload{"\xC3\x28", 2};

  EXPECT_THROW(MakeSuppliedToolCall("call_1", "run_python", payload, ToolKind::kCustom),
               fl::Exception);
  EXPECT_THROW(MakeGeneratedToolCall("call_1", "run_python", payload, ToolKind::kCustom),
               fl::Exception);
}

TEST(TranscriptCustomToolTest, SuppliedFunctionCallStillRejectsNonObjectArguments) {
  EXPECT_THROW(MakeSuppliedToolCall("call_1", "get_weather", "not json", ToolKind::kFunction), fl::Exception);
}

TEST(TranscriptCustomToolTest, GeneratedAndReplayedCustomCallsNormalizeIdentically) {
  // Round-trip: what the model produced, unwrapped on the way out, replayed by the caller, renders the same bytes
  // back to the model on the next turn.
  const std::string payload = "line one\n\ttabbed\tünïcode  ";
  auto generated = MakeGeneratedToolCall("call_1", "run_python", payload, ToolKind::kCustom);
  auto replayed = MakeSuppliedToolCall("call_1", "run_python", payload, ToolKind::kCustom);

  EXPECT_EQ(generated.call.arguments, replayed.arguments);
  EXPECT_EQ(generated.call.normalized_arguments, replayed.normalized_arguments);
  EXPECT_EQ(generated.call.kind, replayed.kind);

  // And the wrapped form is exactly what the extractor unwraps back to the payload.
  EXPECT_EQ(ExtractCustomToolInput(replayed.normalized_arguments.dump()), payload);
}

TEST(TranscriptCustomToolTest, CustomCallProjectsTheSynthesizedInputShape) {
  // The chat template and the tool-call grammar only understand function-shaped tools, so a custom call must render
  // as the single-string object the model was prompted with.
  std::vector<TranscriptMessage> messages = {
      MakeAssistant("", {MakeSuppliedToolCall("call_1", "run_python", "print('hi')", ToolKind::kCustom)})};

  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"json([{"role":"assistant","content":"","tool_calls":[{"id":"call_1","type":"function",)json"
            R"json("function":{"name":"run_python","arguments":{"input":"print('hi')"}}}]}])json");
}

TEST(TranscriptCustomToolTest, CustomKindSurvivesCommitAndFullHistoryProjection) {
  ChatTranscript transcript;
  transcript.CommitTurn({UserMessage("Print hi")},
                        MakeAssistant("", {MakeSuppliedToolCall("call_1", "run_python", "print('hi')",
                                                                ToolKind::kCustom)}),
                        {});
  transcript.CommitTurn({TranscriptMessage::ToolResult("call_1", "hi")}, MakeAssistant("Done.", {}), {});

  auto calls = transcript.Messages()[1].ToolCalls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0]->kind, ToolKind::kCustom);
  EXPECT_EQ(calls[0]->arguments, "print('hi')");

  EXPECT_EQ(BuildChatMessagesJson(transcript.Messages()),
            R"json([{"role":"user","content":"Print hi"},)json"
            R"json({"role":"assistant","content":"","tool_calls":[{"id":"call_1","type":"function",)json"
            R"json("function":{"name":"run_python","arguments":{"input":"print('hi')"}}}]},)json"
            R"json({"role":"tool","content":"hi","tool_call_id":"call_1"},)json"
            R"json({"role":"assistant","content":"Done."}])json");
}

// ===========================================================================
// Item ingestion with a per-request kind snapshot
// ===========================================================================

TEST(TranscriptIngestTest, ReplayedCustomCallIsNormalizedThroughTheKindSnapshot) {
  Request request;
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "run_python", "print('hi')"));

  const std::unordered_map<std::string, ToolKind> kinds = {{"run_python", ToolKind::kCustom}};
  auto messages = BuildTranscriptMessages(request.items, kinds);

  ASSERT_FALSE(messages.empty());
  auto calls = messages.back().ToolCalls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0]->kind, ToolKind::kCustom);
  EXPECT_EQ(calls[0]->arguments, "print('hi')");
  EXPECT_EQ(calls[0]->normalized_arguments, nlohmann::ordered_json({{"input", "print('hi')"}}));
}

TEST(TranscriptIngestTest, NamesAbsentFromTheKindSnapshotAreFunctionTools) {
  Request request;
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "get_weather", R"({"city":"Seattle"})"));

  const std::unordered_map<std::string, ToolKind> kinds = {{"run_python", ToolKind::kCustom}};
  auto messages = BuildTranscriptMessages(request.items, kinds);

  auto calls = messages.back().ToolCalls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0]->kind, ToolKind::kFunction);
  EXPECT_EQ(calls[0]->normalized_arguments, nlohmann::ordered_json({{"city", "Seattle"}}));
}

TEST(TranscriptIngestTest, ReplayedCustomCallWithoutTheSnapshotIsRejectedAsMalformedJson) {
  // Documents why the snapshot has to reach ingestion: read as a function call, a text payload is not a JSON object.
  Request request;
  request.AddOwnedItem(std::make_unique<ToolCallItem>("call_1", "run_python", "print('hi')"));

  EXPECT_THROW(BuildTranscriptMessages(request.items), fl::Exception);
}
