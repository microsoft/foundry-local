// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for the shared wire-to-core tool definition layer (contracts/tool_definitions.*) and for
// the property that matters most about it: every surface — the C ABI, Chat Completions and
// Responses — turns the same declared tool inventory into the same core definitions.
//
#include "contracts/tool_definitions.h"

#include "contracts/chat_completions.h"
#include "contracts/chat_completions_converter.h"
#include "exception.h"
#include "inferencing/generative/openresponses/response_converter.h"
#include "inferencing/session/tool_registry.h"
#include "internal_api/toolcalling/coding_agent_tools_fixture.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace fl;
using json = nlohmann::json;

namespace {

#define EXPECT_INVALID_ARGUMENT(expr)                                        \
  try {                                                                      \
    expr;                                                                    \
    ADD_FAILURE() << "expected fl::Exception from: " #expr;                  \
  } catch (const fl::Exception& ex) {                                        \
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) << ex.what(); \
  }

ChatCompletionRequest ChatRequestWithTools(const std::string& tools_array_json) {
  auto j = json::parse(R"({"model": "m", "messages": [{"role": "user", "content": "hi"}]})");
  j["tools"] = json::parse(tools_array_json);
  return j.get<ChatCompletionRequest>();
}

responses::ResponseCreateParams ResponsesParamsWithTools(const json& tools_array) {
  json j{{"model", "m"}, {"input", "hi"}};
  j["tools"] = tools_array;
  return j.get<responses::ResponseCreateParams>();
}

std::vector<std::string> NamesOf(const std::vector<ToolDefinition>& definitions) {
  std::vector<std::string> names;
  names.reserve(definitions.size());
  for (const auto& definition : definitions) {
    names.push_back(definition.name);
  }
  return names;
}

}  // namespace

// ========================================================================
// ParseCustomToolFormat — text-only provider transport
// ========================================================================

TEST(CustomToolFormatTest, AcceptsAbsentFormat) {
  const auto format = tools::ParseCustomToolFormat(json(), "apply_patch");
  EXPECT_EQ(format, json::parse(R"({"type":"text"})"));
}

TEST(CustomToolFormatTest, AcceptsTextFormat) {
  const auto format =
      tools::ParseCustomToolFormat(json::parse(R"({"type":"text"})"), "apply_patch");
  EXPECT_EQ(format, json::parse(R"({"type":"text"})"));
}

TEST(CustomToolFormatTest, RejectsGrammarFormats) {
  EXPECT_INVALID_ARGUMENT(tools::ParseCustomToolFormat(
      json::parse(R"({"type":"grammar","grammar":{"syntax":"lark","definition":"start: X"}})"),
      "apply_patch"));
  EXPECT_INVALID_ARGUMENT(tools::ParseCustomToolFormat(
      json::parse(R"({"type":"grammar","syntax":"lark","definition":"start: X"})"),
      "apply_patch"));
}

TEST(CustomToolFormatTest, RejectsUnknownFormatType) {
  EXPECT_INVALID_ARGUMENT(tools::ParseCustomToolFormat(
      json::parse(R"({"type":"binary"})"), "apply_patch"));
}

TEST(CustomToolFormatTest, RejectsNonObjectFormat) {
  EXPECT_INVALID_ARGUMENT(tools::ParseCustomToolFormat(json("text"), "apply_patch"));
  EXPECT_INVALID_ARGUMENT(tools::ParseCustomToolFormat(json::array({"text"}), "apply_patch"));
}

TEST(CustomToolFormatTest, RejectsFormatWithoutType) {
  EXPECT_INVALID_ARGUMENT(tools::ParseCustomToolFormat(json::object(), "apply_patch"));
  EXPECT_INVALID_ARGUMENT(
      tools::ParseCustomToolFormat(json::parse(R"({"type":7})"), "apply_patch"));
}

TEST(CustomToolFormatTest, RejectsTextFormatCarryingConflictingMembers) {
  EXPECT_INVALID_ARGUMENT(tools::ParseCustomToolFormat(
      json::parse(R"({"type":"text","grammar":{"syntax":"lark","definition":"start: X"}})"),
      "apply_patch"));
  EXPECT_INVALID_ARGUMENT(tools::ParseCustomToolFormat(
      json::parse(R"({"type":"text","syntax":"lark"})"), "apply_patch"));
}

TEST(CustomToolFormatTest, ErrorNamesTheOffendingTool) {
  try {
    tools::ParseCustomToolFormat(json::parse(R"({"type":"grammar"})"), "apply_patch");
    ADD_FAILURE() << "expected a rejection";
  } catch (const fl::Exception& ex) {
    EXPECT_NE(std::string(ex.what()).find("apply_patch"), std::string::npos) << ex.what();
  }
}

// ========================================================================
// MakeFunctionTool / MakeCustomTool
// ========================================================================

TEST(ToolDefinitionFactoryTest, FunctionToolKeepsItsSchema) {
  auto definition = tools::MakeFunctionTool("get_weather", "Get weather", R"({"type":"object"})");

  EXPECT_EQ(definition.name, "get_weather");
  EXPECT_EQ(definition.description, "Get weather");
  EXPECT_EQ(definition.json_schema, R"({"type":"object"})");
  EXPECT_EQ(definition.kind, ToolKind::kFunction);
}

TEST(ToolDefinitionFactoryTest, FunctionToolWithoutParametersGetsNeutralSchema) {
  // The registry requires a schema for every function tool; `{}` is the JSON Schema spelling of
  // "declares no parameters" and keeps a parameterless tool registrable.
  EXPECT_EQ(tools::MakeFunctionTool("ping", "", "").json_schema, "{}");
}

TEST(ToolDefinitionFactoryTest, WirePresenceIsPreservedForPromptProjection) {
  auto function = tools::MakeFunctionTool("ping", "", "", false, false);
  auto custom = tools::MakeCustomTool("apply_patch", "", false);

  EXPECT_FALSE(function.include_description_in_prompt);
  EXPECT_FALSE(function.include_parameters_in_prompt);
  EXPECT_FALSE(custom.include_description_in_prompt);
  EXPECT_TRUE(custom.include_parameters_in_prompt);
}

TEST(ToolDefinitionFactoryTest, StrictFalseIsPreservedByCoreFunctionDefinition) {
  const auto definition = tools::MakeFunctionTool("strict_fn", "", "{}", true, true, false);
  EXPECT_EQ(definition.strict, false);
}

TEST(ToolDefinitionFactoryTest, CustomToolCarriesNoSchema) {
  auto definition = tools::MakeCustomTool("apply_patch", "Apply a patch");

  EXPECT_EQ(definition.name, "apply_patch");
  EXPECT_EQ(definition.kind, ToolKind::kCustom);
  EXPECT_TRUE(definition.json_schema.empty()) << "the registry synthesizes the custom schema";
}

TEST(ToolDefinitionFactoryTest, RegisteredCustomToolGetsExactlyTheSynthesizedSchema) {
  ToolRegistry registry;
  registry.Add(tools::MakeCustomTool("apply_patch", "Apply a patch"));

  auto registered = registry.Definitions();
  ASSERT_EQ(registered.size(), 1u);
  EXPECT_EQ(registered[0].json_schema, kCustomToolInputSchema);
}

// ========================================================================
// NarrowToForcedTool / RetainAllowedTools
// ========================================================================

TEST(ToolDefinitionFilterTest, NarrowToForcedToolKeepsOnlyTheNamedToolOfThatKind) {
  std::vector<ToolDefinition> definitions{tools::MakeFunctionTool("a", "", "{}"),
                                          tools::MakeFunctionTool("b", "", "{}"),
                                          tools::MakeCustomTool("c", "")};

  tools::NarrowToForcedTool(definitions, "b", ToolKind::kFunction);

  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions[0].name, "b");
}

TEST(ToolDefinitionFilterTest, NarrowToForcedToolMatchesOnKindAsWellAsName) {
  // A function and a custom tool can never be swapped for one another, even under the same name.
  std::vector<ToolDefinition> definitions{tools::MakeFunctionTool("apply_patch", "", "{}")};

  EXPECT_THROW(tools::NarrowToForcedTool(definitions, "apply_patch", ToolKind::kCustom), fl::Exception);
}

TEST(ToolDefinitionFilterTest, NarrowToForcedToolRejectsUndeclaredTools) {
  std::vector<ToolDefinition> definitions{tools::MakeFunctionTool("a", "", "{}"),
                                          tools::MakeFunctionTool("b", "", "{}")};

  EXPECT_THROW(tools::NarrowToForcedTool(definitions, "never_declared", ToolKind::kFunction), fl::Exception);
}

TEST(ToolDefinitionFilterTest, RetainAllowedToolsFiltersCaseInsensitivelyAndKeepsOrder) {
  std::vector<ToolDefinition> definitions{tools::MakeFunctionTool("Alpha", "", "{}"),
                                          tools::MakeFunctionTool("beta", "", "{}"),
                                          tools::MakeCustomTool("Gamma", "")};

  tools::RetainAllowedTools(definitions, {"gamma", "ALPHA"});

  EXPECT_EQ(NamesOf(definitions), (std::vector<std::string>{"Alpha", "Gamma"}));
}

TEST(ToolDefinitionFilterTest, RetainAllowedToolsIgnoresDuplicateAndUnknownEntries) {
  std::vector<ToolDefinition> definitions{tools::MakeFunctionTool("a", "", "{}"),
                                          tools::MakeFunctionTool("b", "", "{}")};

  tools::RetainAllowedTools(definitions, {"a", "a", "a", "never_declared"});

  EXPECT_EQ(NamesOf(definitions), (std::vector<std::string>{"a"}));
}

TEST(ToolDefinitionFilterTest, RetainAllowedToolsWithAnEmptyListKeepsNothing) {
  std::vector<ToolDefinition> definitions{tools::MakeFunctionTool("a", "", "{}")};

  tools::RetainAllowedTools(definitions, {});

  EXPECT_TRUE(definitions.empty());
}

// ========================================================================
// Captured stock GHCP apply_patch declarations
// ========================================================================

TEST(CodingAgentToolDeclarationTest, RejectsCapturedChatGrammarDeclaration) {
  EXPECT_INVALID_ARGUMENT(ChatRequestWithTools(test::kCodingAgentChatToolsJson));
}

TEST(CodingAgentToolDeclarationTest, RejectsCapturedResponsesGrammarDeclaration) {
  EXPECT_INVALID_ARGUMENT(
      ResponsesParamsWithTools(json::parse(test::kCodingAgentResponsesToolsJson)));
}
