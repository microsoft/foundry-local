// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Responses custom-tool support: the flat custom declaration, forced custom choices and allowed
// tools, custom tool call / result items, and the streaming event lifecycle for a produced call.
//
#include "contracts/responses.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/openresponses/response_chain.h"
#include "inferencing/generative/openresponses/response_converter.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <variant>
#include <vector>

using namespace fl;
using namespace fl::responses;
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

ResponseCreateParams ParamsWithTools(const std::string& tools_array_json) {
  json j{{"model", "m"}, {"input", "hi"}};
  j["tools"] = json::parse(tools_array_json);
  return j.get<ResponseCreateParams>();
}

/// The kind snapshot a session declaring `apply_patch` as a custom tool hands this turn. Names absent from it are
/// function tools, which is what makes the kind authoritative rather than inferred.
std::unordered_map<std::string, ToolKind> CustomApplyPatchKinds() {
  return {{"apply_patch", ToolKind::kCustom}, {"bash", ToolKind::kFunction}};
}

ResponseCreateParams ParamsWithInput(const std::string& input_array_json) {
  json j{{"model", "m"}};
  j["input"] = json::parse(input_array_json);
  return j.get<ResponseCreateParams>();
}

fl::Response ResponseWithCalls(std::vector<std::unique_ptr<Item>> items) {
  fl::Response response;
  response.items = std::move(items);
  response.finish_reason = FOUNDRY_LOCAL_FINISH_TOOL_CALLS;
  return response;
}

/// The template input a request projects to, honoring the replay segment boundaries it carries.
///
/// Segment-aware ingestion is what a real turn uses, so a stored chain regroups per hop here exactly as it does in
/// ChatSession — comparing the flat grouping instead would hide a hop boundary being lost.
std::string ProjectRequest(const Request& request) {
  auto ingest = IngestRequestItems(request.items, request.item_segment_starts, CustomApplyPatchKinds());
  return BuildChatMessagesJson(ingest.messages);
}

}  // namespace

// ========================================================================
// Flat custom tool declarations
// ========================================================================

TEST(ResponsesCustomToolDeclarationTest, ParsesFlatCustomAndFunctionTools) {
  auto params = ParamsWithTools(R"([
    {"type": "function", "name": "bash", "description": "run", "parameters": {"type": "object"}},
    {"type": "custom", "name": "apply_patch", "description": "patch", "format": {"type": "text"}}
  ])");

  ASSERT_TRUE(params.tools.has_value());
  ASSERT_EQ(params.tools->size(), 2u);

  EXPECT_FALSE((*params.tools)[0].IsCustom());
  EXPECT_EQ((*params.tools)[0].Name(), "bash");

  EXPECT_TRUE((*params.tools)[1].IsCustom());
  EXPECT_EQ((*params.tools)[1].Name(), "apply_patch");
  EXPECT_EQ((*params.tools)[1].custom->description.value_or(""), "patch");
}

TEST(ResponsesCustomToolDeclarationTest, AcceptsCustomToolWithNoFormat) {
  auto params = ParamsWithTools(R"([{"type": "custom", "name": "apply_patch"}])");

  ASSERT_TRUE(params.tools.has_value());
  EXPECT_TRUE((*params.tools)[0].IsCustom());
}

TEST(ResponsesCustomToolDeclarationTest, RejectsFlatLarkGrammarFormat) {
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(
      R"([{"type": "custom", "name": "apply_patch", "format": {"type": "grammar", "syntax": "lark",
           "definition": "start: X"}}])"));
}

TEST(ResponsesCustomToolDeclarationTest, RejectsMalformedGrammarAndUnknownFormats) {
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(
      R"([{"type": "custom", "name": "apply_patch", "format": {"type": "grammar",
           "grammar": {"syntax": "lark", "definition": "start: X"}}}])"));
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(
      R"([{"type": "custom", "name": "apply_patch", "format": {"type": "grammar", "syntax": "lark"}}])"));
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type": "custom", "name": "apply_patch", "format": {"type": "binary"}}])"));
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type": "custom", "name": "apply_patch", "format": "text"}])"));
}

TEST(ResponsesCustomToolDeclarationTest, RejectsCustomToolWithoutAName) {
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(R"([{"type": "custom", "description": "patch"}])"));
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(R"([{"type": "custom", "name": ""}])"));
}

TEST(ResponsesCustomToolDeclarationTest, RejectsFunctionMembersAndNestedCustomRegardlessOfValue) {
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type":"custom","name":"x","parameters":null}])"));
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type":"custom","name":"x","strict":false}])"));
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type":"custom","name":"x","custom":null}])"));
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type":"custom","name":"x","function":null}])"));
}

TEST(ResponsesCustomToolDeclarationTest, FunctionMembersHaveStableValidation) {
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type":"function","name":"x","parameters":[]}])"));
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type":"function","name":"x","strict":"true"}])"));
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type":"function","function":null}])"));
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type":"function","name":"x","custom":null}])"));
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type":"function","name":"x","parameters":{},"strict":true}])"));
}

TEST(ResponsesCustomToolDeclarationTest, FunctionNullableFieldsRoundTripWithoutLosingPresence) {
  const auto absent = ParamsWithTools(R"([{"type":"function","name":"absent"}])");
  const auto nulls =
      ParamsWithTools(R"([{"type":"function","name":"nulls","parameters":null,"strict":null}])");
  const auto values = ParamsWithTools(
      R"([{"type":"function","name":"values","parameters":{"type":"object"},"strict":false}])");

  const json absent_json = (*absent.tools)[0];
  const json nulls_json = (*nulls.tools)[0];
  const json false_json = (*values.tools)[0];

  EXPECT_FALSE(absent_json.contains("parameters"));
  EXPECT_FALSE(absent_json.contains("strict"));
  EXPECT_TRUE(nulls_json.at("parameters").is_null());
  EXPECT_TRUE(nulls_json.at("strict").is_null());
  EXPECT_EQ(false_json.at("parameters"), json::parse(R"({"type":"object"})"));
  EXPECT_EQ(false_json.at("strict"), false);

  Request request;
  const auto definitions = ResponseConverter::ExtractResponsesToolDefinitions(values, request);
  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions[0].strict, false);

  Request null_request;
  const auto null_definitions =
      ResponseConverter::ExtractResponsesToolDefinitions(nulls, null_request);
  ASSERT_EQ(null_definitions.size(), 1u);
  EXPECT_FALSE(null_definitions[0].strict.has_value());
  EXPECT_FALSE(null_definitions[0].include_parameters_in_prompt);

  const json echoed_response =
      ResponseConverter::BuildInitialResponseObject("resp_1", 10, "m", nulls);
  ASSERT_EQ(echoed_response.at("tools").size(), 1u);
  EXPECT_TRUE(echoed_response.at("tools").at(0).at("parameters").is_null());
  EXPECT_TRUE(echoed_response.at("tools").at(0).at("strict").is_null());

  EXPECT_INVALID_ARGUMENT(ParamsWithTools(
      R"([{"type":"function","name":"x","parameters":7}])"));
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(
      R"([{"type":"function","name":"x","strict":[]}])"));
}

TEST(ResponsesCustomToolDeclarationTest, RejectsUnknownToolType) {
  // Symmetric with Chat Completions: an unrecognized type is refused, not read as a function tool. Falling through
  // would offer the model a tool this runtime cannot honour — and for a type that nests its declaration, a nameless
  // one — instead of telling the caller it is unsupported.
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(R"([{"type": "web_search"}])"));
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(R"([{"type": "code_interpreter", "container": {"type": "auto"}}])"));

  // Notably a type that *does* carry a usable-looking name is still refused rather than silently downgraded.
  EXPECT_INVALID_ARGUMENT(
      ParamsWithTools(R"([{"type": "mcp", "name": "fetch", "parameters": {"type": "object"}}])"));

  // A non-string type is a malformed declaration, not an unknown one.
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(R"([{"type": 7, "name": "x"}])"));
}

TEST(ResponsesCustomToolDeclarationTest, UnknownTypeIsRejectedEvenAlongsideValidTools) {
  // The whole declaration is refused: a caller must not silently lose one tool out of a set it declared.
  EXPECT_INVALID_ARGUMENT(ParamsWithTools(R"([
    {"type": "function", "name": "bash", "parameters": {"type": "object"}},
    {"type": "custom", "name": "apply_patch"},
    {"type": "web_search"}
  ])"));
}

TEST(ResponsesCustomToolDeclarationTest, AbsentTypeStillDefaultsToFunction) {
  // Older clients omit `type` entirely; that remains a function declaration.
  auto params = ParamsWithTools(R"([{"name": "bash", "parameters": {"type": "object"}}])");

  ASSERT_TRUE(params.tools.has_value());
  ASSERT_EQ(params.tools->size(), 1u);
  EXPECT_FALSE((*params.tools)[0].IsCustom());
  EXPECT_EQ((*params.tools)[0].Name(), "bash");
}

TEST(ResponsesCustomToolDeclarationTest, EchoesCustomToolInTheFlatShape) {
  auto params = ParamsWithTools(R"([{"type": "custom", "name": "apply_patch", "description": "patch"}])");

  const json echoed = (*params.tools)[0];
  EXPECT_EQ(echoed.at("type"), "custom");
  EXPECT_EQ(echoed.at("name"), "apply_patch");
  EXPECT_EQ(echoed.at("description"), "patch");
  EXPECT_EQ(echoed.at("format"), json::parse(R"({"type":"text"})"));
  EXPECT_FALSE(echoed.contains("parameters"));
}

// ========================================================================
// Definitions, forced choices and allowed tools
// ========================================================================

TEST(ResponsesCustomToolDefinitionTest, MixedToolsKeepOrderAndKinds) {
  auto params = ParamsWithTools(R"([
    {"type": "custom", "name": "apply_patch"},
    {"type": "function", "name": "bash", "parameters": {"type": "object"}}
  ])");

  Request session_request;
  auto definitions = ResponseConverter::ExtractResponsesToolDefinitions(params, session_request);

  ASSERT_EQ(definitions.size(), 2u);
  EXPECT_EQ(definitions[0].name, "apply_patch");
  EXPECT_EQ(definitions[0].kind, fl::ToolKind::kCustom);
  EXPECT_TRUE(definitions[0].json_schema.empty());
  EXPECT_EQ(definitions[1].name, "bash");
  EXPECT_EQ(definitions[1].kind, fl::ToolKind::kFunction);
}

TEST(ResponsesToolChoiceTest, ParsesForcedCustomChoice) {
  json j{{"model", "m"}, {"input", "hi"}};
  j["tool_choice"] = json::parse(R"({"type": "custom", "name": "apply_patch"})");
  auto params = j.get<ResponseCreateParams>();

  ASSERT_TRUE(params.tool_choice.has_value());
  ASSERT_TRUE(std::holds_alternative<ForcedCustomTool>(*params.tool_choice));
  EXPECT_EQ(std::get<ForcedCustomTool>(*params.tool_choice).name, "apply_patch");
}

TEST(ResponsesToolChoiceTest, RejectsMalformedChoices) {
  auto with_choice = [](const char* raw) {
    json j{{"model", "m"}, {"input", "hi"}};
    j["tool_choice"] = json::parse(raw);
    return j.get<ResponseCreateParams>();
  };

  EXPECT_INVALID_ARGUMENT(with_choice(R"("sometimes")"));
  EXPECT_INVALID_ARGUMENT(with_choice(R"({"type": "custom"})"));
  EXPECT_INVALID_ARGUMENT(with_choice(R"({"type": "grammar", "name": "x"})"));
  EXPECT_INVALID_ARGUMENT(
      with_choice(R"({"type":"allowed_tools","mode":"none","tools":[]})"));
  EXPECT_INVALID_ARGUMENT(
      with_choice(R"({"type":"allowed_tools","mode":"auto","tools":[]})"));
  EXPECT_INVALID_ARGUMENT(
      with_choice(R"({"type":"allowed_tools","mode":"auto","tools":[{"type":"custom"}]})"));
  EXPECT_INVALID_ARGUMENT(with_choice(
      R"({"type":"allowed_tools","mode":"auto","tools":[{"type":"grammar","name":"x"}]})"));
  EXPECT_INVALID_ARGUMENT(with_choice("7"));
}

TEST(ResponsesToolChoiceTest, AllowedToolsChoiceFiltersByKindAndNameAndEchoesExactly) {
  auto params = ParamsWithTools(R"([
    {"type":"function","name":"same","parameters":{}},
    {"type":"custom","name":"apply_patch"}
  ])");
  params.tool_choice =
      json::parse(R"({"type":"allowed_tools","mode":"required",)"
                  R"("tools":[{"type":"custom","name":"apply_patch"}]})")
          .get<AllowedToolsChoice>();

  Request request;
  const auto definitions =
      ResponseConverter::ExtractResponsesToolDefinitions(params, request);

  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions.front().name, "apply_patch");
  EXPECT_EQ(definitions.front().kind, ToolKind::kCustom);
  EXPECT_EQ(std::string(request.options.Find("tool_choice")), "required");

  const json response =
      ResponseConverter::BuildInitialResponseObject("resp_1", 10, "m", params);
  EXPECT_EQ(response.at("tool_choice"),
            json::parse(R"({"type":"allowed_tools","mode":"required",)"
                        R"("tools":[{"type":"custom","name":"apply_patch"}]})"));
}

TEST(ResponsesToolChoiceTest, ForcedCustomChoiceNarrowsToThatTool) {
  auto params = ParamsWithTools(R"([
    {"type": "function", "name": "bash", "parameters": {"type": "object"}},
    {"type": "custom", "name": "apply_patch"}
  ])");
  params.tool_choice = ForcedCustomTool{"apply_patch"};

  Request session_request;
  auto definitions = ResponseConverter::ExtractResponsesToolDefinitions(params, session_request);

  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions[0].name, "apply_patch");
  EXPECT_EQ(definitions[0].kind, fl::ToolKind::kCustom);
  EXPECT_EQ(std::string(session_request.options.Find("tool_choice")), "required");
}

TEST(ResponsesToolChoiceTest, ForcedCustomChoiceIsEchoedWithItsKind) {
  ResponseCreateParams params;
  params.model = "m";
  params.input = std::string("hi");
  params.tool_choice = ForcedCustomTool{"apply_patch"};

  auto response = ResponseConverter::BuildInitialResponseObject("resp_1", 10, "m", params);
  const json serialized = response;

  EXPECT_EQ(serialized.at("tool_choice"), json::parse(R"({"type":"custom","name":"apply_patch"})"));
}

TEST(ResponsesAllowedToolsTest, IncludesExcludesAndToleratesDuplicatesAndUnknowns) {
  auto params = ParamsWithTools(R"([
    {"type": "function", "name": "bash", "parameters": {"type": "object"}},
    {"type": "custom", "name": "apply_patch"},
    {"type": "function", "name": "view", "parameters": {"type": "object"}}
  ])");
  params.allowed_tools = std::vector<std::string>{"APPLY_PATCH", "apply_patch", "bash", "never_declared"};

  Request session_request;
  auto definitions = ResponseConverter::ExtractResponsesToolDefinitions(params, session_request);

  ASSERT_EQ(definitions.size(), 2u);
  EXPECT_EQ(definitions[0].name, "bash");
  EXPECT_EQ(definitions[1].name, "apply_patch");
  EXPECT_EQ(definitions[1].kind, fl::ToolKind::kCustom);
}

TEST(ResponsesAllowedToolsTest, ExcludingAForcedCustomToolIsRejected) {
  auto params = ParamsWithTools(R"([
    {"type": "custom", "name": "apply_patch"},
    {"type": "function", "name": "bash", "parameters": {"type": "object"}}
  ])");
  params.tool_choice = ForcedCustomTool{"apply_patch"};
  params.allowed_tools = std::vector<std::string>{"bash"};

  Request session_request;
  EXPECT_INVALID_ARGUMENT(
      ResponseConverter::ExtractResponsesToolDefinitions(params, session_request));
}

TEST(ResponsesAllowedToolsTest, OfficialReferencesMustMatchDeclaredKindAndNameExactly) {
  auto params = ParamsWithTools(R"([
    {"type":"function","name":"same","parameters":{}},
    {"type":"custom","name":"apply_patch"}
  ])");
  Request request;

  params.tool_choice =
      json::parse(R"({"type":"allowed_tools","mode":"auto",)"
                  R"("tools":[{"type":"function","name":"apply_patch"}]})")
          .get<AllowedToolsChoice>();
  EXPECT_INVALID_ARGUMENT(ResponseConverter::ExtractResponsesToolDefinitions(params, request));

  params.tool_choice =
      json::parse(R"({"type":"allowed_tools","mode":"auto",)"
                  R"("tools":[{"type":"custom","name":"Apply_Patch"}]})")
          .get<AllowedToolsChoice>();
  EXPECT_INVALID_ARGUMENT(ResponseConverter::ExtractResponsesToolDefinitions(params, request));

  params.tool_choice =
      json::parse(R"({"type":"allowed_tools","mode":"auto",)"
                  R"("tools":[{"type":"custom","name":"missing"}]})")
          .get<AllowedToolsChoice>();
  EXPECT_INVALID_ARGUMENT(ResponseConverter::ExtractResponsesToolDefinitions(params, request));
}

TEST(ResponsesAllowedToolsTest, OfficialDuplicateReferencesAreIdempotentAndPreservedForEcho) {
  auto params = ParamsWithTools(R"([{"type":"custom","name":"apply_patch"}])");
  params.tool_choice =
      json::parse(R"({"type":"allowed_tools","mode":"auto","tools":[)"
                  R"({"type":"custom","name":"apply_patch"},)"
                  R"({"type":"custom","name":"apply_patch"}]})")
          .get<AllowedToolsChoice>();

  Request request;
  const auto definitions = ResponseConverter::ExtractResponsesToolDefinitions(params, request);
  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions.front().name, "apply_patch");

  const json response =
      ResponseConverter::BuildInitialResponseObject("resp_1", 10, "m", params);
  EXPECT_EQ(response.at("tool_choice").at("tools"),
            json::parse(R"([{"type":"custom","name":"apply_patch"},)"
                        R"({"type":"custom","name":"apply_patch"}])"));
}

// ========================================================================
// Historical input items
// ========================================================================

TEST(ResponsesToolTranscriptTest, ParsesPriorCallsAndResultsOfBothKinds) {
  auto params = ParamsWithInput(R"([
    {"type": "message", "role": "user", "content": [{"type": "input_text", "text": "patch it"}]},
    {"type": "function_call", "call_id": "call_1", "name": "bash", "arguments": "{\"command\":\"ls\"}"},
    {"type": "function_call_output", "call_id": "call_1", "output": "a.txt"},
    {"type": "custom_tool_call", "call_id": "call_2", "name": "apply_patch", "input": "PATCH BODY"},
    {"type": "custom_tool_call_output", "call_id": "call_2", "output": "applied"}
  ])");

  Request request = ResponseConverter::ToSessionRequest(params);

  ASSERT_EQ(request.items.size(), 5u);
  EXPECT_EQ(request.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);

  const auto& function_call = static_cast<const ToolCallItem&>(*request.items[1]);
  EXPECT_EQ(function_call.call_id, "call_1");
  EXPECT_EQ(function_call.name, "bash");
  EXPECT_EQ(function_call.arguments, R"({"command":"ls"})");

  const auto& function_result = static_cast<const ToolResultItem&>(*request.items[2]);
  EXPECT_EQ(function_result.call_id, "call_1");
  EXPECT_EQ(function_result.result, "a.txt");

  const auto& custom_call = static_cast<const ToolCallItem&>(*request.items[3]);
  EXPECT_EQ(custom_call.call_id, "call_2");
  EXPECT_EQ(custom_call.name, "apply_patch");
  EXPECT_EQ(custom_call.arguments, "PATCH BODY") << "raw input is carried through, not re-encoded";

  const auto& custom_result = static_cast<const ToolResultItem&>(*request.items[4]);
  EXPECT_EQ(custom_result.call_id, "call_2");
  EXPECT_EQ(custom_result.result, "applied");
}

TEST(ResponsesToolTranscriptTest, RejectsCustomCallWithNonStringInput) {
  EXPECT_INVALID_ARGUMENT(ParamsWithInput(
      R"([{"type": "custom_tool_call", "call_id": "c", "name": "apply_patch", "input": {"a": 1}}])"));
}

TEST(ResponsesToolTranscriptTest, CustomCallInputIsRequiredAndMayBeEmpty) {
  EXPECT_INVALID_ARGUMENT(ParamsWithInput(
      R"([{"type":"custom_tool_call","call_id":"c","name":"apply_patch"}])"));
  EXPECT_INVALID_ARGUMENT(ParamsWithInput(
      R"([{"type":"custom_tool_call","call_id":"c","name":"apply_patch","input":null}])"));

  const auto params = ParamsWithInput(
      R"([{"type":"custom_tool_call","call_id":"c","name":"apply_patch","input":""}])");
  const auto& items = std::get<std::vector<InputItem>>(params.input);
  EXPECT_EQ(std::get<CustomToolCallInputItem>(items.at(0)).input, "");
}

TEST(ResponsesToolTranscriptTest, CallsAndResultsRequireNonEmptyIdentifiers) {
  for (const auto* raw : {
           R"([{"type":"function_call","name":"f","arguments":"{}"}])",
           R"([{"type":"function_call","call_id":"","name":"f","arguments":"{}"}])",
           R"([{"type":"function_call","call_id":"c","name":"","arguments":"{}"}])",
           R"([{"type":"custom_tool_call","name":"c","input":"raw"}])",
           R"([{"type":"custom_tool_call","call_id":"","name":"c","input":"raw"}])",
           R"([{"type":"custom_tool_call","call_id":"c","name":"","input":"raw"}])",
           R"([{"type":"function_call_output","call_id":"","output":"done"}])",
           R"([{"type":"custom_tool_call_output","call_id":"","output":"done"}])"}) {
    EXPECT_INVALID_ARGUMENT(ParamsWithInput(raw));
  }
}

TEST(ResponsesToolTranscriptTest, StringArgumentsAndCustomInputArePreservedExactly) {
  const auto function_payload = std::string("{ \"x\": 1 }\n");
  const auto custom_payload = std::string("line 1\nline 2  ");
  json request{{"model", "m"}};
  request["input"] = json::array({
      {{"type", "function_call"}, {"call_id", "call_f"}, {"name", "f"}, {"arguments", function_payload}},
      {{"type", "custom_tool_call"}, {"call_id", "call_c"}, {"name", "c"}, {"input", custom_payload}},
  });

  const auto params = request.get<ResponseCreateParams>();
  const auto& items = std::get<std::vector<InputItem>>(params.input);
  EXPECT_EQ(std::get<FunctionCallInputItem>(items[0]).arguments, function_payload);
  EXPECT_EQ(std::get<CustomToolCallInputItem>(items[1]).input, custom_payload);
}

TEST(ResponsesToolTranscriptTest, StoredChainReplaysCustomCallsAndResults) {
  // Drive the real store: a custom call is emitted on one hop and answered on the next, so the reconstructed chain
  // is the one the service actually replays rather than a hand-ordered stand-in.
  ResponseStore store;

  json first;
  first["id"] = "resp_1";
  first["previous_response_id"] = nullptr;
  first["output"] = json::array({{{"type", "custom_tool_call"},
                                  {"id", "ctc_1"},
                                  {"call_id", "call_2"},
                                  {"name", "apply_patch"},
                                  {"input", "PATCH BODY"},
                                  {"status", "completed"}}});
  store.Store("resp_1", first, json::array({{{"type", "message"}, {"role", "user"}, {"content", "patch it"}}}));

  json second;
  second["id"] = "resp_2";
  second["previous_response_id"] = "resp_1";
  second["output"] = json::array();
  store.Store("resp_2", second,
              json::array({{{"type", "custom_tool_call_output"}, {"call_id", "call_2"}, {"output", "applied"}}}));

  auto context = store.BuildChainContext("resp_2");
  ASSERT_TRUE(context.has_value());

  ResponseCreateParams params;
  params.model = "m";
  params.input = std::string("carry on");

  Request request = ResponseConverter::ToSessionRequest(params, &(*context));

  // Five items, not four: the second hop recorded an empty output, and replay still emits the assistant turn
  // boundary that hop committed live. Dropping it would leave the tool result and the new user turn adjacent.
  ASSERT_EQ(request.items.size(), 5u);
  EXPECT_EQ(request.items[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);

  // The call precedes the result that answers it — the chain is replayed in the order it happened.
  const auto& call = static_cast<const ToolCallItem&>(*request.items[1]);
  EXPECT_EQ(call.call_id, "call_2");
  EXPECT_EQ(call.name, "apply_patch");
  EXPECT_EQ(call.arguments, "PATCH BODY") << "the stored raw payload replays unchanged";

  const auto& result = static_cast<const ToolResultItem&>(*request.items[2]);
  EXPECT_EQ(result.result, "applied");
  EXPECT_EQ(result.call_id, call.call_id) << "the replayed call and its result stay correlated";

  EXPECT_EQ(request.items[3]->type, FOUNDRY_LOCAL_ITEM_MESSAGE) << "the empty hop still closes its assistant turn";
  EXPECT_EQ(request.items[4]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);

  // The replayed chain is coherent: the strict transcript path accepts it.
  auto messages = IngestRequestItems(request.items, request.item_segment_starts, CustomApplyPatchKinds()).messages;
  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateInputs(messages));
}

TEST(ResponsesToolTranscriptTest, StoredCustomReplayRequiresInputAndNonEmptyIdentifiers) {
  ResponseCreateParams params;
  params.model = "m";
  params.input = std::string("continue");

  for (const auto& call : {
           json{{"type", "custom_tool_call"},
                {"call_id", "call_1"},
                {"name", "apply_patch"}},
           json{{"type", "custom_tool_call"},
                {"call_id", "call_1"},
                {"name", "apply_patch"},
                {"input", nullptr}},
           json{{"type", "custom_tool_call"},
                {"call_id", ""},
                {"name", "apply_patch"},
                {"input", ""}},
           json{{"type", "custom_tool_call"},
                {"call_id", "call_1"},
                {"name", ""},
                {"input", ""}},
       }) {
    const ResponseChainContext context{
        ResponseChainHop{json::array(), json::array({call})}};
    EXPECT_INVALID_ARGUMENT(ResponseConverter::ToSessionRequest(params, &context));
  }

  const ResponseChainContext valid{
      ResponseChainHop{
          json::array(),
          json::array({{{"type", "custom_tool_call"},
                        {"call_id", "call_1"},
                        {"name", "apply_patch"},
                        {"input", ""}}})}};
  EXPECT_NO_THROW(ResponseConverter::ToSessionRequest(params, &valid));
}

TEST(ResponsesToolTranscriptTest, StoredInputItemsGetKindSpecificIds) {
  auto req_json = json::parse(R"({
    "model": "m",
    "input": [
      {"type": "custom_tool_call", "call_id": "call_2", "name": "apply_patch", "input": "PATCH"},
      {"type": "custom_tool_call_output", "call_id": "call_2", "output": "applied"}
    ]
  })");

  auto items = ResponseConverter::ToInputItems(req_json);

  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0].at("id").get<std::string>().rfind("ctc_", 0), 0u);
  EXPECT_EQ(items[1].at("id").get<std::string>().rfind("ctco_", 0), 0u);
  EXPECT_EQ(items[0].at("input"), "PATCH") << "the stored item keeps the raw payload";
}

// ========================================================================
// Replayed transcripts reaching the prompt
//
// The model is prompted from what BuildChatMessagesJson projects, so these carry both continuation
// styles — stateless echo and stored chain — all the way to the template input, and assert the two
// produce the same thing.
// ========================================================================

TEST(ResponsesToolTranscriptTest, StatelessCustomCallAndResultReachTheTemplate) {
  auto params = ParamsWithInput(R"([
    {"type": "message", "role": "user", "content": "patch it"},
    {"type": "custom_tool_call", "call_id": "call_2", "name": "apply_patch", "input": "PATCH BODY"},
    {"type": "custom_tool_call_output", "call_id": "call_2", "output": "applied"}
  ])");

  Request request = ResponseConverter::ToSessionRequest(params);
  auto messages = BuildTranscriptMessages(request.items, CustomApplyPatchKinds());

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

  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateInputs(messages));
}

TEST(ResponsesToolTranscriptTest, AssistantPreambleStaysBeforeTheCustomCall) {
  auto params = ParamsWithInput(R"([
    {"role": "user", "content": [{"type": "input_text", "text": "patch it"}]},
    {"role": "assistant", "content": [{"type": "output_text", "text": "Let me apply that patch."}]},
    {"type": "custom_tool_call", "call_id": "call_2", "name": "apply_patch", "input": "PATCH BODY"}
  ])");

  Request request = ResponseConverter::ToSessionRequest(params);
  auto messages = BuildTranscriptMessages(request.items, CustomApplyPatchKinds());

  ASSERT_EQ(messages.size(), 2u) << "the preamble and the call it introduced are one assistant turn";
  ASSERT_EQ(messages[1].entries.size(), 2u);
  EXPECT_EQ(messages[1].entries[0].kind, TranscriptEntry::Kind::kText);
  EXPECT_EQ(messages[1].entries[1].kind, TranscriptEntry::Kind::kToolCall);
  EXPECT_EQ(messages[1].VisibleText(), "Let me apply that patch.");
}

TEST(ResponsesToolTranscriptTest, TwoParallelCustomAndFunctionCallsStayOnOneAssistantMessage) {
  auto params = ParamsWithInput(R"([
    {"role": "user", "content": [{"type": "input_text", "text": "do both"}]},
    {"role": "assistant", "content": [{"type": "output_text", "text": "On it."}]},
    {"type": "function_call", "call_id": "call_1", "name": "bash", "arguments": "{\"command\":\"ls\"}"},
    {"type": "custom_tool_call", "call_id": "call_2", "name": "apply_patch", "input": "PATCH BODY"},
    {"type": "function_call_output", "call_id": "call_1", "output": "a.txt"},
    {"type": "custom_tool_call_output", "call_id": "call_2", "output": "applied"}
  ])");

  Request request = ResponseConverter::ToSessionRequest(params);
  auto messages = BuildTranscriptMessages(request.items, CustomApplyPatchKinds());

  ASSERT_EQ(messages.size(), 4u);
  const auto calls = messages[1].ToolCalls();
  ASSERT_EQ(calls.size(), 2u) << "both calls stay on the same assistant message";
  EXPECT_EQ(calls[0]->kind, ToolKind::kFunction);
  EXPECT_EQ(calls[1]->kind, ToolKind::kCustom);

  EXPECT_EQ(BuildChatMessagesJson(messages),
            R"([{"role":"user","content":"do both"},)"
            R"({"role":"assistant","content":"On it.","tool_calls":[)"
            R"({"id":"call_1","type":"function","function":{"name":"bash","arguments":{"command":"ls"}}},)"
            R"({"id":"call_2","type":"function",)"
            R"("function":{"name":"apply_patch","arguments":{"input":"PATCH BODY"}}}]},)"
            R"({"role":"tool","content":"a.txt","tool_call_id":"call_1"},)"
            R"({"role":"tool","content":"applied","tool_call_id":"call_2"}])");

  ChatTranscript transcript;
  EXPECT_NO_THROW(transcript.ValidateInputs(messages));
}

TEST(ResponsesToolTranscriptTest, StoredChainAndStatelessReplayProduceTheSameTemplateInput) {
  // The stored chain reconstructs the conversation from what the service persisted; the stateless form echoes it
  // back. A custom call must reach the model identically either way — there is only one chain reconstruction.
  //
  // The stored form is grouped by hop: each recorded turn is one segment, so the call the first hop produced stays
  // on its own assistant turn exactly as the live session committed it.
  ResponseChainContext previous_context{
      ResponseChainHop{
          json::parse(R"([{"type": "message", "role": "user", "content": "patch it"}])"),
          json::parse(
              R"([{"type": "custom_tool_call", "call_id": "call_2", "name": "apply_patch", "input": "PATCH BODY"}])")},
      ResponseChainHop{
          json::parse(R"([{"type": "custom_tool_call_output", "call_id": "call_2", "output": "applied"}])"),
          json::parse(R"([{"type": "message", "role": "assistant", "content": "Patched."}])")}};

  ResponseCreateParams chained;
  chained.model = "m";
  chained.input = std::string("and now?");
  auto chained_request = ResponseConverter::ToSessionRequest(chained, &previous_context);

  auto stateless = ParamsWithInput(R"([
    {"type": "message", "role": "user", "content": "patch it"},
    {"type": "custom_tool_call", "call_id": "call_2", "name": "apply_patch", "input": "PATCH BODY"},
    {"type": "custom_tool_call_output", "call_id": "call_2", "output": "applied"},
    {"type": "message", "role": "assistant", "content": "Patched."},
    {"role": "user", "content": [{"type": "input_text", "text": "and now?"}]}
  ])");
  auto stateless_request = ResponseConverter::ToSessionRequest(stateless);

  const auto chained_projection = ProjectRequest(chained_request);
  EXPECT_EQ(chained_projection, ProjectRequest(stateless_request));

  // The payload survives the store round trip byte for byte, and the call it belongs to is still the model's own
  // assistant turn rather than a turn of its own.
  EXPECT_EQ(chained_projection,
            R"([{"role":"user","content":"patch it"},)"
            R"({"role":"assistant","content":"","tool_calls":[{"id":"call_2","type":"function",)"
            R"("function":{"name":"apply_patch","arguments":{"input":"PATCH BODY"}}}]},)"
            R"({"role":"tool","content":"applied","tool_call_id":"call_2"},)"
            R"({"role":"assistant","content":"Patched."},)"
            R"({"role":"user","content":"and now?"}])");
}

// ========================================================================
// Produced calls — output items and streaming lifecycle
// ========================================================================

TEST(ResponsesToolCallEmissionTest, CustomCallBecomesACustomToolCallItem) {
  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>(
      "call_2", "apply_patch", kPatch, false, ToolKind::kCustom));

  auto [output, output_text] =
      ResponseConverter::FromSessionResponse(ResponseWithCalls(std::move(items)));

  ASSERT_EQ(output.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<CustomToolCallOutputItem>(output[0]));

  const auto& custom = std::get<CustomToolCallOutputItem>(output[0]);
  EXPECT_EQ(custom.call_id, "call_2") << "the core call id is reported unchanged";
  EXPECT_EQ(custom.name, "apply_patch");
  EXPECT_EQ(custom.input, kPatch);
  EXPECT_EQ(custom.id.rfind("ctc_", 0), 0u);

  const json serialized = custom;
  EXPECT_EQ(serialized.at("type"), "custom_tool_call");
  EXPECT_EQ(serialized.at("input"), kPatch);
  EXPECT_FALSE(serialized.contains("arguments"));
  EXPECT_FALSE(serialized.contains("status"));
}

TEST(ResponsesToolCallEmissionTest, SimultaneousCallsDoNotCrossAssociate) {
  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>("call_fn", "bash", R"({"command":"ls"})"));
  items.push_back(std::make_unique<ToolCallItem>(
      "call_custom", "apply_patch", kPatch, false, ToolKind::kCustom));

  auto [output, output_text] =
      ResponseConverter::FromSessionResponse(ResponseWithCalls(std::move(items)));

  ASSERT_EQ(output.size(), 2u);

  const auto& function = std::get<FunctionCallOutputItem>(output[0]);
  EXPECT_EQ(function.call_id, "call_fn");
  EXPECT_EQ(function.arguments, R"({"command":"ls"})");

  const auto& custom = std::get<CustomToolCallOutputItem>(output[1]);
  EXPECT_EQ(custom.call_id, "call_custom");
  EXPECT_EQ(custom.input, kPatch);
  EXPECT_NE(function.id, custom.id);
}

TEST(ResponsesToolCallEmissionTest, UndeclaredNameIsReportedAsAFunctionCall) {
  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>("call_1", "mystery", R"({"a":1})"));

  auto [output, output_text] =
      ResponseConverter::FromSessionResponse(ResponseWithCalls(std::move(items)));

  ASSERT_EQ(output.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<FunctionCallOutputItem>(output[0]));
}

TEST(ResponsesToolCallEmissionTest, CustomCallStreamsTheCompleteInputLifecycle) {
  ToolCallItem call("call_2", "apply_patch", kPatch, false, ToolKind::kCustom);
  int sequence_number = 5;

  auto output = ResponseConverter::BuildToolCallStreamOutput(call, 2, sequence_number);

  ASSERT_EQ(output.events.size(), 4u);
  EXPECT_EQ(sequence_number, 9);

  const auto& completed = std::get<CustomToolCallOutputItem>(output.completed_item);
  EXPECT_EQ(completed.call_id, "call_2");
  EXPECT_EQ(completed.input, kPatch);

  const auto& added = output.events[0];
  EXPECT_EQ(added.type, StreamEventType::kOutputItemAdded);
  EXPECT_EQ(added.output_index, 2);
  ASSERT_TRUE(added.item.has_value());
  const auto& added_item = std::get<CustomToolCallOutputItem>(*added.item);
  EXPECT_EQ(added_item.id, completed.id);
  EXPECT_TRUE(added_item.input.empty()) << "the announced item carries no payload; the deltas deliver it";

  const json delta = output.events[1];
  EXPECT_EQ(delta, json({{"type", "response.custom_tool_call_input.delta"},
                         {"sequence_number", 6},
                         {"output_index", 2},
                         {"item_id", completed.id},
                         {"delta", kPatch}}));

  const json done = output.events[2];
  EXPECT_EQ(done, json({{"type", "response.custom_tool_call_input.done"},
                        {"sequence_number", 7},
                        {"output_index", 2},
                        {"item_id", completed.id},
                        {"input", kPatch}}));

  const json item_done = output.events[3];
  EXPECT_EQ(item_done,
            json({{"type", "response.output_item.done"},
                  {"sequence_number", 8},
                  {"output_index", 2},
                  {"item",
                   {{"type", "custom_tool_call"},
                    {"id", completed.id},
                    {"call_id", "call_2"},
                    {"name", "apply_patch"},
                    {"input", kPatch}}}}));
}

TEST(ResponsesToolCallEmissionTest, StreamedAndFinalItemsAgreeOnCallIdAndInput) {
  ToolCallItem call("call_2", "apply_patch", kPatch, false, ToolKind::kCustom);

  int sequence_number = 0;
  auto streamed = ResponseConverter::BuildToolCallStreamOutput(call, 0, sequence_number);

  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>(
      "call_2", "apply_patch", kPatch, false, ToolKind::kCustom));
  auto [output, output_text] =
      ResponseConverter::FromSessionResponse(ResponseWithCalls(std::move(items)));

  const auto& streamed_item = std::get<CustomToolCallOutputItem>(streamed.completed_item);
  const auto& final_item = std::get<CustomToolCallOutputItem>(output.at(0));

  EXPECT_EQ(streamed_item.call_id, "call_2");
  EXPECT_EQ(streamed_item.call_id, final_item.call_id);
  EXPECT_EQ(streamed_item.name, final_item.name);
  EXPECT_EQ(streamed_item.input, final_item.input);
}

TEST(ResponsesToolCallEmissionTest, FunctionCallLifecycleIsUnchanged) {
  ToolCallItem call("call_1", "bash", R"({"command":"ls"})");
  int sequence_number = 0;

  auto output = ResponseConverter::BuildToolCallStreamOutput(call, 0, sequence_number);

  const json delta = output.events[1];
  EXPECT_EQ(delta.at("type"), "response.function_call_arguments.delta");
  EXPECT_EQ(delta.at("delta"), R"({"command":"ls"})");

  const json done = output.events[2];
  EXPECT_EQ(done.at("type"), "response.function_call_arguments.done");
  EXPECT_EQ(done.at("arguments"), R"({"command":"ls"})");
  EXPECT_FALSE(done.contains("input"));

  EXPECT_TRUE(std::holds_alternative<FunctionCallOutputItem>(output.completed_item));
}

TEST(ResponsesToolCallEmissionTest, CompletedResponseCarriesTheCustomCallForTheNextTurn) {
  auto params = ParamsWithTools(R"([{"type": "custom", "name": "apply_patch"}])");

  std::vector<std::unique_ptr<Item>> items;
  items.push_back(std::make_unique<ToolCallItem>(
      "call_2", "apply_patch", kPatch, false, ToolKind::kCustom));
  auto [output, output_text] =
      ResponseConverter::FromSessionResponse(ResponseWithCalls(std::move(items)));

  auto response = ResponseConverter::BuildResponseObject("resp_1", 10, "m", params, std::move(output), output_text,
                                                         TokenUsage{});
  const json stored = response;

  ASSERT_EQ(stored.at("output").size(), 1u);
  EXPECT_EQ(stored.at("output").at(0).at("type"), "custom_tool_call");
  EXPECT_EQ(stored.at("output").at(0).at("call_id"), "call_2");
  EXPECT_EQ(stored.at("output").at(0).at("input"), kPatch);
  EXPECT_FALSE(stored.at("output").at(0).contains("status"));

  // Feeding the stored output back as previous context reproduces the same call, still correlated with the result
  // the client sends for it. The stored output is that hop's own output; the result answering it belongs to the next
  // request's input, which is how a client actually continues a stored chain.
  const ResponseChainContext previous_context{ResponseChainHop{json::array(), stored.at("output")}};

  auto next = ParamsWithInput(R"([
    {"type": "custom_tool_call_output", "call_id": "call_2", "output": "applied"},
    {"role": "user", "content": [{"type": "input_text", "text": "continue"}]}
  ])");

  Request continued = ResponseConverter::ToSessionRequest(next, &previous_context);

  ASSERT_EQ(continued.items.size(), 3u);
  const auto& replayed_call = static_cast<const ToolCallItem&>(*continued.items[0]);
  EXPECT_EQ(replayed_call.arguments, kPatch) << "the raw payload survives the store round trip byte for byte";
  EXPECT_TRUE(replayed_call.replayed_from_store);
  EXPECT_EQ(replayed_call.replayed_kind, ToolKind::kCustom)
      << "the stored call states its own kind on replay";
  EXPECT_EQ(static_cast<const ToolResultItem&>(*continued.items[1]).call_id, "call_2");
}
