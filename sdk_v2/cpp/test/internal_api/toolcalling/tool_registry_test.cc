// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for the session tool registry: kind-aware registration, name uniqueness, custom-tool schema
// synthesis, snapshot consistency under concurrent mutation, and the unwrapping of a generated
// custom call's raw payload.
//
#include "exception.h"
#include "inferencing/session/tool_registry.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <thread>

using fl::ExtractCustomToolInput;
using fl::ToolDefinition;
using fl::ToolKind;
using fl::ToolRegistry;

namespace {

/// The exact bytes a custom tool's synthesized schema must have. Spelled out independently of the
/// constant the implementation uses so a change to either side has to be deliberate.
constexpr const char* kExpectedCustomSchema =
    R"({"type":"object","properties":{"input":{"type":"string"}},"required":["input"],"additionalProperties":false})";

ToolDefinition Function(std::string name, std::string schema = "{\"type\":\"object\"}") {
  return ToolDefinition{std::move(name), "description", std::move(schema), ToolKind::kFunction};
}

ToolDefinition Custom(std::string name) {
  return ToolDefinition{std::move(name), "description", "", ToolKind::kCustom};
}

/// Look a definition up in a snapshot. Generation resolves kinds this way too — the registry hands
/// out snapshots and is never queried by name.
std::optional<ToolDefinition> Find(const ToolRegistry& registry, const std::string& name) {
  for (const auto& td : registry.Definitions()) {
    if (!td.name.empty() && td.name == name) {
      return td;
    }
  }

  return std::nullopt;
}

/// The kind registered under `name`, defaulting to kFunction for names the registry does not know.
ToolKind KindOf(const ToolRegistry& registry, const std::string& name) {
  auto definition = Find(registry, name);
  return definition ? definition->kind : ToolKind::kFunction;
}

/// Assert that `expression` throws fl::Exception with FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT.
#define EXPECT_INVALID_ARGUMENT(expression)                                    \
  do {                                                                         \
    try {                                                                      \
      expression;                                                              \
      FAIL() << "expected fl::Exception from: " #expression;                   \
    } catch (const fl::Exception& ex) {                                        \
      EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) << ex.what(); \
    }                                                                          \
  } while (false)

}  // namespace

// ========================================================================
// Function tools — existing semantics must not change
// ========================================================================

TEST(ToolRegistryTest, RegistersFunctionToolVerbatim) {
  ToolRegistry registry;
  registry.Add(Function("get_weather", R"({"type":"object","properties":{"city":{"type":"string"}}})"));

  ASSERT_EQ(registry.Definitions().size(), 1u);
  auto definition = Find(registry, "get_weather");
  ASSERT_TRUE(definition.has_value());
  EXPECT_EQ(definition->name, "get_weather");
  EXPECT_EQ(definition->description, "description");
  EXPECT_EQ(definition->json_schema, R"({"type":"object","properties":{"city":{"type":"string"}}})");
  EXPECT_EQ(definition->kind, ToolKind::kFunction);
  EXPECT_EQ(KindOf(registry, "get_weather"), ToolKind::kFunction);
}

TEST(ToolRegistryTest, RejectsFunctionToolWithSchemaThatIsNotJson) {
  ToolRegistry registry;
  EXPECT_INVALID_ARGUMENT(registry.Add(Function("broken", "{not json")));
  EXPECT_TRUE(registry.Definitions().empty());
}

TEST(ToolRegistryTest, RejectsFunctionToolWithEmptySchema) {
  // An empty string is not valid JSON text, so a function tool's schema stays required.
  ToolRegistry registry;
  EXPECT_INVALID_ARGUMENT(registry.Add(Function("no_schema", "")));
  EXPECT_TRUE(registry.Definitions().empty());
}

TEST(ToolRegistryTest, AcceptsAnyValidJsonAsFunctionSchema) {
  // The contract is "valid JSON text", not "valid JSON Schema" — no stricter validation is applied.
  ToolRegistry registry;
  registry.Add(Function("array_schema", "[]"));
  registry.Add(Function("string_schema", "\"anything\""));
  registry.Add(Function("number_schema", "42"));
  registry.Add(Function("null_schema", "null"));

  EXPECT_EQ(registry.Definitions().size(), 4u);
  EXPECT_EQ(Find(registry, "number_schema")->json_schema, "42");
}

TEST(ToolRegistryTest, AllowsUnnamedPreSerializedDefinitions) {
  // The converter paths register a whole pre-serialized tools array under an empty name. Those are
  // not registrable tools: uniqueness does not apply and they are not findable.
  ToolRegistry registry;
  registry.Add(ToolDefinition{"", "", R"([{"type":"function"}])", ToolKind::kFunction});
  registry.Add(ToolDefinition{"", "", R"([{"type":"function"}])", ToolKind::kFunction});

  EXPECT_EQ(registry.Definitions().size(), 2u);
  EXPECT_FALSE(Find(registry, "").has_value());
}

// ========================================================================
// Custom tools
// ========================================================================

TEST(ToolRegistryTest, SynthesizesExactCustomToolSchema) {
  ToolRegistry registry;
  registry.Add(Custom("apply_patch"));

  auto definition = Find(registry, "apply_patch");
  ASSERT_TRUE(definition.has_value());
  EXPECT_EQ(definition->kind, ToolKind::kCustom);
  EXPECT_EQ(definition->json_schema, kExpectedCustomSchema);
  EXPECT_EQ(std::string(fl::kCustomToolInputSchema), kExpectedCustomSchema);
  EXPECT_EQ(KindOf(registry, "apply_patch"), ToolKind::kCustom);
}

TEST(ToolRegistryTest, RejectsCustomToolThatSuppliesASchema) {
  ToolRegistry registry;
  EXPECT_INVALID_ARGUMENT(registry.Add(ToolDefinition{"custom", "d", "{}", ToolKind::kCustom}));
  EXPECT_INVALID_ARGUMENT(
      registry.Add(ToolDefinition{"custom", "d", fl::kCustomToolInputSchema, ToolKind::kCustom}));
  EXPECT_INVALID_ARGUMENT(registry.Add(ToolDefinition{"custom", "d", " ", ToolKind::kCustom}));
  EXPECT_TRUE(registry.Definitions().empty());
}

TEST(ToolRegistryTest, RejectsUnnamedCustomTool) {
  ToolRegistry registry;
  EXPECT_INVALID_ARGUMENT(registry.Add(Custom("")));
  EXPECT_TRUE(registry.Definitions().empty());
}

// ========================================================================
// Uniqueness, case sensitivity, removal
// ========================================================================

TEST(ToolRegistryTest, RejectsDuplicateNameWithinAKind) {
  ToolRegistry registry;
  registry.Add(Function("tool"));
  EXPECT_INVALID_ARGUMENT(registry.Add(Function("tool")));
  EXPECT_EQ(registry.Definitions().size(), 1u);
}

TEST(ToolRegistryTest, RejectsDuplicateNameAcrossKinds) {
  ToolRegistry function_first;
  function_first.Add(Function("tool"));
  EXPECT_INVALID_ARGUMENT(function_first.Add(Custom("tool")));
  ASSERT_EQ(function_first.Definitions().size(), 1u);
  EXPECT_EQ(KindOf(function_first, "tool"), ToolKind::kFunction);

  ToolRegistry custom_first;
  custom_first.Add(Custom("tool"));
  EXPECT_INVALID_ARGUMENT(custom_first.Add(Function("tool")));
  ASSERT_EQ(custom_first.Definitions().size(), 1u);
  EXPECT_EQ(KindOf(custom_first, "tool"), ToolKind::kCustom);
}

TEST(ToolRegistryTest, NamesAreCaseSensitive) {
  ToolRegistry registry;
  registry.Add(Function("Tool"));
  registry.Add(Custom("tool"));
  registry.Add(Custom("TOOL"));

  EXPECT_EQ(registry.Definitions().size(), 3u);
  EXPECT_EQ(KindOf(registry, "Tool"), ToolKind::kFunction);
  EXPECT_EQ(KindOf(registry, "tool"), ToolKind::kCustom);
  EXPECT_EQ(KindOf(registry, "TOOL"), ToolKind::kCustom);
  EXPECT_FALSE(Find(registry, "ToOl").has_value());
}

TEST(ToolRegistryTest, RemoveThenReAddSucceedsAndMayChangeKind) {
  ToolRegistry registry;
  registry.Add(Function("tool"));

  EXPECT_TRUE(registry.Remove("tool"));
  EXPECT_FALSE(Find(registry, "tool").has_value());
  EXPECT_FALSE(registry.Remove("tool"));

  registry.Add(Custom("tool"));
  ASSERT_EQ(registry.Definitions().size(), 1u);
  EXPECT_EQ(KindOf(registry, "tool"), ToolKind::kCustom);
}

TEST(ToolRegistryTest, MovingARegistryKeepsBothObjectsUsable) {
  ToolRegistry source;
  source.Add(Function("before"));

  ToolRegistry destination(std::move(source));
  source.Add(Function("after"));

  EXPECT_TRUE(Find(destination, "before").has_value());
  EXPECT_FALSE(Find(destination, "after").has_value());
  EXPECT_TRUE(Find(source, "after").has_value());
}

TEST(ToolRegistryTest, RemoveKeepsTheRemainingEntriesResolvable) {
  // Removal shifts positions, so the surviving entries must still resolve to their own definitions
  // and the freed name must actually be free.
  ToolRegistry registry;
  registry.Add(Function("a"));
  registry.Add(Custom("b"));
  registry.Add(Custom("c"));

  ASSERT_TRUE(registry.Remove("b"));
  ASSERT_EQ(registry.Definitions().size(), 2u);
  ASSERT_TRUE(Find(registry, "a").has_value());
  EXPECT_EQ(Find(registry, "a")->name, "a");
  EXPECT_EQ(KindOf(registry, "c"), ToolKind::kCustom);

  EXPECT_INVALID_ARGUMENT(registry.Add(Function("c")));
  registry.Add(Function("b"));
  EXPECT_EQ(KindOf(registry, "b"), ToolKind::kFunction);
}

TEST(ToolRegistryTest, ClearDropsEverythingAndFreesTheNames) {
  ToolRegistry registry;
  registry.Add(Function("a"));
  registry.Add(Custom("b"));

  registry.Clear();
  EXPECT_TRUE(registry.Definitions().empty());

  registry.Add(Custom("a"));
  registry.Add(Function("b"));
  EXPECT_EQ(registry.Definitions().size(), 2u);
}

TEST(ToolRegistryTest, PreservesRegistrationOrder) {
  ToolRegistry registry;
  registry.Add(Function("first"));
  registry.Add(Custom("second"));
  registry.Add(Function("third"));

  ASSERT_EQ(registry.Definitions().size(), 3u);
  EXPECT_EQ(registry.Definitions()[0].name, "first");
  EXPECT_EQ(registry.Definitions()[1].name, "second");
  EXPECT_EQ(registry.Definitions()[2].name, "third");
}

// ========================================================================
// Unnamed entries, removal safety
// ========================================================================

TEST(ToolRegistryTest, RemoveWithAnEmptyNameRemovesNothing) {
  // An unnamed entry is a request-scoped pre-serialized tools payload, not a registered tool.
  // Matching it positionally would let RemoveToolDefinition("") tear out the tools the in-flight
  // request is being generated against.
  ToolRegistry registry;
  registry.Add(ToolDefinition{"", "", R"([{"type":"function"}])", ToolKind::kFunction});
  registry.Add(Function("named"));

  EXPECT_FALSE(registry.Remove(""));
  EXPECT_EQ(registry.Definitions().size(), 2u);
  EXPECT_EQ(registry.Definitions()[0].json_schema, R"([{"type":"function"}])");

  EXPECT_TRUE(registry.Remove("named"));
  EXPECT_EQ(registry.Definitions().size(), 1u);
  EXPECT_EQ(registry.Definitions()[0].name, "");
}

// ========================================================================
// Concurrency — registration must not race an in-flight request's reads
// ========================================================================

TEST(ToolRegistryTest, SnapshotsAreConsistentWhileToolsAreRegisteredAndRemoved) {
  // Models the real hazard: a caller adds and removes tools from one thread while a request reads
  // the registry from another. Readers take snapshots, so they must always observe a whole,
  // self-consistent registry — never a resized vector, a stale index, or a torn definition.
  ToolRegistry registry;
  registry.Add(Custom("stable_custom"));
  registry.Add(Function("stable_function"));

  std::atomic<bool> stop{false};
  std::atomic<int> reads{0};

  std::thread writer([&] {
    for (int i = 0; i < 2000; ++i) {
      registry.Add(Custom("churn_" + std::to_string(i)));
      EXPECT_TRUE(registry.Remove("churn_" + std::to_string(i)));
    }
    stop = true;
  });

  std::thread reader([&] {
    while (!stop) {
      auto definitions = registry.Definitions();

      // The two tools that are never churned must be present, intact, and of the right kind in
      // every snapshot, regardless of what the writer is doing.
      EXPECT_GE(definitions.size(), 2u);
      std::set<std::string> names;
      for (const auto& definition : definitions) {
        EXPECT_TRUE(names.insert(definition.name).second) << definition.name;
      }

      const auto custom = std::find_if(definitions.begin(), definitions.end(),
                                       [](const ToolDefinition& definition) {
                                         return definition.name == "stable_custom";
                                       });
      ASSERT_NE(custom, definitions.end());
      EXPECT_EQ(custom->kind, ToolKind::kCustom);
      EXPECT_EQ(custom->json_schema, kExpectedCustomSchema);

      const auto function = std::find_if(definitions.begin(), definitions.end(),
                                         [](const ToolDefinition& definition) {
                                           return definition.name == "stable_function";
                                         });
      ASSERT_NE(function, definitions.end());
      EXPECT_EQ(function->kind, ToolKind::kFunction);

      for (const auto& definition : definitions) {
        EXPECT_FALSE(definition.json_schema.empty());
      }

      ++reads;
    }
  });

  writer.join();
  reader.join();

  EXPECT_GT(reads.load(), 0);
  EXPECT_EQ(registry.Definitions().size(), 2u);
}

TEST(ToolRegistryTest, ASnapshotIsUnaffectedByALaterKindFlip) {
  // A turn resolves its replayed calls, its prompt and its produced calls against one snapshot taken
  // when the turn started. This pins the property that makes that safe: re-registering a name under
  // another kind cannot reach back into a snapshot already handed out, so a turn can never read its
  // own output with a tool set that never shaped its prompt.
  ToolRegistry registry;
  registry.Add(Custom("run"));

  const auto snapshot = registry.Definitions();

  EXPECT_TRUE(registry.Remove("run"));
  registry.Add(Function("run"));

  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(snapshot[0].name, "run");
  EXPECT_EQ(snapshot[0].kind, ToolKind::kCustom);
  EXPECT_EQ(snapshot[0].json_schema, kExpectedCustomSchema);

  // The registry itself has moved on; only the snapshot is frozen.
  EXPECT_EQ(KindOf(registry, "run"), ToolKind::kFunction);
}

// ========================================================================
// ExtractCustomToolInput
//
// Exactly one shape is unwrapped: {"input": "<string>"} and nothing else. Every other shape is a
// model defect, and the raw bytes are returned untouched so no evidence of it is lost.
// ========================================================================

TEST(ExtractCustomToolInputTest, UnwrapsTheSynthesizedWrapperVerbatim) {
  // The payload comes back byte for byte: JSON escapes resolved, and line endings, tabs, trailing
  // spaces, Unicode and JSON-looking text all intact rather than re-serialized.
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":"hello"})"), "hello");
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":""})"), "");
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":"line one\r\nline two"})"), "line one\r\nline two");
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":"col1\tcol2  "})"), "col1\tcol2  ");
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":"héllo 🌍 \u00e9"})"), "héllo 🌍 é");
  const auto escaped_quote = ExtractCustomToolInput(R"json({"input":"quote \" backslash \\ done"})json");
  const auto json_looking_text = ExtractCustomToolInput(R"json({"input":"{\"a\": 1}"})json");
  EXPECT_EQ(escaped_quote, "quote \" backslash \\ done");
  EXPECT_EQ(json_looking_text, R"json({"a": 1})json");

  const std::string patch =
      "*** Begin Patch\n*** Update File: a.txt\n@@\n-old line\t\n+new line \n*** End Patch";
  EXPECT_EQ(ExtractCustomToolInput(nlohmann::json{{"input", patch}}.dump()), patch);
}

TEST(ExtractCustomToolInputTest, PassesEveryOtherShapeThroughUntouched) {
  // Anything but a lone string `input` is a model defect. Partially unwrapping or re-serializing it
  // would destroy the only faithful record of what the model actually produced.

  // Not JSON at all.
  EXPECT_EQ(ExtractCustomToolInput("just text"), "just text");
  EXPECT_EQ(ExtractCustomToolInput(""), "");
  EXPECT_EQ(ExtractCustomToolInput("{unterminated"), "{unterminated");
  EXPECT_EQ(ExtractCustomToolInput(" \t{ \"input\" : \"payload\" "), " \t{ \"input\" : \"payload\" ");

  // JSON, but not an object: unwrapping would drop the quoting the model chose to emit.
  EXPECT_EQ(ExtractCustomToolInput(R"("just text")"), R"("just text")");
  EXPECT_EQ(ExtractCustomToolInput("42"), "42");
  EXPECT_EQ(ExtractCustomToolInput(R"(["input"])"), R"(["input"])");

  // An object without `input`, or with siblings alongside it.
  EXPECT_EQ(ExtractCustomToolInput("{}"), "{}");
  EXPECT_EQ(ExtractCustomToolInput(R"({"other":"value"})"), R"({"other":"value"})");
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":"payload","extra":1})"), R"({"input":"payload","extra":1})");
  EXPECT_EQ(ExtractCustomToolInput(R"( { "extra" : 1, "input" : "payload" } )"),
            R"( { "extra" : 1, "input" : "payload" } )");
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":"first","input":"second"})"),
            R"({"input":"first","input":"second"})");

  // `input` present but not a string: serializing it would lose the distinction between the string
  // "42" and the number 42, and between a nested object and the text that produced it.
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":42})"), R"({"input":42})");
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":null})"), R"({"input":null})");
  EXPECT_EQ(ExtractCustomToolInput(R"({"input":{"a":1}})"), R"({"input":{"a":1}})");
}
