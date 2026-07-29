# Foundry Local C++ SDK

The Foundry Local C++ SDK provides a C++17 static library for running AI models locally via [Foundry Local](https://www.foundrylocal.ai/). Discover, download, load, and run inference entirely on your own machine — no cloud required.

> **Windows-only** — requires MSVC or clang-cl (MSVC-compatible toolchain).

## Features

- **Model catalog** — browse and search all available models; filter by cached or loaded state
- **Lifecycle management** — download, load, unload, and remove models programmatically
- **Chat completions** — synchronous and streaming via OpenAI-compatible types
- **Audio transcription** — transcribe audio files with streaming support
- **Tool calling** — define tools and handle tool-call responses in chat completions
- **Download progress** — wire up a callback for real-time download percentage
- **Model variants** — select specific hardware/quantization variants per model alias
- **Optional web service** — start an OpenAI-compatible REST endpoint
- **Execution providers** — discover, download, and register EPs with per-EP progress reporting
- **Auto NuGet download** — CMake auto-downloads native runtime DLLs at configure time

## Prerequisites

| Requirement | Notes |
|---|---|
| **CMake >= 3.20** | Ships with Visual Studio 2022 |
| **Ninja** | Ships with Visual Studio 2022 |
| **vcpkg** | Set the `VCPKG_ROOT` environment variable to your vcpkg installation |
| **MSVC** (or clang-cl) | Visual Studio 2022 Build Tools or full IDE |
| **NuGet CLI** | Required for auto-downloading native runtime DLLs. Install via `winget install Microsoft.NuGet` |

## Building from Source

### 0. Open an x64 developer environment

All commands below must run in a shell that has the x64 MSVC toolchain on the PATH.
Choose **one** of the following:

| Method | How to open |
|---|---|
| **Developer Command Prompt** | Start Menu → *"x64 Native Tools Command Prompt for VS 2022"* |
| **Developer PowerShell** | Start Menu → *"Developer PowerShell for VS 2022"* |
| **Inside an existing cmd** | Run `"<VS_INSTALL>\VC\Auxiliary\Build\vcvars64.bat"` where `<VS_INSTALL>` is your Visual Studio installation path (e.g. `C:\Program Files\Microsoft Visual Studio\2022\Enterprise`) |
| **VS Code terminal** | Open the project folder in VS Code with the CMake Tools extension; it configures the environment automatically |

Verify by running `cl.exe` — the banner should say **x64**.

### 1. Clone & navigate

```bash
git clone https://github.com/microsoft/Foundry-Local.git
cd Foundry-Local/sdk/cpp
```

### 2. Configure (CMake + vcpkg)

```bash
cmake --preset x64-debug
```

This uses the `x64-debug` preset which:
- Uses the **Ninja** generator
- Resolves C++ dependencies via **vcpkg** (`nlohmann-json`, `ms-gsl`, `gtest`)
- Builds with the `x64-windows-static-md` triplet
- Auto-downloads native runtime DLLs via **NuGet**:
  - `Microsoft.AI.Foundry.Local.Core` (1.2.1) — Foundry Local core runtime
  - `Microsoft.ML.OnnxRuntime.Foundry` (1.26.0) — ONNX Runtime
  - `Microsoft.ML.OnnxRuntimeGenAI.Foundry` (0.14.1) — ONNX Runtime GenAI

NuGet packages are cached in `out/build/<preset>/_native_deps/` and only downloaded on first configure. Runtime DLLs are automatically copied next to executables via post-build steps.

### 3. Build

```bash
cmake --build --preset x64-debug
```

### Release build

```bash
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```

## Quick Start

```cpp
#include "foundry_local.h"
#include <iostream>

using namespace foundry_local;

int main() {
    // 1. Create the manager
    Manager::Create({"MyApp"});
    auto& manager = Manager::Instance();

    // 2. Get a model from the catalog
    auto& catalog = manager.GetCatalog();
    auto* model = catalog.GetModel("phi-3.5-mini");
    if (!model) return 1;

    // 3. Download (if needed) and load
    model->Download();
    model->Load();

    // 4. Chat
    OpenAIChatClient chat(*model);
    std::vector<ChatMessage> messages = {{"user", "Hello!"}};
    ChatSettings settings;
    settings.max_tokens = 128;

    auto response = chat.CompleteChat(messages, settings);
    if (!response.choices.empty() && response.choices[0].message) {
        std::cout << response.choices[0].message->content << "\n";
    }

    // 5. Cleanup
    model->Unload();
    Manager::Destroy();
}
```

### Vision Sample (Responses API)

A complete vision sample is included at `sample/web-server-responses-vision/`. It demonstrates image understanding using the Responses API with streaming via cURL.

Build and run from the SDK root:

```bash
cmake --preset x64-debug
cmake --build --preset x64-debug --target WebServerResponsesVision
.\out\build\x64-debug\WebServerResponsesVision.exe qwen3.5-0.8b
```

Or build standalone from the sample directory:

```bash
cd sample/web-server-responses-vision
cmake --preset x64-debug
cmake --build --preset x64-debug
.\out\build\x64-debug\web-server-responses-vision.exe qwen3.5-0.8b
```

See [sample/web-server-responses-vision/README.md](sample/web-server-responses-vision/README.md) for full details.

## Usage

### Initialization

`Manager` is a singleton. Call `Create` once at startup:

```cpp
Manager::Create(Configuration{"MyApp"}, &myLogger);
```

Access it anywhere afterward via `Manager::Instance()`. Check `Manager::IsInitialized()` to verify creation.

Call `Manager::Destroy()` to perform deterministic cleanup when done.

### Catalog

The catalog lists all models known to the Foundry Local Core:

```cpp
auto& catalog = Manager::Instance().GetCatalog();

// List all available models
auto models = catalog.GetModels();
for (auto* m : models)
    std::cout << m->GetAlias() << " — " << m->GetId() << "\n";

// Get a specific model by alias
auto* model = catalog.GetModel("phi-3.5-mini");

// Get a specific variant by its unique model ID
auto* variant = catalog.GetModelVariant("phi-3.5-mini-generic-gpu-4");

// List models already downloaded to the local cache
auto cached = catalog.GetCachedModels();

// List models currently loaded in memory
auto loaded = catalog.GetLoadedModels();
```

### Model Lifecycle

Each model may have multiple variants (different quantizations, hardware targets). The SDK auto-selects the best variant, or you can pick one.

```cpp
// Check and select variants
if (auto* concrete = dynamic_cast<Model*>(model)) {
    for (const auto& v : concrete->GetVariants()) {
        std::cout << v.GetId() << " (cached: " << v.IsCached() << ")\n";
    }
    // Switch to a specific variant (e.g., CPU)
    for (const auto& variant : concrete->GetVariants()) {
        if (variant.GetInfo().runtime &&
            variant.GetInfo().runtime->device_type == DeviceType::CPU) {
            concrete->SelectVariant(variant);
            break;
        }
    }
}
```

Download, load, and unload:

```cpp
// Download with progress reporting
model->Download([](float progress) {
    std::cout << "Download: " << progress << "%\n";
    return true;
});

// Load into memory
model->Load();

// Unload when done
model->Unload();

// Remove from local cache entirely
model->RemoveFromCache();
```

### Chat Completions

```cpp
OpenAIChatClient chat(*model);

std::vector<ChatMessage> messages = {
    {"system", "You are a helpful assistant."},
    {"user", "Explain async/await in C#."}
};
ChatSettings settings;

auto response = chat.CompleteChat(messages, settings);
if (!response.choices.empty() && response.choices[0].message) {
    std::cout << response.choices[0].message->content << "\n";
}
```

### Streaming

Use a callback for token-by-token output:

```cpp
chat.CompleteChatStreaming(messages, settings, [](const ChatCompletionCreateResponse& chunk) {
    if (!chunk.choices.empty() && chunk.choices[0].delta) {
        std::cout << chunk.choices[0].delta->content << std::flush;
    }
});
```

### Chat Settings

Tune generation parameters per request:

```cpp
ChatSettings settings;
settings.temperature = 0.7f;
settings.max_tokens = 256;
settings.top_p = 0.9f;
settings.frequency_penalty = 0.5f;
```

### Audio Transcription

```cpp
OpenAIAudioClient audio(*model);

// One-shot transcription
auto result = audio.TranscribeAudio(R"(C:\path\to\audio.wav)");
std::cout << result.text << "\n";

// Streaming transcription
audio.TranscribeAudioStreaming(R"(C:\path\to\audio.wav)", [](const AudioCreateTranscriptionResponse& chunk) {
    std::cout << chunk.text;
});
```

### Tool Calling

See `sample/main.cpp` (Example 5) for a full tool-calling walkthrough.

### Web Service

Start an OpenAI-compatible REST endpoint for use by external tools or processes:

```cpp
Configuration config{"MyApp"};
config.web = WebServiceConfig{ "http://127.0.0.1:5000" };
Manager::Create(std::move(config));

Manager::Instance().StartWebService();
auto urls = Manager::Instance().GetWebServiceEndpoints();
for (const auto& url : urls)
    std::cout << "Listening on: " << url << "\n";

// ... use the service ...

Manager::Instance().StopWebService();
```

### Execution Providers

Discover and download execution providers for hardware acceleration:

```cpp
// Discover available EPs
auto eps = manager.DiscoverEps();
for (const auto& ep : eps) {
    std::cout << ep.name << " — registered: " << (ep.is_registered ? "yes" : "no") << "\n";
}

// Download and register all EPs with progress
std::string currentEp;
auto result = manager.DownloadAndRegisterEps([&](const std::string& epName, double percent) {
    if (epName != currentEp) {
        if (!currentEp.empty()) std::cout << "\n";
        currentEp = epName;
    }
    std::cout << "\r  " << epName << "  " << percent << "%" << std::flush;
});
std::cout << "\n";

// Or download specific EPs only
auto result2 = manager.DownloadAndRegisterEps({"WebGpuExecutionProvider"});

// Check results
if (result.success) {
    for (const auto& ep : result.registered_eps)
        std::cout << "Registered: " << ep << "\n";
}
```

### Using the Prebuilt SDK (Zip)

#### Creating the zip

Build and install the SDK to produce the redistributable layout:

```bash
cd sdk/cpp

# Release build
cmake --preset x64-release
cmake --build --preset x64-release
cmake --install out/build/x64-release --prefix out/foundry-local-cpp-sdk

# Optional: also install Debug lib for consumers who need Debug builds
cmake --preset x64-debug
cmake --build --preset x64-debug
cmake --install out/build/x64-debug --prefix out/foundry-local-cpp-sdk
```

This creates:

```
out/foundry-local-cpp-sdk/
├── include/          # Public headers
├── lib/CppSdk.lib   # Prebuilt static library
├── bin/              # Runtime DLLs (Core, OnnxRuntime, OnnxRuntimeGenAI)
├── cmake/            # FoundryLocalConfig.cmake
└── README.md
```

Zip the `out/foundry-local-cpp-sdk/` folder and distribute.

#### Using the zip in your project

1. Unzip to a folder (e.g. `foundry-local-cpp-sdk/`)
2. In your `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my-app)

set(CMAKE_CXX_STANDARD 17)
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/foundry-local-cpp-sdk")
find_package(FoundryLocal REQUIRED)

add_executable(my-app main.cpp)
target_link_libraries(my-app PRIVATE FoundryLocal::FoundryLocal)

# Auto-copies Core DLL, ORT DLLs next to the exe
fl_copy_runtime_dlls(my-app)
```

3. Create a `vcpkg.json` with the required transitive dependencies:

```json
{
  "dependencies": ["nlohmann-json", "ms-gsl"]
}
```

4. Build:

```bash
cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

> **Note:** Match your build type to the SDK's. If the zip only contains a Release lib, build your project in Release (`-DCMAKE_BUILD_TYPE=Release`) to avoid MSVC runtime-library mismatches. If both Debug and Release libs are included, CMake selects the correct one automatically.

### Using the SDK from Source

Include the SDK via `add_subdirectory` (e.g. from the repo):

```cmake
add_subdirectory(path/to/sdk/cpp ${CMAKE_CURRENT_BINARY_DIR}/CppSdk)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE CppSdk)
fl_copy_runtime_dlls(my_app)
```

## Configuration

| Property | Type | Default | Description |
|---|---|---|---|
| `app_name` | `std::string` | (required) | Your application name |
| `app_data_dir` | `optional<path>` | `~/.{app_name}` | Application data directory |
| `model_cache_dir` | `optional<path>` | `{app_data_dir}/cache/models` | Where models are stored locally |
| `logs_dir` | `optional<path>` | `{app_data_dir}/logs` | Log output directory |
| `log_level` | `LogLevel` | `Warning` | Verbose, Debug, Information, Warning, Error, Fatal |
| `web` | `optional<WebServiceConfig>` | `nullopt` | Web service configuration (see below) |
| `additional_settings` | `optional<unordered_map>` | `nullopt` | Extra key-value settings passed to Core |

**WebServiceConfig**

| Property | Type | Default | Description |
|---|---|---|---|
| `urls` | `optional<string>` | `127.0.0.1:0` | Bind address; semicolon-separated for multiple |
| `external_url` | `optional<string>` | `nullopt` | URI for accessing the web service in a separate process |

## API Reference

Key types:

| Type | Description |
|---|---|
| `Manager` | Singleton entry point — create, catalog, web service, EP management |
| `Configuration` | Initialization settings |
| `Catalog` | Model catalog — list, search, filter |
| `IModel` | Model interface — identity, metadata, lifecycle |
| `Model` | Model with variant selection (implements `IModel`) |
| `ModelVariant` | A specific variant of a model (implements `IModel`) |
| `OpenAIChatClient` | Chat completions (sync + streaming) |
| `OpenAIAudioClient` | Audio transcription (sync + streaming) |
| `EpInfo` | Execution provider discovery info (name, registration status) |
| `EpDownloadResult` | Result of EP download/registration (success, registered/failed EPs) |
| `ChatSettings` | Chat generation parameters |
| `ModelInfo` | Full model metadata record |

## Tests

```bash
ctest --preset x64-debug
```

Or run the test executable directly:

```bash
.\out\build\x64-debug\CppSdkTests.exe
```

## Project Structure

```
sdk/cpp/
├── include/                  # Public headers
│   ├── foundry_local.h       # Umbrella header (include this)
│   ├── configuration.h       # Configuration struct
│   ├── foundry_local_manager.h  # Manager singleton + EP types
│   ├── catalog.h             # Model catalog
│   ├── model.h               # Model & ModelVariant
│   ├── logger.h              # ILogger interface
│   └── openai/
│       ├── chat_client.h     # Chat completion client
│       ├── audio_client.h    # Audio transcription client
│       └── tool_types.h      # Tool calling types
├── src/                      # Private implementation
├── sample/
│   ├── main.cpp              # Sample application
│   └── web-server-responses-vision/  # Vision sample (Responses API)
├── test/                     # Unit & E2E tests (GTest)
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json                # vcpkg dependencies
└── vcpkg-configuration.json
```

## Troubleshooting

| Error | Cause | Fix |
|---|---|---|
| `DML provider requested, but GenAI has not been built with DML support` | GPU variant selected but ONNX Runtime GenAI lacks DML | Select a CPU variant or update Foundry Local |
| `OgaGenerator_TokenCount not found in onnxruntime-genai` | Version mismatch between Foundry Local components | Update NuGet package versions in CMakeLists.txt |
| `API version [N] is not available` | ONNX Runtime version too old for the Foundry Local service | Update NuGet package versions in CMakeLists.txt |
| `nuget.exe not found on PATH` | NuGet CLI not installed | Install via `winget install Microsoft.NuGet` |
| `Failed to load shared library: Microsoft.AI.Foundry.Local.Core.dll` | Runtime DLLs not next to executable | Reconfigure with `cmake --preset x64-debug` to re-download NuGet packages, then rebuild |
| NuGet packages not installed or DLLs not copied correctly | Stale or corrupted build cache | Delete the `out` folder (`rmdir /s /q out`) and reconfigure from scratch: `cmake --preset x64-debug && cmake --build --preset x64-debug` |

## License

Licensed under the MIT License.
