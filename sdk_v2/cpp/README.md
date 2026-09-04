# Foundry Local C++ SDK (v2)

The Foundry Local C++ SDK runs AI models locally through the Foundry Local engine. It
provides a header-only, RAII-style C++17 wrapper over the stable C ABI for catalog
access, model lifecycle management, chat, tool calling, embeddings, audio
transcription, execution-provider management, and the optional OpenAI-compatible web
service.

The SDK implementation builds as C++20, while applications that consume the public
wrapper need only C++17 or newer.

## Features

- **Model catalog** -- discover model aliases, variants, cached models, and loaded models
- **Model lifecycle** -- download, load, unload, remove, and register local model assets
- **Chat** -- stateful chat sessions with streaming callbacks and per-request settings
- **Tool calling** -- session-scoped and request-scoped tool definitions, plus typed tool call/result items
- **Embeddings** -- single and batch `std::vector<float>` helpers
- **Audio** -- offline and real-time streaming audio transcription
- **Execution providers** -- discover, download, and register hardware acceleration on demand
- **Embedded web service** -- optionally expose an OpenAI-compatible local endpoint
- **C API** -- a stable ABI for other language bindings and low-level integrations

## Prerequisites

- CMake 3.20 or newer
- Python 3.10 or newer to run `build.py`
- A C++20-capable compiler to build the SDK: Visual Studio 2022/2026 on Windows,
  Clang 16+ or GCC 12+ on Linux, or Xcode Command Line Tools on macOS
- [vcpkg](https://github.com/microsoft/vcpkg), available through `VCPKG_ROOT` or
  explicitly supplied with `--vcpkg_root`

The build resolves its C++ dependencies from the manifest in `vcpkg.json`, and obtains
the pinned ONNX Runtime and ONNX Runtime GenAI dependencies during CMake configuration.

## Build From Source

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

The public wrapper is header-only. Add `include/` to your header search path and link
your executable to `foundry_local`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_foundry_app LANGUAGES CXX)

add_executable(my_foundry_app main.cc)
target_compile_features(my_foundry_app PRIVATE cxx_std_17)
target_include_directories(my_foundry_app PRIVATE /path/to/foundry-local/sdk_v2/cpp/include)
target_link_libraries(my_foundry_app PRIVATE /path/to/foundry-local.dll-or-library)
```

On Windows, place `foundry_local.dll`, `onnxruntime.dll`, `onnxruntime-genai.dll`, and
the other files from the SDK build output beside your executable. On Linux and macOS,
place `libfoundry_local` and its neighboring runtime libraries beside the executable or
in a loader search path. The shared library uses `$ORIGIN` on Linux and `@loader_path` on
macOS to locate co-located dependencies.

For a lower-level integration or another language binding, include
`foundry_local/foundry_local_c.h` and use the versioned C API function tables instead.

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