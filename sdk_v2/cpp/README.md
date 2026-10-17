# Foundry Local C and C++ SDK (v2)

The Foundry Local C++ SDK runs AI models locally through the Foundry Local engine. It
provides a header-only, RAII-style C++17 wrapper over the stable C ABI for catalog
access, model lifecycle management, chat, tool calling, embeddings, audio
transcription, execution-provider management, and the optional OpenAI-compatible web
service.

C and C++ are supported application interfaces to the same native runtime that powers
the C#, JavaScript, Python, and Rust bindings. C applications use the
[public C API](include/foundry_local/foundry_local_c.h) directly; C++ applications can
use the [C++ wrapper](include/foundry_local/foundry_local_cpp.h). Neither requires a
separate server process.

The SDK implementation builds as C++20, while applications that consume the public
wrapper need only C++17 or newer.

For an inference-focused introduction to models, sessions, requests, responses, items,
and streaming across languages, see [Inference with the Foundry Local SDK](../INFERENCING.md).

## Features

- **Model catalog** -- discover model aliases, variants, cached models, and loaded models
- **Model lifecycle** -- download, load, unload, remove, and register local model assets
- **Chat** -- stateful chat sessions, including text-only `coding` models, with streaming and per-request settings
- **Tool calling** -- session-scoped and request-scoped tool definitions, plus typed tool call/result items
- **Embeddings** -- single and batch `std::vector<float>` helpers
- **Audio** -- offline and real-time streaming audio transcription
- **Execution providers** -- discover, download, and register hardware acceleration on demand
- **Embedded web service** -- optionally expose an OpenAI-compatible local endpoint
- **C API** -- a stable ABI for other language bindings and low-level integrations

Models with task `coding` use `ChatSession`. See [coding models](../INFERENCING.md#coding-models) for
capabilities, registration, and compatibility considerations.

## Install From NuGet

**You do not need to build Foundry Local from source.** Restore the published
[`Microsoft.AI.Foundry.Local.Runtime`](https://www.nuget.org/packages/Microsoft.AI.Foundry.Local.Runtime)
package and its dependencies with the NuGet CLI:

```bash
nuget install Microsoft.AI.Foundry.Local.Runtime -OutputDirectory packages
```

The Runtime package contains the public C/C++ headers and prebuilt native libraries.
It is also the native dependency of the `Microsoft.AI.Foundry.Local` C# SDK package.
Install the Runtime package directly for C and C++ to avoid the managed SDK dependencies.
Neither C nor C++ applications need a .NET runtime to use the native libraries.

After restoring the packages:

| Asset | Location |
|---|---|
| C API and C++17 wrapper headers | `packages/Microsoft.AI.Foundry.Local.Runtime.<version>/build/native/include/` |
| Foundry Local native library | `packages/Microsoft.AI.Foundry.Local.Runtime.<version>/runtimes/<rid>/native/` |
| ONNX Runtime and GenAI dependencies | Matching native assets in the restored `Microsoft.ML.OnnxRuntime` and `Microsoft.ML.OnnxRuntimeGenAI.Foundry` packages |

Choose the RID for your application's operating system and architecture:
`win-x64`, `win-arm64`, `linux-x64`, `linux-arm64`, or `osx-arm64`.
Add the header directory to your compiler's include path. On Windows, link
`foundry_local.lib`; on Linux, link `libfoundry_local.so`; on macOS, link
`libfoundry_local.dylib`. Copy the matching native runtime files and their dependency
libraries beside your executable or into its loader search path. Keep the restored
package versions together; do not mix headers and native libraries from different
releases.

See [Use In Your Project](#use-in-your-project) for a CMake example,
[Quick Start](#quick-start) for C++, and [C API](#c-api) for C.

## Source Build Prerequisites

The following tools are needed only when developing or customizing the native SDK,
not when consuming the published NuGet packages.

- CMake 3.20 or newer
- Python 3.10 or newer to run `build.py`
- A C++20-capable compiler to build the SDK: Visual Studio 2022/2026 on Windows,
  Clang 16+ or GCC 12+ on Linux, or Xcode Command Line Tools on macOS
- [vcpkg](https://github.com/microsoft/vcpkg), available through `VCPKG_ROOT` or
  explicitly supplied with `--vcpkg_root`

The build resolves its C++ dependencies from the manifest in `vcpkg.json`, and obtains
the pinned ONNX Runtime and ONNX Runtime GenAI dependencies during CMake configuration.

Linux runtime requirements are architecture-specific: glibc 2.28 or newer on x64,
and glibc 2.34 or newer on ARM64. The ARM64 minimum also applies to CPU-only usage
because the pinned GenAI runtime requires it.

Chat completions accept a typed `chat_template_kwargs` JSON object, such as
`{"enable_thinking": false}`, for model-specific template controls. The pinned stable
GenAI package supports this feature; a nightly package is not required. Session-level
kwargs are defaults: per-request kwargs override the entire object, and `{}` explicitly
clears an inherited default for that request.
For OpenAI JSON requests, the payload takes precedence over request options, followed
by session defaults. Omitting kwargs inherits the session default, if any.

### CUDA acceleration

On Windows and Linux (x64 and ARM64), Foundry Local downloads and verifies only the
CUDA execution provider and ORT GenAI CUDA libraries. It does not distribute the
CUDA toolkit or cuDNN runtime libraries. Install compatible CUDA and cuDNN runtimes
separately before registering the CUDA execution provider.

On Windows, make the runtime DLL directories available through `PATH` for ORT's
provider loader. On x64, also call `AddDllDirectory` for the CUDA runtime directory
before CUDA registration: the GenAI CUDA bridge directly imports the CUDA runtime,
and its explicit DLL search policy includes application-added directories but not `PATH`.

Applications that manage DLL search directories instead of `PATH` can call
`AddDllDirectory` for each runtime directory and enable these directories with
`SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` before CUDA registration.
This process-wide setting replaces the standard PATH-based DLL search; register all
required runtime directories when using it. It is not needed solely for the x64 bridge load.
Foundry Local leaves runtime dependency loading to ORT and GenAI. On Linux, use the
system loader configuration or `LD_LIBRARY_PATH`.

## Clearing a Model Cache

Use `Model::RemoveFromCache()` to clear a completed, interrupted, or corrupted download.
It does not require `IsCached()` to return true. Unload the model first and stop any active
downloads; removal rejects loaded models and conflicting downloads. After successful
removal, the model handle reports `IsCached() == false` and its path is cleared.
Removal on the same handle is rejected until an active download has finished its
callbacks and published its cached state, including cache-hit downloads.
Externally registered models must be unregistered instead; their files are not deleted.
Applications sharing a cache must coordinate removal with other instances using the
model. The download lock serializes file mutations, but does not protect cache-hit
reads or models loaded in another process.

`GetPath()` returns the inference directory, which may be a revision subdirectory.
Do not delete that directory manually: download markers and other revisions can live
above it. Removal clears the model's cache root, including payloads, inference metadata,
`download.tmp`, blob resume sidecars, and legacy progress markers. An empty root may
remain for download-lock safety; it is not considered cached. Shared catalog metadata
and other models are retained.

For scanner-discovered locations outside the standard layout, removal also recognizes a
parent containing only numeric revision directories (`v2`, `v3`, etc.) and download
markers or root-level `inference_model.json`. Matching root metadata can establish
ownership without download markers; corrupt metadata requires a marker. Removal locks
the parent before inspecting ownership and retains that lock even if unrelated entries
or another model's metadata restrict deletion to the discovered inference directory.
It never expands to the cache root or across unrelated parent entries.

## Native Export Isolation

The shared library exports only `FoundryLocalGetApi` and `FoundryLocalGetVersionString`.
ELF builds use a version script and `--exclude-libs,ALL` to keep bundled dependencies
and compiler compatibility archives private. macOS/iOS use an exported-symbols
allowlist; Windows retains its explicit `.def` exports. Static and OBJECT libraries
remain available to white-box tests. Linux's statically linked internal test executable
also localizes archive-owned symbols so telemetry-thread vtables cannot interpose
with GenAI. The public export allowlist applies to `foundry_local`, not separately
shipped shared dependencies.

Model-free exact-export checks run in Linux and macOS builds; Linux also checks
strong/weak archive isolation, private transport symbols in internal tests, and runtime
calls through the public fixture API.
ELF checks inspect external definitions only: local symbols retained in `.dynsym`
by GCC compatibility objects are not public exports.

## Build From Source

Building from source is optional. For application development, use the
[published NuGet packages](#install-from-nuget) instead.

From this directory:

```pwsh
# Windows
./build.bat --configure --build --test --config RelWithDebInfo

# Or use the cross-platform Python driver directly.
python build.py --configure --build --test --config RelWithDebInfo
```

On Linux or macOS:

```bash
./build.sh --configure --build --test --config RelWithDebInfo
```

With no phase flags, `build.py` runs configure, build, and test. Useful development
invocations include:

```pwsh
# Rebuild an already configured tree without running tests.
python build.py --build --config Debug

# Build without examples or the embedded web service.
python build.py --configure --build --skip_examples --skip_service

# Supply local ORT and ORT GenAI source/build roots.
python build.py --configure --build --ort_home C:\src\onnxruntime --ort_genai_home C:\src\onnxruntime-genai

# Generate Windows ARM64 build files.
python build.py --configure --build --arm64
```

Build output is rooted at `build/<platform>/<configuration>/bin/`. On Windows, for
example, the default library and examples are under
`build/Windows/RelWithDebInfo/bin/RelWithDebInfo/`.

The build places `foundry_local` alongside its ONNX Runtime and ONNX Runtime GenAI
dependencies. Keep these files together when copying a build to another location.

### Dependency regression checks

Native Windows x64, macOS ARM64, and Linux CI run the telemetry shutdown and
installed nanoarrow consumer regressions. Telemetry tests require
`TELEMETRY_SHUTDOWN_SDK_SOURCE_DIR` to name the pinned telemetry sources with all
patches from `ports/cpp-client-telemetry/portfile.cmake` applied. They link the real
Foundry Manager and observe SDK lock, worker, version, and encoder lifetimes.
Events traverse normal decoration, queuing, storage, and encoding; only the HTTP
transport is replaced with an in-process responder. The `upload-after-ci` case
holds encoding until process exit after a CI-suppressed first Manager.

Windows also runs five DLL lifecycle cases for the Manager owner and standalone
telemetry. A mock provider avoids the real SDK workers' DLL pinning so the
fixture can exercise actual `FreeLibrary` teardown. Process-termination cases
retain a pending mock uploader callback; they do not access private locks or
reproduce lock ownership by terminated threads. Normal destruction and explicit
`FreeLibrary` must each flush, tear down, and release the uploader exactly once. Only Windows
process termination abandons cleanup; pending telemetry is not flushed there.
The standalone bridge covers teardown inside `FreeLibrary`; active Manager
runtime resources can retain the DLL, so Manager coverage explicitly destroys
the instance before unloading.

```pwsh
python tools/run_dependency_tests.py --build build/Windows/RelWithDebInfo --config RelWithDebInfo
```

The runner rejects missing and skipped cases. Nanoarrow consumers configure,
link, and run separately in Debug and Release, with an application-defined
`flatccrt` target declared both before and after package discovery.

### Measuring dependency updates

Build baseline and updated SDKs from the same Foundry sources, toolchain,
configuration, and ORT/GenAI packages, preserving each native bundle and its
vcpkg status file. The `sdk-dependency-probe` executable loads the actual shared
SDK through its public C ABI. Compare the bundles with:

```pwsh
python tools/measure_sdk_dependencies.py `
  --probe build/Windows/RelWithDebInfo/bin/RelWithDebInfo/sdk-dependency-probe.exe `
  --baseline build/baseline-native/foundry_local.dll `
  --candidate build/updated-native/foundry_local.dll `
  --output build/dependency-measurements --samples 10
```

Add `--model-id <exact-catalog-model-id>` to time real model downloads into fresh,
isolated caches; this is opt-in and may transfer substantial data. Runs alternate
baseline/current order and preserve every sample, binary hashes, medians, native
payload/ZIP sizes, and (on Windows) unsigned C++ SDK-layout ZIP size including
headers, symbols, and the import library. Telemetry is CI-suppressed in both
bundles to prevent external collector transmission. These are warm-file-cache,
local-host observations, not cold-boot, inference, telemetry-enabled, mobile, or
release-signed-package measurements. Network variability is not itself evidence
of a dependency performance regression.
### Qwen XML region guidance

With ONNX Runtime GenAI 0.18.0 or later, Foundry Local automatically constrains
the body of eligible Qwen XML function calls. No caller setting is required.

Eligibility requires native Qwen XML automatic text-and-tools on Engine, distinct
tool marker token IDs published by GenAI's tokenizer whose decoded strings match
the effective model markers, no caller guidance or forced/raw tool,
and a **fully representable** offered set of ordinary function tools. The planner
accepts at most 64 declared fields per tool and emits required fields before optional fields
in a canonical order, keeping grammar size linear in the field count. The strict
decoder still accepts any valid field order. Bounded supported schemas include
scalar, array, and nested object types, nullable types, enums, local `#/$defs/name`
references, and selected unions; unsupported or malformed definitions leave the
entire turn unguided with a warning. Explicitly open root parameter objects are
ineligible, and the full request is subject to normalization and compiled-grammar
size budgets. This is not unrestricted JSON Schema support.
An unused unsupported but well-formed tool does not block recovery of a valid
call to another tool; malformed declarations still block recovery, and a call
to the unsupported tool remains invalid.
The region grammar constrains function and parameter names, required fields, and
reserved XML framing, while preserving multiline code and literal `<`. Structured
values that are not constrained by the grammar are checked by the strict decoder,
so guidance is not a guarantee that every generated value is valid.
Ambiguous interrupted calls remain
subject to strict rejection/finalization rules. As in the existing
native `auto` path, an interrupted, structurally plausible *incomplete* call
can be surfaced as text; schema-invalid or unsupported-only function calls cannot
dispatch, and unrecognizable function declarations cannot leak their XML.
Admission errors from the local GenAI API are reported, not retried as
unguided success.

To exercise the guided path with a Qwen model, run from the repository root:

```powershell
python sdk_v2\cpp\test\manual\qwen_region_guidance_smoke.py --model ..\models\qwen_3.8_27b_int4_int8kv_dflash2_int4
```

This optional Windows ARM64 smoke test requires a built Python SDK binding
(`--binding <package-directory>` if it is not installed) and CUDA EP. It checks
that the native Engine installed region guidance on each request, then verifies
required, optional, parameterless, and multiline tool calls, strict decoding,
and tool-result continuation; it does not download a model in CI.
Run the same command with `--no-reasoning` to check two unconstrained text
turns followed by a tool call without relying on reasoning-driven invalidation.
Region guidance alone does not discard retained Engine state after an ordinary
non-reasoning text turn; generated tool calls still invalidate it.

## Quick Start

Include the C++ wrapper and link the `foundry_local` shared library. The example below
uses a catalog model, downloading it on first use:

```cpp
#include <foundry_local/foundry_local_cpp.h>

#include <iostream>

using namespace foundry_local;

int main() {
  try {
    Configuration config("my-app");
    Manager manager(std::move(config));

    auto& catalog = manager.GetCatalog();
    auto model = catalog.GetModel("qwen2.5-0.5b");
    if (!model) {
      std::cerr << "Model was not found in the catalog.\n";
      return 1;
    }

    if (!model->IsCached()) {
      model->Download([](float percent) -> int {
        std::cout << "\rDownloading: " << static_cast<int>(percent) << "%" << std::flush;
        return 0;  // Return nonzero to cancel.
      });
      std::cout << "\n";
    }

    model->Load();
    {
      // The session owns conversation state. Creating a session is cheap;
      // loading the model is the expensive operation.
      ChatSession session(*model);
      Request request{UserMessage("Why is the sky blue?")};
      Response response = session.ProcessRequest(request);

      for (const Item& item : response.GetItems()) {
        if (item.GetType() == FOUNDRY_LOCAL_ITEM_MESSAGE) {
          std::cout << item.GetMessage().GetSimpleText() << "\n";
        }
      }
    }
    model->Unload();
  } catch (const Error& error) {
    std::cerr << "Foundry Local error [" << error.Code() << "]: " << error.what() << "\n";
    return 1;
  }
}
```

`Manager` must outlive catalog, model, and metadata views obtained from it. SDK wrapper
objects use RAII; requests, responses, sessions, and items release their native handles
when they leave scope.

## Use In Your Project

The public wrapper is header-only. After [installing from NuGet](#install-from-nuget),
point CMake at the restored Runtime package and your application's RID, and link
your executable to the packaged native library:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_foundry_app LANGUAGES CXX)

set(FOUNDRY_LOCAL_RUNTIME_DIR "" CACHE PATH "Restored Microsoft.AI.Foundry.Local.Runtime.<version> directory")
set(FOUNDRY_LOCAL_RID "linux-x64" CACHE STRING "Target NuGet runtime identifier")
find_library(FOUNDRY_LOCAL_LIBRARY
    NAMES foundry_local
    PATHS "${FOUNDRY_LOCAL_RUNTIME_DIR}/runtimes/${FOUNDRY_LOCAL_RID}/native"
    NO_DEFAULT_PATH REQUIRED)

add_executable(my_foundry_app main.cc)
target_compile_features(my_foundry_app PRIVATE cxx_std_17)
target_include_directories(my_foundry_app PRIVATE "${FOUNDRY_LOCAL_RUNTIME_DIR}/build/native/include")
target_link_libraries(my_foundry_app PRIVATE "${FOUNDRY_LOCAL_LIBRARY}")
```

On Windows, place `foundry_local.dll`, `onnxruntime.dll`, `onnxruntime-genai.dll`, and
the other native runtime dependencies from the restored packages beside your executable. On Linux and macOS,
place `libfoundry_local` and its neighboring runtime libraries beside the executable or
in a loader search path. The shared library uses `$ORIGIN` on Linux and `@loader_path` on
macOS to locate co-located dependencies.

### C API

For a C application, a lower-level integration, or another language binding, include
[`foundry_local/foundry_local_c.h`](include/foundry_local/foundry_local_c.h) and link the
same `foundry_local` shared library. Use the [NuGet installation instructions](#install-from-nuget)
above to restore the prebuilt native library, matching headers, and runtime dependencies.

Call `FoundryLocalGetApi(FOUNDRY_LOCAL_API_VERSION)` to obtain the root `flApi` function
table. Check for a null pointer, which indicates that the library does not support the
requested API version. The root table exposes manager operations and accessors such as
`GetConfigurationApi()`, `GetCatalogApi()`, `GetModelApi()`, `GetInferenceApi()`, and
`GetItemApi()` for configuration, catalog access, model management, and inference.

The C API uses opaque handles and explicit ownership rather than C++ classes:

- Check the returned `flStatus*` from status-returning calls. A null status means
  success; otherwise inspect `Status_GetErrorCode()` and `Status_GetErrorMessage()`,
  then call `Status_Release()`.
- Release owned handles with their corresponding `*_Release` function. Borrowed
  pointers and views remain valid only for the lifetime documented in the header.
- Follow the lifetime and callback ownership contracts documented on each function
  in the public C header. The C++ wrapper automates handle cleanup with RAII.

The C API exposes chat, tool calling, embeddings, audio transcription, and the same
model and execution-provider lifecycle used by the C++ wrapper.

## Common Workflows

### Inspect and manage models

```cpp
auto& catalog = manager.GetCatalog();

for (const auto& model : catalog.GetModels()) {
  ModelInfo info = model->GetInfo();
  std::cout << info.Alias() << " (" << info.Id() << ")\n";
}

auto model = catalog.GetModel("phi-3.5-mini");
if (model && !model->IsCached()) {
  model->Download();
}
if (model) {
  model->Load();
  // Use a session here.
  model->Unload();
}
```

Use `GetVariants()` and `SelectVariant()` to choose a particular hardware or
quantization variant. `GetCachedModels()` and `GetLoadedModels()` avoid scanning the full
catalog when only local state matters.

### Stream chat results

`SetStreamingCallback` receives items through the native item queue. Return zero to
continue or a nonzero value to cancel generation.

```cpp
ChatSession session(*model);
session.SetStreamingCallback([](flStreamingCallbackData event) -> int {
  flItem* raw_item = nullptr;
  if (detail::item_api()->ItemQueue_TryPop(event.item_queue, &raw_item)) {
    Item item(*raw_item);
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
      std::cout << item.GetText().text << std::flush;
    }
  }
  return 0;
});

Response response = session.ProcessRequest(Request{UserMessage("Write a short poem.")});
```

The final `Response` remains available after streaming completes, including finish reason
and token usage.

### Configure requests and tools

Apply generation settings to an individual request or to every request on a session:

```cpp
ChatSession session(*model);
session.AddToolDefinition(ToolDefinition{
    "get_weather",
    "Get the current weather for a city.",
    R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"]})",
});

Request request{UserMessage("What is the weather in Seattle?")};
RequestOptions options;
options.search.temperature = 0.2f;
options.search.max_output_tokens = 256;
options.tool_choice = FOUNDRY_LOCAL_TOOL_CHOICE_AUTO;
request.SetOptions(options);
Response response = session.ProcessRequest(request);
```

Read tool calls using `Item::GetToolCall()`, execute them in application code, and send
results on the next turn with `Item::ToolResult(call_id, result)`.

### Generate embeddings

```cpp
EmbeddingsSession session(*model);
std::vector<float> vector = session.Embed("The quick brown fox");
std::vector<std::vector<float>> batch = session.Embed({"First input", "Second input"});
```

`EmbeddingsSession::Embed` returns L2-normalized vectors. The model must be an embeddings
model and loaded before creating the session.

### Manage execution providers

Catalog access does not trigger execution-provider downloads. Discover and register them
explicitly when hardware acceleration is needed:

```cpp
for (const EpInfo& ep : manager.GetDiscoverableEps()) {
  std::cout << ep.name << ": " << (ep.is_registered ? "registered" : "available") << "\n";
}

manager.DownloadAndRegisterEps({}, [](std::string_view name, float percent) {
  std::cout << "\r" << name << ": " << percent << "%" << std::flush;
  return true;  // Return false to cancel.
});
```

### Start the embedded web service

```cpp
Configuration config("my-app");
config.AddWebServiceEndpoint("http://127.0.0.1:5000");
Manager manager(std::move(config));

manager.StartWebService();
for (const std::string& endpoint : manager.GetWebServiceEndpoints()) {
  std::cout << "Listening on " << endpoint << "\n";
}

manager.StopWebService();
```

Streaming chat completions, Responses, and audio transcriptions cancel their active inference when the SSE client
disconnects. The service sends SSE keepalive comments during token silence so a dropped connection can be detected
without waiting for the next token. Undrained streams are limited to 1 MiB of buffered events; exceeding the limit
cancels inference and replaces pending events with a terminal SSE error. A disconnected stream cannot be resumed.
Non-streaming chat completions cancel inference on TCP resets or other transport errors without consuming pending
request bytes. A graceful FIN alone does not cancel inference: clients can half-close their sending direction and
still read a response, so a graceful full close cannot be distinguished until the response is written. Other
non-streaming routes are unchanged. Engine cancellation is cooperative between inference steps; an in-progress
prefill step cannot be interrupted before it returns.

Configure with `--skip_service` when the web service is not required; this removes its
oat++ dependency from the build.

## Examples

The build produces these example executables in the build output directory:

| Example | Demonstrates |
|---|---|
| `basic_chat_example` | Catalog lookup, model lifecycle, non-streaming and streaming chat |
| `tool_calling_example` | Tool definitions, tool-result turns, and streaming tool calls |
| `embeddings_example` | Single and batch embeddings with cosine similarity |
| `realtime_audio_example <audio_file_path>` | PCM streaming input and streaming transcription output |

The source for each is under [examples/](examples/). `basic_chat_example` honors
`FOUNDRY_LOCAL_SAMPLE_CACHE_DIR` to use an existing model-cache location.

## Testing

`build.py --test` invokes CTest with a 600-second timeout and prints failures. The build
creates three test executables:

- `foundry_local_tests`: implementation-level unit tests
- `sdk_integration_tests`: public C++ wrapper tests compiled as C++17
- `cache_only_tests`: cache-only, live-catalog, BYOM, and manager web-service tests

To run one executable directly after a build on Windows:

```pwsh
./build/Windows/RelWithDebInfo/bin/RelWithDebInfo/sdk_integration_tests.exe --gtest_filter="*Chat*"
```

Some integration tests require cached test models. Set `FOUNDRY_TEST_DATA_DIR` to the
shared test-model cache directory before running them. Tests that cannot use the cache
are skipped rather than downloading multi-gigabyte assets.

## Project Layout

```text
sdk_v2/cpp/
├── include/foundry_local/  Public C and header-only C++ APIs
├── src/                    Engine implementation
├── examples/               Runnable chat, tool, embedding, and audio programs
├── test/                   Internal and public-SDK GoogleTest suites
├── cmake/                  Shared CMake configuration
├── build.py                Configure, build, test, and cross-compilation driver
├── vcpkg.json              Dependency manifest
└── docs/                   Design notes and implementation documentation
```

## Further Reading

- [Wrapper interface design](docs/WrapperInterfacesDesign.md)
- [Runtime loading behavior](docs/OrtRuntimeLoading.md)
- [Item data ownership](docs/ItemDataOwnershipDesign.md)
- [C++ port guide](docs/CppPortGuide.md)

## License

Licensed under the MIT License. See [LICENSE.txt](LICENSE.txt).