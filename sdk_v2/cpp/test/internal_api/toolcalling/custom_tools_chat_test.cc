// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Chat Completions custom-tool support: parsing mixed function/custom tool arrays and tool choices,
// reading a prior turn's calls back out of a transcript, and reporting produced calls on the wire.
//
#include "contracts/chat_completions.h"

#include "contracts/chat_completions_converter.h"
#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace fl;
using namespace fl::chat_completions;
using json = nlohmann::json;

namespace {

constexpr const char* kPatch = "*** Begin Patch\n*** Update File: a.txt\n-old\n+new\n*** End Patch\n";

#define EXPECT_INVALID_ARGUMENT(expr)                                        \
  try {                                                                      \
    expr;                                                                    \
    ADD_FAILURE() << "expected fl::Exception from: " #expr;                  \
  } catch (const fl::Exception& ex) {                                        \
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) << ex.what(); \
  }

ChatCompletionRequest RequestWithTools(const std::string& tools_array_json) {
  auto j = json::parse(R"({"model": "m", "messages": [{"role": "user", "content": "hi"}]})");
  j["tools"] = json::parse(tools_array_json);
  return j.get<ChatCompletionRequest>();
}

/// The tool_calls array of a built response, serialized exactly as it goes on the wire.
json ToolCallsOf(const ChatCompletionResponse& response) {
  const json serialized = response;
  return serialized.at("choices").at(0).at("message").at("tool_calls");
}

/// The kind snapshot a session declaring `apply_patch` as a custom tool hands this turn. Names absent from it are
/// function tools, which is what makes the kind authoritative rather than inferred.
std::unordered_map<std::string, ToolKind> CustomApplyPatchKinds() {
  return {{"apply_patch", ToolKind::kCustom}, {"bash", ToolKind::kFunction}};
}

Response ResponseWithCalls(std::vector<std::unique_ptr<Item>> items) {
  Response response;
  response.items = std::move(items);
  response.finish_reason = FOUNDRY_LOCAL_FINISH_TOOL_CALLS;
  return response;
}

}  // namespace

// ========================================================================
// Tool declarations — mixed arrays, nesting, formats
// ========================================================================

TEST(ChatCustomToolDeclarationTest, ParsesMixedFunctionAndCustomTools) {
  auto req = RequestWithTools(R"([
    {"type": "function", "function": {"name": "get_weather", "description": "w",
                                       "parameters": {"type": "object"}}},
    {"type": "custom", "custom": {"name": "apply_patch", "description": "p", "format": {"type": "text"}}}
  ])");

  ASSERT_TRUE(req.tools.has_value());
  ASSERT_EQ(req.tools->size(), 2u);

  EXPECT_FALSE((*req.tools)[0].IsCustom());
  EXPECT_EQ((*req.tools)[0].Name(), "get_weather");

  EXPECT_TRUE((*req.tools)[1].IsCustom());
  EXPECT_EQ((*req.tools)[1].Name(), "apply_patch");
  EXPECT_EQ((*req.tools)[1].custom->description.value_or(""), "p");
}

TEST(ChatCustomToolDeclarationTest, AcceptsCustomToolWithNoFormat) {
  auto req = RequestWithTools(R"([{"type": "custom", "custom": {"name": "apply_patch"}}])");

  ASSERT_TRUE(req.tools.has_value());
  ASSERT_EQ(req.tools->size(), 1u);
  EXPECT_TRUE((*req.tools)[0].IsCustom());
}

TEST(ChatCustomToolDeclarationTest, RejectsNestedLarkGrammarFormat) {
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([
    {"type": "custom", "custom": {"name": "apply_patch",
      "format": {"type": "grammar", "grammar": {"syntax": "lark", "definition": "start: X"}}}}
  ])"));
}

TEST(ChatCustomToolDeclarationTest, RejectsMalformedNestedGrammarFormat) {
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([
    {"type": "custom", "custom": {"name": "apply_patch",
      "format": {"type": "grammar", "syntax": "lark", "definition": "start: X"}}}
  ])"));
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([
    {"type": "custom", "custom": {"name": "apply_patch",
      "format": {"type": "grammar", "grammar": {"syntax": "lark"}}}}
  ])"));
}

TEST(ChatCustomToolDeclarationTest, RejectsUnknownToolType) {
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([{"type": "web_search", "web_search": {"name": "s"}}])"));
}

TEST(ChatCustomToolDeclarationTest, RejectsWrongNesting) {
  // A custom tool nested under "function", and a function tool nested under "custom".
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([{"type": "custom", "function": {"name": "apply_patch"}}])"));
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([{"type": "function", "custom": {"name": "apply_patch"}}])"));
}

TEST(ChatCustomToolDeclarationTest, RejectsFunctionOnlyMembersAndWrongSiblingRegardlessOfValue) {
  EXPECT_INVALID_ARGUMENT(RequestWithTools(
      R"([{"type":"custom","custom":{"name":"x","parameters":null}}])"));
  EXPECT_INVALID_ARGUMENT(
      RequestWithTools(R"([{"type":"custom","custom":{"name":"x","strict":false}}])"));
  EXPECT_INVALID_ARGUMENT(RequestWithTools(
      R"([{"type":"custom","custom":{"name":"x"},"function":null}])"));
  EXPECT_INVALID_ARGUMENT(RequestWithTools(
      R"([{"type":"function","function":{"name":"x"},"custom":null}])"));
}

TEST(ChatCustomToolDeclarationTest, FunctionParametersAndStrictHaveStableValidation) {
  EXPECT_INVALID_ARGUMENT(RequestWithTools(
      R"([{"type":"function","function":{"name":"x","parameters":[]}}])"));
  EXPECT_INVALID_ARGUMENT(RequestWithTools(
      R"([{"type":"function","function":{"name":"x","strict":"true"}}])"));
  EXPECT_INVALID_ARGUMENT(RequestWithTools(
      R"([{"type":"function","function":{"name":"x","parameters":{},"strict":true}}])"));
}

TEST(ChatCustomToolDeclarationTest, FunctionNullsAreUnspecifiedAndStrictFalseIsPreserved) {
  const auto absent = RequestWithTools(
      R"([{"type":"function","function":{"name":"absent"}}])");
  const auto nulls = RequestWithTools(
      R"([{"type":"function","function":{"name":"nulls","parameters":null,"strict":null}}])");
  const auto values = RequestWithTools(
      R"([{"type":"function","function":{"name":"values","parameters":{"type":"object"},"strict":false}}])");

  EXPECT_FALSE((*absent.tools)[0].function.parameters.has_value());
  EXPECT_FALSE((*absent.tools)[0].function.strict.has_value());
  EXPECT_FALSE((*nulls.tools)[0].function.parameters.has_value());
  EXPECT_FALSE((*nulls.tools)[0].function.strict.has_value());
  const json normalized_nulls = (*nulls.tools)[0];
  EXPECT_FALSE(normalized_nulls.at("function").contains("parameters"));
  EXPECT_FALSE(normalized_nulls.at("function").contains("strict"));

  EXPECT_EQ((*values.tools)[0].function.parameters, json::parse(R"({"type":"object"})"));
  EXPECT_EQ((*values.tools)[0].function.strict, false);

  Request request;
  const auto definitions = ExtractToolDefinitions(values, request);
  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions[0].strict, false);

  const json echoed = (*values.tools)[0];
  EXPECT_EQ(echoed.at("function").at("strict"), false);

  EXPECT_INVALID_ARGUMENT(RequestWithTools(
      R"([{"type":"function","function":{"name":"x","parameters":7}}])"));
  EXPECT_INVALID_ARGUMENT(RequestWithTools(
      R"([{"type":"function","function":{"name":"x","strict":{}}}])"));
}

TEST(ChatCustomToolDeclarationTest, RejectsMissingOrEmptyName) {
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([{"type": "custom", "custom": {"description": "p"}}])"));
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([{"type": "custom", "custom": {"name": ""}}])"));
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([{"type": "function", "function": {"description": "w"}}])"));
}

TEST(ChatCustomToolDeclarationTest, RejectsNonStringToolType) {
  EXPECT_INVALID_ARGUMENT(RequestWithTools(R"([{"type": 7, "function": {"name": "x"}}])"));
}

TEST(ChatCustomToolDeclarationTest, RoundTripsCustomDeclaration) {
  auto req = RequestWithTools(R"([{"type": "custom", "custom": {"name": "apply_patch", "description": "p"}}])");

  const json serialized = (*req.tools)[0];
  EXPECT_EQ(serialized.at("type"), "custom");
  EXPECT_EQ(serialized.at("custom").at("name"), "apply_patch");
  EXPECT_EQ(serialized.at("custom").at("description"), "p");
  EXPECT_EQ(serialized.at("custom").at("format"), json::parse(R"({"type":"text"})"));
  EXPECT_FALSE(serialized.contains("function"));
}

// ========================================================================
// Tool definitions — custom-only, function-only, mixed
// ========================================================================

TEST(ChatCustomToolDefinitionTest, CustomOnlyRequestProducesOneCustomDefinition) {
  auto req = RequestWithTools(R"([{"type": "custom", "custom": {"name": "apply_patch", "description": "p"}}])");
  Request session_request;

  auto definitions = ExtractToolDefinitions(req, session_request);

  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions[0].name, "apply_patch");
  EXPECT_EQ(definitions[0].description, "p");
  EXPECT_EQ(definitions[0].kind, ToolKind::kCustom);
  EXPECT_TRUE(definitions[0].json_schema.empty());
}

TEST(ChatCustomToolDefinitionTest, MixedRequestKeepsDeclarationOrderAndKinds) {
  auto req = RequestWithTools(R"([
    {"type": "custom", "custom": {"name": "apply_patch"}},
    {"type": "function", "function": {"name": "bash", "parameters": {"type": "object"}}},
    {"type": "custom", "custom": {"name": "write_note"}}
  ])");
  Request session_request;

  auto definitions = ExtractToolDefinitions(req, session_request);

  ASSERT_EQ(definitions.size(), 3u);
  EXPECT_EQ(definitions[0].name, "apply_patch");
  EXPECT_EQ(definitions[0].kind, ToolKind::kCustom);
  EXPECT_EQ(definitions[1].name, "bash");
  EXPECT_EQ(definitions[1].kind, ToolKind::kFunction);
  EXPECT_EQ(definitions[2].name, "write_note");
  EXPECT_EQ(definitions[2].kind, ToolKind::kCustom);
}

// ========================================================================
// tool_choice
// ========================================================================

TEST(ChatToolChoiceTest, ParsesModeStrings) {
  for (const auto& [raw, expected] : std::vector<std::pair<const char*, ChatCompletionToolChoice::Kind>>{
           {R"("auto")", ChatCompletionToolChoice::Kind::kAuto},
           {R"("none")", ChatCompletionToolChoice::Kind::kNone},
           {R"("required")", ChatCompletionToolChoice::Kind::kRequired}}) {
    auto choice = json::parse(raw).get<ChatCompletionToolChoice>();
    EXPECT_EQ(choice.kind, expected) << raw;
    EXPECT_FALSE(choice.IsForced()) << raw;
  }
}

TEST(ChatToolChoiceTest, RejectsUnknownModeString) {
  EXPECT_INVALID_ARGUMENT(json::parse(R"("sometimes")").get<ChatCompletionToolChoice>());
}

TEST(ChatToolChoiceTest, RejectsMalformedForcedChoices) {
  EXPECT_INVALID_ARGUMENT(json::parse(R"({"type":"function"})").get<ChatCompletionToolChoice>());
  EXPECT_INVALID_ARGUMENT(json::parse(R"({"type":"custom","function":{"name":"x"}})").get<ChatCompletionToolChoice>());
  EXPECT_INVALID_ARGUMENT(json::parse(R"({"type":"grammar","custom":{"name":"x"}})").get<ChatCompletionToolChoice>());
  EXPECT_INVALID_ARGUMENT(json::parse("7").get<ChatCompletionToolChoice>());
}

TEST(ChatToolChoiceTest, ForcedCustomToolNarrowsToThatToolAndRequiresACall) {
  auto req = RequestWithTools(R"([
    {"type": "function", "function": {"name": "bash", "parameters": {"type": "object"}}},
    {"type": "custom", "custom": {"name": "apply_patch"}}
  ])");
  req.tool_choice = json::parse(R"({"type": "custom", "custom": {"name": "apply_patch"}})")
                        .get<ChatCompletionToolChoice>();

  Request session_request;
  auto definitions = ExtractToolDefinitions(req, session_request);

  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions[0].name, "apply_patch");
  EXPECT_EQ(definitions[0].kind, ToolKind::kCustom);
  EXPECT_EQ(std::string(session_request.options.Find("tool_choice")), "required");
}

TEST(ChatToolChoiceTest, ForcedFunctionDoesNotSelectASameNamedCustomTool) {
  auto req = RequestWithTools(R"([
    {"type": "custom", "custom": {"name": "apply_patch"}},
    {"type": "function", "function": {"name": "bash", "parameters": {"type": "object"}}}
  ])");
  req.tool_choice = json::parse(R"({"type": "function", "function": {"name": "apply_patch"}})")
                        .get<ChatCompletionToolChoice>();

  Request session_request;
  EXPECT_THROW((void)ExtractToolDefinitions(req, session_request), fl::Exception);
}

// ========================================================================
// Prior turns — assistant calls and tool results become request items
// ========================================================================

TEST(ChatToolTranscriptTest, ParsesPriorFunctionAndCustomCalls) {
  auto j = json::parse(R"({
    "role": "assistant",
    "content": null,
    "tool_calls": [
      {"id": "call_1", "type": "function", "function": {"name": "bash", "arguments": "{\"command\":\"ls\"}"}},
      {"id": "call_2", "type": "custom", "custom": {"name": "apply_patch", "input": "PATCH"}}
    ]
  })");
  auto msg = j.get<ChatCompletionMessage>();

  ASSERT_EQ(msg.tool_calls.size(), 2u);

  EXPECT_FALSE(msg.tool_calls[0].IsCustom());
  EXPECT_EQ(msg.tool_calls[0].Payload(), R"({"command":"ls"})");

  EXPECT_TRUE(msg.tool_calls[1].IsCustom());
  EXPECT_EQ(msg.tool_calls[1].Name(), "apply_patch");
  EXPECT_EQ(msg.tool_calls[1].Payload(), "PATCH");
}

TEST(ChatToolTranscriptTest, RejectsCustomCallWithNonStringInput) {
  EXPECT_INVALID_ARGUMENT(json::parse(R"({
    "role": "assistant",
    "tool_calls": [{"id": "call_1", "type": "custom", "custom": {"name": "apply_patch", "input": {"a": 1}}}]
  })")
                              .get<ChatCompletionMessage>());
}

TEST(ChatToolTranscriptTest, CustomCallInputIsRequiredAndMayBeEmpty) {
  for (const auto* input_member : {"", R"(,"input":null)"}) {
    const auto raw = std::string(
                         R"({"role":"assistant","tool_calls":[{"id":"call_1","type":"custom",)"
                         R"("custom":{"name":"apply_patch")") +
                     input_member + "}}]}";
    EXPECT_INVALID_ARGUMENT(json::parse(raw).get<ChatCompletionMessage>());
  }

  const auto message = json::parse(
                           R"({"role":"assistant","tool_calls":[{"id":"call_1","type":"custom",)"
                           R"("custom":{"name":"apply_patch","input":""}}]})")
                           .get<ChatCompletionMessage>();
  EXPECT_EQ(message.tool_calls.at(0).Payload(), "");
}

TEST(ChatToolTranscriptTest, RejectsUnknownCallType) {
  EXPECT_INVALID_ARGUMENT(json::parse(R"({
    "role": "assistant",
    "tool_calls": [{"id": "call_1", "type": "mystery", "mystery": {"name": "x"}}]
  })")
                              .get<ChatCompletionMessage>());
}

TEST(ChatToolTranscriptTest, PriorCallsAndResultsBecomeCorrelatedRequestItems) {
  auto j = json::parse(R"({
    "model": "m",
    "messages": [
      {"role": "user", "content": "patch it"},
      {"role": "assistant", "content": null, "tool_calls": [
        {"id": "call_2", "type": "custom", "custom": {"name": "apply_patch", "input": "PATCH BODY"}}
      ]},
      {"role": "tool", "tool_call_id": "call_2", "content": "applied"}
    ]
  })");
  auto req = j.get<ChatCompletionRequest>();

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 3u);
  EXPECT_EQ(session_request.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);

  ASSERT_EQ(session_request.items[1]->type, FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  const auto& call = static_cast<const ToolCallItem&>(*session_request.items[1]);
  EXPECT_EQ(call.call_id, "call_2");
  EXPECT_EQ(call.name, "apply_patch");
  EXPECT_EQ(call.arguments, "PATCH BODY") << "raw custom input is carried through, not re-encoded";

  ASSERT_EQ(session_request.items[2]->type, FOUNDRY_LOCAL_ITEM_TOOL_RESULT);
  const auto& result = static_cast<const ToolResultItem&>(*session_request.items[2]);
  EXPECT_EQ(result.call_id, call.call_id) << "the result stays correlated with the call";
  EXPECT_EQ(result.result, "applied");
}

TEST(ChatToolTranscriptTest, FunctionOnlyTranscriptIsUnchanged) {
  auto j = json::parse(R"({
    "model": "m",
    "messages": [
      {"role": "user", "content": "weather?"},
      {"role": "assistant", "content": null, "tool_calls": [
        {"id": "call_1", "type": "function", "function": {"name": "get_weather", "arguments": "{\"city\":\"Seattle\"}"}}
      ]},
      {"role": "tool", "tool_call_id": "call_1", "content": "sunny"}
    ]
  })");
  auto req = j.get<ChatCompletionRequest>();

  Request session_request;
  BuildRequestItems(req, session_request);

  ASSERT_EQ(session_request.items.size(), 3u);
  const auto& call = static_cast<const ToolCallItem&>(*session_request.items[1]);
  EXPECT_EQ(call.call_id, "call_1");
  EXPECT_EQ(call.name, "get_weather");
  EXPECT_EQ(call.arguments, R"({"city":"Seattle"})");
}

// ========================================================================
// Replayed transcripts reaching the prompt
//
// Building the right request items is only half the job: the model is prompted from what
// BuildChatMessagesJson projects, so a call or result that never becomes part of a transcript
// message is a call or result the model never sees. These assert the whole way through to the
// template input, including the two orderings a client depends on — assistant text before the
// calls it introduced, and parallel calls kept on one assistant turn.
// ========================================================================

TEST(ChatToolTranscriptTest, PriorCustomCallAndResultReachTheTemplate) {
  auto j = json::parse(R"({
    "model": "m",
    "messages": [
      {"role": "user", "content": "patch it"},
      {"role": "assistant", "content": null, "tool_calls": [
        {"id": "call_2", "type": "custom", "custom": {"name": "apply_patch", "input": "PATCH BODY"}}
      ]},
      {"role": "tool", "tool_call_id": "call_2", "content": "applied"}
    ]
  })");
  auto req = j.get<ChatCompletionRequest>();

  Request session_request;
  BuildRequestItems(req, session_request);

  auto messages = BuildTranscriptMessages(session_request.items, CustomApplyPatchKinds());

  // The raw payload is what the transcript records; only the projection wraps it.
  ASSERT_EQ(messages.size(), 3u);
  const auto calls = messages[1].ToolCalls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0]->kind, ToolKind::kCustom);
  EXPECT_EQ(calls[0]->arguments, "PATCH BODY") << "the transcript keeps the raw text";

  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"user","content":"patch it"},)"
            R"({"role":"assistant","content":"","tool_calls":[{"id":"call_2","type":"function",)"
            R"("function":{"name":"apply_patch","arguments":{"input":"PATCH BODY"}}}]},)"
            R"({"role":"tool","content":"applied","tool_call_id":"call_2"}])");
}

TEST(ChatToolTranscriptTest, AssistantContentStaysBeforeTheCallsItIntroduced) {
  // A preamble the model emitted before calling a tool is part of the same assistant turn and must precede the
  // calls, not follow them or split into a separate message.
  auto j = json::parse(R"({
    "model": "m",
    "messages": [
      {"role": "user", "content": "patch it"},
      {"role": "assistant", "content": "Let me apply that patch.", "tool_calls": [
        {"id": "call_2", "type": "custom", "custom": {"name": "apply_patch", "input": "PATCH BODY"}}
      ]}
    ]
  })");
  auto req = j.get<ChatCompletionRequest>();

  Request session_request;
  BuildRequestItems(req, session_request);

  auto messages = BuildTranscriptMessages(session_request.items, CustomApplyPatchKinds());

  ASSERT_EQ(messages.size(), 2u) << "the preamble and its calls are one assistant turn";
  EXPECT_EQ(messages[1].role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(messages[1].VisibleText(), "Let me apply that patch.");

  // Authoritative order within the message: text first, then the call.
  ASSERT_EQ(messages[1].entries.size(), 2u);
  EXPECT_EQ(messages[1].entries[0].kind, TranscriptEntry::Kind::kText);
  EXPECT_EQ(messages[1].entries[1].kind, TranscriptEntry::Kind::kToolCall);

  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"user","content":"patch it"},)"
            R"({"role":"assistant","content":"Let me apply that patch.","tool_calls":[{"id":"call_2",)"
            R"("type":"function","function":{"name":"apply_patch","arguments":{"input":"PATCH BODY"}}}]}])");
}

TEST(ChatToolTranscriptTest, TwoParallelCallsStayOnOneAssistantMessage) {
  // Parallel calls are one assistant turn. Splitting them into two messages would tell the model it took two turns.
  auto j = json::parse(R"({
    "model": "m",
    "messages": [
      {"role": "user", "content": "do both"},
      {"role": "assistant", "content": "On it.", "tool_calls": [
        {"id": "call_1", "type": "function", "function": {"name": "bash", "arguments": "{\"command\":\"ls\"}"}},
        {"id": "call_2", "type": "custom", "custom": {"name": "apply_patch", "input": "PATCH BODY"}}
      ]},
      {"role": "tool", "tool_call_id": "call_1", "content": "a.txt"},
      {"role": "tool", "tool_call_id": "call_2", "content": "applied"}
    ]
  })");
  auto req = j.get<ChatCompletionRequest>();

  Request session_request;
  BuildRequestItems(req, session_request);

  auto messages = BuildTranscriptMessages(session_request.items, CustomApplyPatchKinds());

  ASSERT_EQ(messages.size(), 4u) << "user, one assistant turn, then a result per call";

  const auto calls = messages[1].ToolCalls();
  ASSERT_EQ(calls.size(), 2u) << "both calls stay on the same assistant message";
  EXPECT_EQ(calls[0]->call_id, "call_1");
  EXPECT_EQ(calls[0]->kind, ToolKind::kFunction);
  EXPECT_EQ(calls[1]->call_id, "call_2");
  EXPECT_EQ(calls[1]->kind, ToolKind::kCustom);

  // Each kind projects through its own contract: JSON arguments for the function, the wrapped raw
  // payload for the custom tool.
  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"user","content":"do both"},)"
            R"({"role":"assistant","content":"On it.","tool_calls":[)"
            R"({"id":"call_1","type":"function","function":{"name":"bash","arguments":{"command":"ls"}}},)"
            R"({"id":"call_2","type":"function",)"
            R"("function":{"name":"apply_patch","arguments":{"input":"PATCH BODY"}}}]},)"
            R"({"role":"tool","content":"a.txt","tool_call_id":"call_1"},)"
            R"({"role":"tool","content":"applied","tool_call_id":"call_2"}])");

  // The replayed exchange is coherent: every call is answered.
  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateInputs(messages));
}

TEST(ChatToolTranscriptTest, ExplicitCustomKindSurvivesWithoutCurrentToolDefinitions) {
  auto j = json::parse(R"({
    "model": "m",
    "messages": [
      {"role": "assistant", "content": null, "tool_calls": [
        {"id": "call_1", "type": "custom", "custom": {"name": "apply_patch", "input": "PATCH BODY"}}
      ]},
      {"role": "tool", "tool_call_id": "call_1", "content": "sunny"}
    ]
  })");
  auto req = j.get<ChatCompletionRequest>();

  Request session_request;
  BuildRequestItems(req, session_request);

  auto messages = BuildTranscriptMessages(session_request.items, {});

  const auto calls = messages[0].ToolCalls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0]->kind, ToolKind::kCustom);
  EXPECT_EQ(calls[0]->arguments, "PATCH BODY");
  EXPECT_EQ(calls[0]->normalized_arguments.dump(), R"({"input":"PATCH BODY"})");
}

// ========================================================================
// Produced calls — wire shape, ids, raw input
// ========================================================================

TEST(ChatToolCallEmissionTest, CustomCallCarriesRawInputAndKeepsItsId) {
  auto call = MakeToolCall("call_abc", "apply_patch", kPatch, ToolKind::kCustom);

  EXPECT_EQ(call.id, "call_abc") << "the core call id is reported unchanged";
  EXPECT_EQ(call.type, "custom");
  ASSERT_TRUE(call.custom.has_value());
  EXPECT_EQ(call.custom->name, "apply_patch");
  EXPECT_EQ(call.custom->input, kPatch);

  const json serialized = call;
  EXPECT_EQ(serialized.at("id"), "call_abc");
  EXPECT_EQ(serialized.at("type"), "custom");
  EXPECT_EQ(serialized.at("custom").at("input"), kPatch);
  EXPECT_FALSE(serialized.contains("function")) << "a custom call has no function payload";
}

TEST(ChatToolCallEmissionTest, FunctionCallIsUnchanged) {
  auto call = MakeToolCall("call_abc", "get_weather", R"({"city":"Seattle"})", ToolKind::kFunction);

  const json serialized = call;
  EXPECT_EQ(serialized.at("type"), "function");
  EXPECT_EQ(serialized.at("function").at("name"), "get_weather");
  EXPECT_EQ(serialized.at("function").at("arguments"), R"({"city":"Seattle"})");
  EXPECT_FALSE(serialized.contains("custom"));
}

TEST(ChatToolCallEmissionTest, UndeclaredNameIsReportedAsAFunctionCall) {
  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>("call_1", "unknown_tool", "{}"));

  auto calls = ToolCallsOf(
      BuildResponse(ResponseWithCalls(std::move(items)), "chatcmpl-1", 100, "m"));
  EXPECT_EQ(calls.at(0).at("type"), "function");
}

TEST(ToolCallContextTest, EffectiveToolDefinitionsArePartOfTheCachedConfiguration) {
  ToolCallContext original;
  original.tools_json = R"([{"type":"function","function":{"name":"apply_patch"}}])";
  original.tool_kinds.emplace("apply_patch", ToolKind::kCustom);

  auto same = original;
  EXPECT_TRUE(original.HasSameTools(same));

  same.tool_kinds["apply_patch"] = ToolKind::kFunction;
  EXPECT_FALSE(original.HasSameTools(same));

  same = original;
  same.tools_json = R"([{"type":"function","function":{"name":"bash"}}])";
  EXPECT_FALSE(original.HasSameTools(same));
}

TEST(ChatToolCallEmissionTest, BuildResponseReportsEachCallUnderItsDeclaredKind) {
  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>(
      "call_1", "bash", R"({"command":"ls"})", false, ToolKind::kFunction));
  items.push_back(std::make_unique<ToolCallItem>(
      "call_2", "apply_patch", kPatch, false, ToolKind::kCustom));
  auto response = ResponseWithCalls(std::move(items));

  auto built = BuildResponse(response, "chatcmpl-1", 100, "m");
  auto calls = ToolCallsOf(built);

  ASSERT_EQ(calls.size(), 2u);

  EXPECT_EQ(calls[0].at("id"), "call_1");
  EXPECT_EQ(calls[0].at("type"), "function");
  EXPECT_EQ(calls[0].at("function").at("name"), "bash");
  EXPECT_EQ(calls[0].at("function").at("arguments"), R"({"command":"ls"})");

  EXPECT_EQ(calls[1].at("id"), "call_2");
  EXPECT_EQ(calls[1].at("type"), "custom");
  EXPECT_EQ(calls[1].at("custom").at("name"), "apply_patch");
  EXPECT_EQ(calls[1].at("custom").at("input"), kPatch);
}

TEST(ChatToolCallEmissionTest, SimultaneousCallsDoNotCrossAssociate) {
  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>("call_fn", "bash", R"({"command":"ls"})"));
  items.push_back(std::make_unique<ToolCallItem>(
      "call_custom", "apply_patch", kPatch, false, ToolKind::kCustom));
  auto response = ResponseWithCalls(std::move(items));

  auto calls = ToolCallsOf(BuildResponse(response, "chatcmpl-1", 100, "m"));

  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].at("function").at("arguments"), R"({"command":"ls"})");
  EXPECT_EQ(calls[1].at("custom").at("input"), kPatch);
  EXPECT_NE(calls[0].at("id"), calls[1].at("id"));
}

TEST(ChatToolCallEmissionTest, StreamedChunkAndFinalMessageAgreeOnIdAndPayload) {
  // What the streaming path emits for the call...
  auto streamed = MakeToolCall("call_abc", "apply_patch", kPatch, ToolKind::kCustom);
  streamed.index = 0;
  auto chunk = json::parse(FormatToolCallStreamingChunk({streamed}, "chatcmpl-1", 100, "m"));
  const auto& streamed_call = chunk.at("choices").at(0).at("delta").at("tool_calls").at(0);

  // ...and what the final message reports for the same call.
  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>(
      "call_abc", "apply_patch", kPatch, false, ToolKind::kCustom));
  auto final_call = ToolCallsOf(BuildResponse(ResponseWithCalls(std::move(items)), "chatcmpl-1", 100, "m"))
                        .at(0);

  EXPECT_EQ(streamed_call.at("id"), "call_abc");
  EXPECT_EQ(streamed_call.at("id"), final_call.at("id"));
  EXPECT_EQ(streamed_call.at("type"), "custom");
  EXPECT_EQ(streamed_call.at("type"), final_call.at("type"));
  EXPECT_EQ(streamed_call.at("custom").at("input"), kPatch);
  EXPECT_EQ(streamed_call.at("custom").at("input"), final_call.at("custom").at("input"));
  EXPECT_EQ(streamed_call.at("index"), 0);
}

TEST(ChatToolCallEmissionTest, RawInputWithJsonLikeContentIsNotCoerced) {
  // A payload that happens to look like JSON is still raw text and must survive byte for byte.
  const std::string payload = R"({"input": "not really arguments"}  trailing)";

  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>(
      "call_1", "apply_patch", payload, false, ToolKind::kCustom));

  auto calls = ToolCallsOf(
      BuildResponse(ResponseWithCalls(std::move(items)), "chatcmpl-1", 100, "m"));
  EXPECT_EQ(calls.at(0).at("custom").at("input"), payload);
}
