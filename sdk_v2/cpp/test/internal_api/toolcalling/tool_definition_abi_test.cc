// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// ABI tests for flToolDefinition ingestion (fl::ToolDefinitionFromC).
//
// The point of these tests is the version gate: a consumer compiled against the version 1 header
// allocates a struct that ends at `json_schema`, so the implementation must decide what it may read
// from `version` alone, before touching the tail. LegacyToolDefinitionV1 below is a frozen copy of
// that older struct, and LegacyDefinition places one at the very end of a writable page backed by
// an inaccessible guard page. A read of even one byte past the struct therefore faults, which makes
// the "must not read the tail" contract enforceable on every platform rather than only under
// AddressSanitizer. These tests are also worth running under the sanitizer pass:
//
//   python sdk_v2/cpp/scripts/run_sanitizer_tests.py --unit-only --gtest_filter "ToolDefinitionAbiTest.*"
//
#include "exception.h"
#include "inferencing/session/tool_registry.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using fl::ToolDefinition;
using fl::ToolDefinitionFromC;
using fl::ToolKind;

namespace {

/// Byte-for-byte copy of flToolDefinition as published in API version 1. Frozen on purpose:
/// if the current struct ever reorders or resizes its first four fields, the static_asserts below
/// fail and the ABI break is caught at compile time.
struct LegacyToolDefinitionV1 {
  uint32_t version;
  const char* name;
  const char* description;
  const char* json_schema;
};

static_assert(sizeof(LegacyToolDefinitionV1) < sizeof(flToolDefinition),
              "flToolDefinition must have grown a tail beyond the version 1 prefix");
static_assert(offsetof(LegacyToolDefinitionV1, version) == offsetof(flToolDefinition, version),
              "flToolDefinition::version moved — that is an ABI break");
static_assert(offsetof(LegacyToolDefinitionV1, name) == offsetof(flToolDefinition, name),
              "flToolDefinition::name moved — that is an ABI break");
static_assert(offsetof(LegacyToolDefinitionV1, description) == offsetof(flToolDefinition, description),
              "flToolDefinition::description moved — that is an ABI break");
static_assert(offsetof(LegacyToolDefinitionV1, json_schema) == offsetof(flToolDefinition, json_schema),
              "flToolDefinition::json_schema moved — that is an ABI break");
static_assert(sizeof(LegacyToolDefinitionV1) == offsetof(flToolDefinition, kind),
              "flToolDefinition::kind must be appended directly after the version 1 prefix");
static_assert(sizeof(flToolKind) == sizeof(uint32_t), "flToolKind must be a fixed-width 32-bit ABI type");
static_assert(std::is_same<flToolKind, uint32_t>::value,
              "flToolKind must stay a fixed-width integer — an enum's width is implementation-defined in C");

size_t PageSize() {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  return static_cast<size_t>(info.dwPageSize);
#else
  return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

/// An old consumer's struct, placed so that it ends exactly at a guard page. Reading `kind` off the
/// result reads past the end of the accessible page and faults the test process — the same failure
/// an old caller would suffer in production, made deterministic.
class LegacyDefinition {
 public:
  LegacyDefinition(uint32_t version, const char* name, const char* description, const char* json_schema) {
    const size_t page_size = PageSize();
    region_ = Reserve(page_size);
    Guard(static_cast<char*>(region_) + page_size, page_size);

    // Butt the struct up against the guard page, keeping its natural alignment.
    size_t offset = page_size - sizeof(LegacyToolDefinitionV1);
    offset -= offset % alignof(LegacyToolDefinitionV1);
    storage_ = reinterpret_cast<LegacyToolDefinitionV1*>(static_cast<char*>(region_) + offset);

    storage_->version = version;
    storage_->name = name;
    storage_->description = description;
    storage_->json_schema = json_schema;
  }

  ~LegacyDefinition() { Release(region_, PageSize()); }

  LegacyDefinition(const LegacyDefinition&) = delete;
  LegacyDefinition& operator=(const LegacyDefinition&) = delete;

  const flToolDefinition& AsCurrent() const {
    return *reinterpret_cast<const flToolDefinition*>(storage_);
  }

 private:
  static void* Reserve(size_t page_size) {
#if defined(_WIN32)
    void* region = VirtualAlloc(nullptr, page_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    EXPECT_NE(region, nullptr);
    return region;
#else
    void* region = mmap(nullptr, page_size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(region, MAP_FAILED);
    return region;
#endif
  }

  static void Guard(void* page, size_t page_size) {
#if defined(_WIN32)
    DWORD previous = 0;
    EXPECT_NE(VirtualProtect(page, page_size, PAGE_NOACCESS, &previous), 0);
#else
    EXPECT_EQ(mprotect(page, page_size, PROT_NONE), 0);
#endif
  }

  static void Release(void* region, size_t page_size) {
#if defined(_WIN32)
    (void)page_size;
    VirtualFree(region, 0, MEM_RELEASE);
#else
    munmap(region, page_size * 2);
#endif
  }

  void* region_ = nullptr;
  LegacyToolDefinitionV1* storage_ = nullptr;
};

/// A definition built with the current struct. `kind` is filled with a non-zero garbage pattern so
/// a version that must not read the tail cannot accidentally agree with the expected result.
flToolDefinition CurrentDefinition(uint32_t version, const char* name, const char* description,
                                   const char* json_schema, flToolKind kind) {
  flToolDefinition definition{};
  definition.version = version;
  definition.name = name;
  definition.description = description;
  definition.json_schema = json_schema;
  definition.kind = kind;
  return definition;
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

constexpr flToolKind kGarbageKind = 0x5A5A5A5Au;

}  // namespace

// ========================================================================
// Version matrix
// ========================================================================

TEST(ToolDefinitionAbiTest, RejectsVersionZero) {
  LegacyDefinition legacy(0, "tool", "description", "{}");
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(legacy.AsCurrent()));
}

TEST(ToolDefinitionAbiTest, RejectsVersionZeroOnCurrentStruct) {
  auto definition = CurrentDefinition(0, "tool", "description", "{}", FOUNDRY_LOCAL_TOOL_KIND_CUSTOM);
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(definition));
}

TEST(ToolDefinitionAbiTest, RejectsFutureVersionBeforeReadingTheTail) {
  // The struct is only the old size, so a future version that reached `kind` would read out of
  // bounds. Rejection must happen on `version` alone.
  LegacyDefinition legacy(FOUNDRY_LOCAL_API_VERSION + 1, "tool", "description", "{}");
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(legacy.AsCurrent()));

  LegacyDefinition far_future(0xFFFFFFFFu, "tool", "description", "{}");
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(far_future.AsCurrent()));
}

TEST(ToolDefinitionAbiTest, VersionOneReadsOnlyThePrefixAndDefaultsToFunction) {
  LegacyDefinition legacy(1, "tool", "a description", R"({"type":"object"})");
  auto definition = ToolDefinitionFromC(legacy.AsCurrent());

  EXPECT_EQ(definition.name, "tool");
  EXPECT_EQ(definition.description, "a description");
  EXPECT_EQ(definition.json_schema, R"({"type":"object"})");
  EXPECT_EQ(definition.kind, ToolKind::kFunction);
}

TEST(ToolDefinitionAbiTest, LegacyVersionOnCurrentStructIgnoresTheTail) {
  // A new caller that still stamps an old version gets old behavior even though its struct really
  // does carry a `kind` — the stamped version, not the allocation size, is what the ABI promises.
  auto custom = CurrentDefinition(1, "tool", "d", "{}", FOUNDRY_LOCAL_TOOL_KIND_CUSTOM);
  EXPECT_EQ(ToolDefinitionFromC(custom).kind, ToolKind::kFunction);

  auto garbage = CurrentDefinition(1, "tool", "d", "{}", kGarbageKind);
  EXPECT_EQ(ToolDefinitionFromC(garbage).kind, ToolKind::kFunction);
}

TEST(ToolDefinitionAbiTest, CurrentVersionReadsTheKind) {
  auto function = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, "tool", "d", "{}",
                                    FOUNDRY_LOCAL_TOOL_KIND_FUNCTION);
  EXPECT_EQ(ToolDefinitionFromC(function).kind, ToolKind::kFunction);

  auto custom = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, "tool", "d", nullptr,
                                  FOUNDRY_LOCAL_TOOL_KIND_CUSTOM);
  EXPECT_EQ(ToolDefinitionFromC(custom).kind, ToolKind::kCustom);
}

TEST(ToolDefinitionAbiTest, KindIsIntroducedAtVersionTwo) {
  // Pins the version the tail was added in: bumping FOUNDRY_LOCAL_API_VERSION must not move it.
  auto custom = CurrentDefinition(2, "tool", "d", "", FOUNDRY_LOCAL_TOOL_KIND_CUSTOM);
  EXPECT_EQ(ToolDefinitionFromC(custom).kind, ToolKind::kCustom);
  EXPECT_GE(FOUNDRY_LOCAL_API_VERSION, 2);
}

// ========================================================================
// Kind validation
// ========================================================================

TEST(ToolDefinitionAbiTest, RejectsUnknownKind) {
  for (flToolKind kind : {2u, 0x5A5A5A5Au, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
    auto definition = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, "tool", "d", "{}", kind);
    EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(definition));
  }
}

// ========================================================================
// Null handling
// ========================================================================

TEST(ToolDefinitionAbiTest, RejectsNullNameOrDescription) {
  auto no_name = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, nullptr, "d", "{}",
                                   FOUNDRY_LOCAL_TOOL_KIND_FUNCTION);
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(no_name));

  auto no_description = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, "tool", nullptr, "{}",
                                          FOUNDRY_LOCAL_TOOL_KIND_FUNCTION);
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(no_description));
}

TEST(ToolDefinitionAbiTest, VersionOneAcceptsEmptyFunctionNameForReleasedPreSerializedCompatibility) {
  LegacyDefinition legacy(1, "", "d", "{}");
  auto converted = ToolDefinitionFromC(legacy.AsCurrent());
  EXPECT_EQ(converted.name, "");
  EXPECT_EQ(converted.json_schema, "{}");
  EXPECT_EQ(converted.kind, ToolKind::kFunction);
}

TEST(ToolDefinitionAbiTest, VersionTwoRejectsEmptyPublicToolNames) {
  auto current = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, "", "d", "{}",
                                   FOUNDRY_LOCAL_TOOL_KIND_FUNCTION);
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(current));

  auto custom = CurrentDefinition(2, "", "d", nullptr, FOUNDRY_LOCAL_TOOL_KIND_CUSTOM);
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(custom));
}

TEST(ToolDefinitionAbiTest, RejectsNullSchemaForAFunctionToolAndAcceptsItForACustomTool) {
  auto function = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, "tool", "d", nullptr,
                                    FOUNDRY_LOCAL_TOOL_KIND_FUNCTION);
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(function));

  LegacyDefinition legacy(1, "tool", "d", nullptr);
  EXPECT_INVALID_ARGUMENT(ToolDefinitionFromC(legacy.AsCurrent()));

  auto custom = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, "tool", "d", nullptr,
                                  FOUNDRY_LOCAL_TOOL_KIND_CUSTOM);
  EXPECT_EQ(ToolDefinitionFromC(custom).json_schema, "");
}

// ========================================================================
// Ownership — the caller's buffers are copied, never retained
// ========================================================================

TEST(ToolDefinitionAbiTest, CopiesTheCallerBuffers) {
  std::vector<char> name{'t', 'o', 'o', 'l', '\0'};
  std::vector<char> description{'d', '\0'};
  std::vector<char> schema{'{', '}', '\0'};

  auto definition = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, name.data(), description.data(),
                                      schema.data(), FOUNDRY_LOCAL_TOOL_KIND_FUNCTION);
  auto converted = ToolDefinitionFromC(definition);

  // Scribble over the caller's buffers, then free them entirely.
  std::memset(name.data(), 'X', name.size() - 1);
  std::memset(description.data(), 'X', description.size() - 1);
  std::memset(schema.data(), ' ', schema.size() - 1);
  name.clear();
  name.shrink_to_fit();
  description.clear();
  description.shrink_to_fit();
  schema.clear();
  schema.shrink_to_fit();

  EXPECT_EQ(converted.name, "tool");
  EXPECT_EQ(converted.description, "d");
  EXPECT_EQ(converted.json_schema, "{}");
}

TEST(ToolDefinitionAbiTest, RegistryKeepsItsOwnCopyOfTheCallerBuffers) {
  fl::ToolRegistry registry;
  {
    std::string name = "apply_patch";
    std::string description = "applies a patch";
    auto definition = CurrentDefinition(FOUNDRY_LOCAL_API_VERSION, name.c_str(), description.c_str(),
                                        nullptr, FOUNDRY_LOCAL_TOOL_KIND_CUSTOM);
    registry.Add(ToolDefinitionFromC(definition));
  }

  auto stored = registry.Definitions();
  ASSERT_EQ(stored.size(), 1u);
  EXPECT_EQ(stored[0].name, "apply_patch");
  EXPECT_EQ(stored[0].description, "applies a patch");
  EXPECT_EQ(stored[0].kind, ToolKind::kCustom);
  EXPECT_EQ(stored[0].json_schema, fl::kCustomToolInputSchema);
}
