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
### Native non-generative packages

When the web service is enabled, `POST /v1/rank` and `POST /v1/systemone`
resolve the request `model` as a local-catalog ID or alias. Multi-component
packages use the metadata described in
[`docs/non-generative-packages.md`](docs/non-generative-packages.md). Runtime
caches are isolated by canonical model identity and provider; active requests
hold lifecycle ownership and block unload. Explicit local paths remain a
prototype fallback when an alias is not registered:

```bash
export FOUNDRY_LOCAL_RANK_MODEL_PATH=/models/clm
export FOUNDRY_LOCAL_SYSTEMONE_MODEL_PATH=/models/kev
export FOUNDRY_LOCAL_NON_GENERATIVE_PROVIDER=cuda
# Bounded session-local caches; setting either corresponding value to 0 disables.
export FOUNDRY_LOCAL_CLM_CACHE_CAPACITY=256
export FOUNDRY_LOCAL_CLM_CACHE_CAPACITY_BYTES=67108864
export FOUNDRY_LOCAL_KEV_CACHE_CAPACITY=512
export FOUNDRY_LOCAL_KEV_CACHE_CAPACITY_BYTES=16777216
# Alternatively set FOUNDRY_LOCAL_NON_GENERATIVE_PACKAGE_ROOT, containing
# rank/ and systemone/ subdirectories.

curl -s http://127.0.0.1:5272/v1/rank \
  -H 'Content-Type: application/json' \
  -d '{"model":"clm","context":{"text":"sample"},"question":"Select","answers":["first","second"],"temperature":1}'

curl -s http://127.0.0.1:5272/v1/systemone \
  -H 'Content-Type: application/json' \
  -d '{"model":"kev","state":"sample","questions":{"q":{"type":"noul","instructions":"Is this valid?"}}}'
```

Missing fallback configuration is reported as HTTP 503. Invalid contracts,
tasks, criteria, or token bounds are HTTP 400. This bounded integration does
not add remote catalog submission or C/Python SDK APIs.
Until the corresponding ORT GenAI package is published, configure this branch
with `--ort_genai_home /path/to/onnxruntime-genai` so the native
`DirectoryTokenizer` and component session exports are available.

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
### Structured coding tools

Structured tool turns on both Engine and Generator use one immutable `ToolCallPlan` for declaration
admission, call-name selection, original-schema validation, and batch acceptance.
The model emits a nonempty canonical JSON array inside its published tool
markers, for example:

```json
[{"name":"edit","parameters":{"path":"main.py","new_str":"print(\"hello\")\n"}}]
```

This replaces the model's native XML argument protocol; callers continue using
the ordinary Chat Completions, Responses, or SDK tool APIs. Arguments are never
coerced, repaired, default-filled, or guessed from XML text.

**Generation and validation have different responsibilities.** Structural Lark
guidance restricts JSON syntax, the canonical envelope, declared/forced tool names,
and serial versus parallel calls. It preserves arbitrary argument property order,
JSON structural whitespace, and standard string escapes, including printable Unicode escapes. It does
not use the runtime's JSON Schema-to-grammar projection: a mask must not turn a
valid argument spelling into a different value to satisfy a compiler limitation.
The unchanged original parameter schema is validated with jsoncons before any
call in the batch is published. Structural guidance alone does not guarantee
schema compliance or that the model chose the user's intended argument value.

Automatic tool turns select their response mode explicitly: ordinary assistant
text starts with an internal `MESSAGE` line, while executable calls start with
the model's dedicated tool boundary. The runtime removes the text framing line
incrementally; callers still receive normal content and tool-call events.
Unframed tool-shaped text is rejected instead of silently returned as an answer.
Historical assistant text uses the same framing when projected back to the model.

The admitted schema profile supports Draft 2020-12 and supported Draft 7
declarations, local JSON Pointer references, composition, string/numeric/array
bounds, and open objects when the original schema permits them. Remote references,
recursive references, anchors, custom vocabularies, reserved control keywords, and
unsupported content features are rejected explicitly, not silently dropped.
Unknown non-reserved keywords and `format` are treated as annotations; their original
values remain in the schema for prompting and validation. Format assertions are not enabled.
Schema input is limited to 1 MiB, individual argument JSON to 4 MiB, a canonical batch
to 16 MiB, and nesting to 64. Duplicate JSON members are rejected before DOM parsing can erase them.

Only model-published token identities delimit reasoning and executable tool
regions. Marker spellings quoted in reasoning or in argument strings remain
data, even when the tokenizer marks the published added tokens `special:false`.
Transcript text, tool results, schemas, and historical argument bodies use a
separate literal tokenizer; template-owned structure and runtime-owned protocol
boundary exemplars use their actual token identities. This separation preserves
multiline code, backslashes, Unicode, bracket-prefixed strings, and literal
tool/reasoning marker text.

Automatic structured tool turns use whole-turn guidance rather than region guidance, so a
marker mentioned in scratchpad text cannot prematurely activate argument decoding.
Whole-turn guidance disables speculative drafts; qualify its performance as well
as correctness with the intended model. Region-only masks cannot enforce the
initial text-versus-tool response mode and are not an automatic tool fallback.
Required tools, forced names, and `parallel_tool_calls=false` remain authoritative.

Concurrent Engine requests wait while resident turns make progress, including
background coding-agent compaction. The capacity watchdog bounds stalled capacity,
not total queue time; queued requests remain cancelable and do not expire merely
because another healthy local generation lasts more than 30 seconds.

An invalid uncommitted call gets at most one guided retry with a bounded schema
diagnostic and the remaining output-token budget. Already streamed text/reasoning
retains its exact token prefix and is not published twice. No retry is allowed
after a tool call was published, cancellation, truncation, stop-sequence
interruption, or incompatible explicit/raw guidance. Rejected and interrupted
argument payloads never become assistant text. Exhaustion is an explicit error,
not an empty successful response.

Recovery also shares implicit classic Generator output limits; it cannot acquire a
fresh default allowance or use the retained conversation's full context as an output
budget. Engine text turns remain uncapped when the caller leaves their limit unset.

Qualification requires both protocol fidelity and the **real Copilot CLI** coding
workflow, not just an offline harness self-test. With the server already running:

```powershell
python sdk_v2\cpp\test\manual\tool_call_gate.py `
  --endpoint http://127.0.0.1:5273/catalogs/local/v1 --model qwen-3.8-27b-dflash2:1 `
  --output-dir sdk_v2\cpp\build\tool-call-gate
python sdk_v2\cpp\test\manual\coding_agent_gate.py `
  --endpoint http://127.0.0.1:5273/catalogs/local/v1 --model qwen-3.8-27b-dflash2:1 `
  --schema-profile native --timeout-seconds 600 --max-output-tokens 8192 `
  --output-dir sdk_v2\cpp\build\coding-agent-gate
```

Use fresh output directories and run the Copilot gate separately for `native`,
`minlength`, and `oneof`. The isolated CLI must read/search files, encounter a real
tool error and failing tests, repair the adversarial fixture through real editing
tools, and finish with passing tests and exact file bytes. Private HTTP/SSE
artifacts must show the intended local provider/model, no provider errors, and no
invalid calls. These opt-in gates require the supplied model and installed CLI;
they do not download models in CI or certify an unchanged release package.
An `edit` qualifies only with a successful correlated CLI completion and exact
full-file original and replacement arguments, including terminal LF; a failed edit
followed by shell repair is insufficient. Protocol checks require a successful
terminal choice, with `[DONE]` following it for streaming Chat Completions.
The protocol gate also checks optional-property ordering through nonstreaming
`/responses`, requiring a completed response and completed function calls.
It checks ordinary function calls with a raw patch descriptor enabled in both
streaming and nonstreaming Chat Completions.
Qualification applies to the model, backend, platform, and exact native binaries
recorded for the run. Scripted Generator regressions do not substitute for a
trained Generator model gate, and these text-only workflows do not qualify
multimodal input. Rebuilt release artifacts must pass the same gates independently.

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
| `kev_decision_example <package_path> [provider]` | Run a typed KEV decision package through Foundry Local Core |

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

## Non-generative ranking and decisions

The CLM ranking and KEV decision routes translate their existing JSON contracts
to the public ORT GenAI `OgaStructuredRequest` and
`OgaFreeFormRankRequest` types. Rendering, tokenization, tensor preparation,
pooling, scoring, and typed result construction are owned by ORT GenAI; Foundry
only performs contract translation. Set
`FOUNDRY_LOCAL_NON_GENERATIVE_PROVIDER=cuda` to select CUDA only for prototype
path-fallback packages. Catalog-selected packages use their registered/package
provider metadata and are not overridden by this environment variable.
The `FOUNDRY_LOCAL_{CLM,KEV}_CACHE_CAPACITY` and `_CAPACITY_BYTES` variables
configure bounded session-local runtime caches. Package runtimes are also
cached per canonical catalog identity and evicted by model unload. KEV caches
tokenized prefixes/rows only.
True recurrent/convolution/KV state branching needs a future exported
graph/state-handle contract.
The HTTP request and response schemas are unchanged.

## License

Licensed under the MIT License. See [LICENSE.txt](LICENSE.txt).
