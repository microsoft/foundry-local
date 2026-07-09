# Foundry Local C++ SDK — Coding Conventions and Standards

> Adapted from the [ONNX Runtime Coding Conventions](https://github.com/microsoft/onnxruntime/blob/main/docs/Coding_Conventions_and_Standards.md).
> The [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) should be followed where possible.

## C++ Code Style

Google style from <https://google.github.io/styleguide/cppguide.html> with the following alterations:

### Line Length

* Maximum line length is **120** characters.

### Exceptions

* Allowed to throw fatal errors that are expected to result in a top-level handler catching them, logging them and terminating the program.

### Non-const References

* Allowed.
* Use a non-const reference for arguments that are modifiable but cannot be `nullptr` so the API clearly advertises the intent.
* Const correctness and usage of smart pointers (`shared_ptr` and `unique_ptr`) is expected. A non-const reference equates to *"this is a non-null object that you can change but are not being given ownership of."*

### `std::span` / `gsl::span`

* Prefer passing `std::span<const T>` by value as input arguments when passing const references to containers with contiguous storage (like `std::vector`). This allows the function to be container-independent and the argument to represent arbitrary memory spans or sub-spans.

```cpp
// Instead of
void foo(const std::vector<int64_t>&);

// Use — accepts std::vector, std::array, raw spans, etc.
void foo(std::span<const int64_t>);
```

* Prefer returning `std::span<const T>` by value instead of a const reference to a contiguous member container. The size is included in the span.

```cpp
// Instead of
const std::vector<int64_t>& foo();

// Return a span by value
std::span<const int64_t> foo();
```

### `std::string_view`

* Prefer passing `std::string_view` by value instead of `const std::string&`.
* Ensure the lifespan of the `std::string` instance outlives the corresponding `std::string_view` instance.

### `using namespace`

* Permitted with limited scope.
* Follow the C++ Core Guidelines:
  * [SF.6: Use `using namespace` directives for transition, for foundation libraries (such as `std`), or within a local scope only](https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md#Rs-using)
  * [SF.7: Don't write `using namespace` at global scope in a header file](https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md#Rs-using-directive)

---

## General Rules

* **Qualify `auto`** with `const`, `*`, `&` and `&&` where applicable to more clearly express intent.
* **Disable copy/assignment/move by default** when adding a new class. Enable selectively only when there is a proven need and the implementation supports it.
* **Prefer `std::optional`** over `std::unique_ptr` for delayed or optional construction of members when possible, to reduce heap allocations.
* **Don't overuse `std::shared_ptr`.** Use it only if it's not clear when and where the object will be de-allocated. See: [C++ Core Guidelines R.shared_ptr](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rf-shared_ptr).
* **Avoid using the `long` type**, which could be either 32 bits or 64 bits.
* **Prefer `std::make_unique()`** when allocating objects on the heap. See:
  * <https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md#Rh-make_unique>
  * <https://herbsutter.com/2013/05/29/gotw-89-solution-smart-pointers/>
* **Use `reserve()` instead of `resize()`** on vectors. `resize()` default-constructs all elements for the given size, which is wasteful when they will be overwritten.

---

## Compiler Warnings

The following C++ warnings should never be disabled:

| Warning | Description |
|---------|-------------|
| C4018 | `signed/unsigned` mismatch |
| C4146 | Unary minus operator applied to unsigned type, result still unsigned |
| C4244 | Conversion from `type1` to `type2`, possible loss of data (e.g., `int64_t` to `size_t`) |
| C4267 | Conversion from `size_t` to `type`, possible loss of data |
| C4302 | Truncation from `type 1` to `type 2` |
| C4308 | Negative integral constant converted to unsigned type |
| C4532 | `continue`: jump out of `__finally` block has undefined behavior during termination handling |
| C4533 | Initialization of variable is skipped by instruction |
| C4700 | Uninitialized local variable used |
| C4789 | Buffer overrun |
| C4995 | Function marked as `#pragma deprecated` |
| C4996 | Use of a deprecated function, class member, variable, or typedef |

---

## Formatting

### Clang-format

Clang-format handles automatic code formatting. The `.clang-format` file in the repository root defaults to Google style rules.

The format configuration should be automatically discovered by clang-format tools. VS Code and Visual Studio both support clang-format on save.

---

## Code Analysis

[C++ Core Guidelines](https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md) rules should be enabled in Visual Studio Code Analysis. Code changes should build with no Code Analysis warnings.

---

## Unit Testing and Code Coverage

* There should be unit tests that cover the core functionality of the product, expected edge cases, and expected errors.
* Code coverage from tests should aim to maintain over **80%** coverage.
* All changes should be covered by new or existing unit tests.
* Tests use [Google Test](https://github.com/google/googletest) (GTest).

### Test Naming

Test methods should be named following the pattern:

```
<TestSuite>.<MethodOrFeature>_<ExpectedBehavior>[_When<Condition>]
```

Examples:
* `CApiTest.ManagerCreate_ReturnsOk`
* `CApiTest.ManagerDestroy_IsNoOp_WhenNull`
* `ConfigurationTest.Validate_RejectsEmptyAppName`

---

## C API Conventions

The C API boundary (`foundry_local_c.h`) follows these conventions for ABI stability:

### Exports and Versioning

* The library exports exactly **two symbols**: `FoundryLocalGetApi(uint32_t version)` and `FoundryLocalGetVersionString()`.
* All other functionality is accessed through **versioned structs of function pointers** (vtables) returned by `FoundryLocalGetApi`.
* `FOUNDRY_LOCAL_API_VERSION` is incremented with each release. New entries are appended at the end of each vtable struct — never removed or reordered.

### Type System

* Opaque types are declared with `FL_TYPE(X)` which expands to `struct flX; typedef struct flX flX`.
* Naming uses lowercase `fl` prefix: `flApi`, `flCatalog`, `flItem`, `flManager`, `flModel`, etc.
* Consumers never see internal struct layouts — all access goes through vtable function pointers.

### Sub-API Architecture

The root `flApi` struct provides accessors to five domain-specific sub-API vtables:

| Sub-API | Struct | Domain |
|---------|--------|--------|
| `GetCatalogApi()` | `flCatalogApi` | Model discovery and querying |
| `GetConfigurationApi()` | `flConfigurationApi` | SDK configuration (app name, dirs, log level, endpoints) |
| `GetItemApi()` | `flItemApi` | Item creation, typed setters/getters (text, tensor, image, message, tools) |
| `GetInferenceApi()` | `flInferenceApi` | Request/Response/Session lifecycle and inference execution |
| `GetModelApi()` | `flModelApi` | Model operations (download, load, unload, info) and ModelInfo accessors |

### Error Reporting

* Functions that can fail return `flStatus*` (typedef'd as `flStatusPtr`). `nullptr` = success, non-null = error.
* Error status is created via `Status_Create(flErrorCode, message)` and released via `Status_Release`.
* `flErrorCode` enum: `FOUNDRY_LOCAL_OK`, `_NOT_IMPLEMENTED`, `_INTERNAL`, `_INVALID_ARGUMENT`, `_INVALID_USAGE`, `_OPERATION_CANCELLED`, `_NETWORK`.
* All API functions are `noexcept` (`FL_NO_EXCEPTION`). Exceptions must be caught in the implementation and converted to status.

### Function Pointer Macros

* `FL_API_STATUS(Name, ...)` — declares a function pointer returning `flStatusPtr` with `_Check_return_` annotation.
* `FL_API_STATUS_IMPL(Name, ...)` — used in `.cc` files to define the implementation matching an `FL_API_STATUS` declaration.
* `FL_API_T(Name, ...)` — declares a function pointer returning an arbitrary type.
* `FL_TYPE_RELEASE(X)` — declares a release function `X_Release(flX*)` for an opaque type.

### Item Type System

Items (`flItem`) are typed via `flItemType` and support:

| Item Type | Setter(s) | Getter | Content |
|-----------|-----------|--------|---------|
| `TENSOR` | `SetTensor(data_type, data, shape, rank)` | `GetTensor(...)` | `flTensorDataType` + raw data + shape |
| `TEXT` | `SetText(text)` | `GetText(...)` | UTF-8 string |
| `IMAGE` | `SetImageBytes(format, data, size)` / `SetImageUri(uri, format)` | `GetImage(...)` | Bytes+format or URI+format |
| `MESSAGE` | `SetMessage(role, content, name)` | `GetMessage(...)` | Role + content + optional participant name |
| `TOOL_DEFINITION` | `SetToolDefinition(name, desc, schema)` | `GetToolDefinition(...)` | Function schema for tool calling |
| `TOOL_CALL` | `SetToolCall(call_id, name, args)` | `GetToolCall(...)` | Model's request to invoke a tool |
| `TOOL_RESULT` | `SetToolResult(call_id, result)` | `GetToolResult(...)` | Return value from a tool invocation |

Setters/getters enforce type matching — calling `SetText` on a `TENSOR` item returns an error.

### Well-Known Constants

* **Model properties**: `FOUNDRY_LOCAL_MODEL_PROP_*_STR` (string) and `FOUNDRY_LOCAL_MODEL_PROP_*_INT` (int64_t) — accessed via `Info_GetStringProperty` / `Info_GetIntProperty`.
* **Request parameters**: `FOUNDRY_LOCAL_PARAM_*` — passed as JSON-typed strings via `Request_SetParameter`. Covers sampling, tool choice, output format, conversation state, and reasoning.
* **Tensor data types**: `flTensorDataType` enum — values 0–24 matching ONNX `TensorProto.DataType`.

### SAL2 Annotations

* All pointer parameters use SAL2 annotations (`_In_`, `_Out_`, `_Outptr_`, `_In_opt_`, etc.) for static analysis.
* Non-MSVC platforms define these as empty macros.

---

## C++ Wrapper Conventions

The header-only C++ wrapper (`foundry_local_cpp.h` + `foundry_local_cpp.inline.h`) follows these conventions:

### File Organization

* `foundry_local_cpp.h` — declarations only (classes, structs, method signatures). Clean for API review.
* `foundry_local_cpp.inline.h` — all inline method implementations. Included at the bottom of the declarations header.
* `detail/cpp_api_helpers.h` — internal helpers shared by the wrapper.

### RAII Base Template

* `detail::Base<T>` is a generic RAII wrapper for opaque C types. Stores a pointer and an optional release function.
* Supports both owning (mutable `T`) and non-owning (const `T`) wrappers via `if constexpr (!std::is_const_v<T>)`.
* Provides `get()` (const), `get_mutable()` (non-const only), `operator*`, and implicit conversion to raw pointer.

### Error Handling

* `Error` class derives from `std::exception` with an `flErrorCode Code()` accessor.
* `Check(flStatus*)` — throws `Error` if status is non-null, releases the status afterward.
* All C++ wrapper methods that call the C API go through `Check()`.

### Class Hierarchy

```
detail::Base<T>
├── Configuration           (owns flConfiguration)
├── Manager                 (owns flManager, holds Configuration)
├── Catalog                 (non-owning flCatalog from Manager)
├── ModelList               (owns flModelList)
├── BasicItem<T>
│   ├── ConstItem           (non-owning const view)
│   └── Item                (owning mutable base)
│       ├── TextItem
│       ├── TensorItem
│       ├── ImageItem       (bytes or URI construction)
│       ├── MessageItem
│       ├── ToolDefinitionItem
│       ├── ToolCallItem
│       └── ToolResultItem
├── BasicModel<T>
│   ├── ConstModel          (non-owning const view)
│   └── Model               (mutable — download/load/unload)
├── Request                 (owns flRequest, accumulates Items)
├── Response                (owns flResponse)
└── Session                 (owns flSession, runs inference)
```

`ModelInfo` is a standalone non-owning view (not derived from `Base<T>`) — it wraps a raw `const flModelInfo*` with typed accessors.

### Content Structs

Each item type has a corresponding plain data struct returned by getter methods:

| Struct | Fields | Notes |
|--------|--------|-------|
| `TextContent` | `text` | |
| `TensorContent` | `data_type`, `data`, `shape` | `data` points into item's buffer |
| `ImageContent` | `format`, `data`, `data_size`, `uri` | Check `data` for null to distinguish bytes vs. URI |
| `MessageContent` | `role`, `content`, `name` | `name` is `optional<string_view>` |
| `ToolDefinitionContent` | `name`, `description`, `json_schema` | |
| `ToolCallContent` | `call_id`, `name`, `arguments` | |
| `ToolResultContent` | `call_id`, `result` | |

### Ownership and Move Semantics

* `Request::AddItem(Item&&)` transfers ownership — the item must not be used after this call.
* `Configuration` uses a builder pattern with chained setters returning `Configuration&`.
* Classes are move-only (no copy) to preserve handle ownership semantics.

