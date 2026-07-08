# C++ Port Developer Guide

> Captures the architectural decisions, naming changes, and behavioral differences between
> the original C# implementation and the new C++ SDK. Intended for developers familiar with
> the C# codebase who need to understand how and why things changed.

## Source Material

The C++ SDK was ported from two C# codebases. The branch was created on **March 10, 2025**.

| C# Source | Commit / Location | What Was Ported |
|-----------|-------------------|-----------------|
| **FoundryLocalCore** | `neutron.main` @ `611cb028d4f5f83638e994105bd161741db159b4` — `src/FoundryLocalCore/Core/` | Core manager, catalog, model management, configuration, tool calling, Responses API, web service |
| **ONNX Inferencing** | `neutron.main` — `src/Service/Providers/Onnx/` | OnnxLoadedModel, OnnxChatGenerator, GenAIConfig, EP detection |
| **Downloader** | `neutron.main` — `src/Downloader/` | Azure blob download, model registry client, inference model writer |
| **C# SDK** | `fl.sdk/sdk/cs/src` → copied to `sdk_v2/cs/` | Updated to use the new C++ native library via P/Invoke |

All features from `FoundryLocalCore` should be replicated in the C++ port. The C# SDK v2
(`sdk_v2/cs/`) consumes the C++ library through a versioned C ABI (`foundry_local_c.h`).

---

## Architecture Overview

### C# Architecture (Original)

```
FoundryLocalCore (singleton, DI container)
├── IModelCatalog<AzureFoundryLocalModel>  (AzureModelCatalog, AggregateModelCatalog)
├── IModelManager                          (ModelManager → ConcurrentDict<id, OnnxLoadedModel>)
├── ChatClient                             (per-model, stateless per-request)
├── AudioClient                            (per-model, stateless)
├── IEpDetector                            (EpDetector + bootstrappers)
├── IResponseStore                         (InMemoryResponseStore)
├── ServiceManager                         (ASP.NET Core web server)
└── Download providers                     (Azure Blob, HuggingFace, etc.)
```

### C++ Architecture (Port)

```
Manager (singleton, explicit Create/Destroy lifecycle)
├── ICatalog (BaseModelCatalog → AzureModelCatalog, LocalModelScanner)
├── ModelLoadManager (mutex-guarded map<id, unique_ptr<GenAIModelInstance>>)
├── Session/ChatSession/AudioSession (stateful, owns conversation history)
├── IEpDetector (EpDetector — real detection + CUDA bootstrapping)
├── ResponseStore (LRU cache for OAI Responses API)
├── WebService (oatpp HTTP server)
│   ├── ChatCompletionsHandler (OAI Chat Completions endpoint)
│   ├── AudioTranscriptionsHandler (OAI Audio Transcriptions endpoint)
│   ├── ResponsesHandler (OAI Responses API endpoint — create/get/list/delete)
│   └── ModelsHandlers (OAI Models API endpoint)
├── DownloadManager (registry + blob download)
└── ITelemetry (stubbed — interface in place)
```

### Public API Layers

The C++ SDK exposes three layers:

1. **C ABI** (`foundry_local_c.h`) — Versioned vtable-based API with opaque handles.
   Only two exported symbols: `FoundryLocalGetApi(version)` and `FoundryLocalGetVersionString()`.
2. **C++ Wrapper** (`foundry_local_cpp.h`) — Header-only RAII wrappers over the C API.
3. **Internal Implementation** (`src/`) — The actual business logic.

The C# original had no equivalent layering — it was a single managed assembly.

---

## Major Architectural Differences

### 1. Session Concept (New in C++)

**C# (Original):** No session object. Each chat/audio request is **stateless**. A new
`OnnxChatGenerator` is created per-request. Conversation history lives in the request JSON
and is re-tokenized on every call.

**C++ (Port):** Explicit `Session` base class with `ChatSession` and `AudioSession`
subclasses. `ChatSession` maintains conversation history as a vector of `MessageItem`
objects and carries generation parameters across turns.

| Aspect | C# | C++ |
|--------|-----|-----|
| History | In request JSON, ephemeral | `ChatSession::history_` vector, persistent |
| Generator lifecycle | Created + destroyed per-request | Created per-request, but session persists. TODO: Refine |
| Multi-turn | Client re-sends full history | Session accumulates turns |
| Tool calling | Single-turn tool defs in request | `Session::AddToolDefinition()` persists across turns |
| Streaming | `IAsyncEnumerable<T>` (pull) | `StreamingCallbackFn` callback (push) |

**Why:** The session abstraction supports the C API's handle-based lifecycle (create
session → send requests → destroy session) and enables future continuous decoding where the
KV cache can be reused across turns. It also makes the Responses API's
`previous_response_id` chaining more natural as we can cache a Session using the id as the key.

**Generator caching in C#:** The Foundry Local branch (`611cb028`) did not have generator
caching — every request created and disposed a fresh `OnnxChatGenerator`. However, the
`main` branch of `neutron-server` implemented **continuous decoding via `GeneratorCache`**
(files: `GeneratorCache.cs`, `RefCountedChatGenerator.cs` in `Service/Providers/Onnx/`).
This was due to be ported to the Foundry Local branch. The mechanism works as follows:

1. **Cache structure:** `GeneratorCache` (one per `OpenAIServiceProviderOnnx`) holds a
   single `RefCountedChatGenerator` — a ref-counted wrapper around `OnnxChatGenerator`.
   It tracks the model name and a SHA-256 hash of the tokenized prompt content
   (`_cachedHistoryHash`).

2. **Cache key:** The hash is computed from the **templated and encoded prompt string**
   (the output of `Tokenizer.ApplyChatTemplate()` on the message history). This is the
   text *after* template application but *before* tokenization into token IDs.

3. **Lookup — `UpdateCachedWithNewRequestIfMatch()`:** On each new request, the cache
   strips the last message (the new user/tool prompt), applies the chat template to the
   remaining history, and hashes it. If it matches `_cachedHistoryHash` **and** the model
   name matches **and** the cached generator has `RefCount == 1` (nobody else is using
   it), the cache returns the existing generator. Only the new prompt tokens are appended
   via `Generator.AppendTokenSequences()`, reusing the existing KV cache.

4. **Cache miss — new generator:** If no match, a new `OnnxChatGenerator` is created from
   scratch (full tokenization + `Generator` construction) and wrapped in
   `RefCountedChatGenerator`. The cache replaces its entry via `Set()`.

5. **Post-generation update — `UpdateWithOutput()`:** After generation completes (both
   streaming and non-streaming), the generated assistant output is appended to the request
   messages and the cache hash is recomputed. This prepares the cache for the next turn.

6. **Eviction:** The cache is disposed when the model is unloaded
   (`DisposeGeneratorCache(modelName)`). Only one generator is cached at a time — new
   entries replace old ones, decrementing the previous generator's ref count.

7. **EP exclusions:** Continuous decoding is disabled for QNN and Vitis AI execution
   providers (their ORT backends don't support `AppendTokenSequences`).

**Ref counting:** `RefCountedChatGenerator` wraps `OnnxChatGenerator` with a ref count.
The cache holds one ref; the active request holds another (via `AddRef()`). When the
request completes and disposes its reference, the generator stays alive in the cache. Only
when the cache replaces or disposes the entry (dropping the last ref) is the underlying
`Generator` actually destroyed.

**C++ equivalent:** The C++ `ChatSession` architecture subsumes this pattern. `ChatSession`
persists across turns and maintains conversation history in `history_`. When session
caching is implemented in the web service layer, a `ChatSession` will be matched to
incoming Chat Completions requests by hashing the input messages, and to Responses API
requests by `previous_response_id`. The session owns its generator state, so the
ref-counting dance is unnecessary — the session's lifetime *is* the generator's lifetime.
See the "Session caching" TODO in `State of the Branch.md` for current status.

**Source mapping:**
- `ChatClient.HandleRequestAsync()` → `ChatSession::ProcessRequest()`
- `ChatClient.HandleStreamRequestAsync()` → `ChatSession::ProcessRequest()` with callback

### 2. Streaming Model

**C#:** Pull-based via `IAsyncEnumerable<ChatCompletionCreateResponse>`. The caller drives
iteration with `await foreach`.

**C++:** Push-based via `StreamingCallbackFn` (a `std::function<int(flStreamingCallbackData, void*)>`).
Each callback delivers an `flStreamingCallbackData` containing an `flItemQueue*` — one item
is pushed to the queue per callback invocation. The return value is 0 to continue or
non-zero to cancel. `StreamingThreadTracker` manages the lifecycle of the background
threads that drive generation to ensure clean shutdown.

**Why:** C++ has no equivalent of `IAsyncEnumerable`. Callbacks are the natural C++
pattern and map cleanly to the C ABI's function-pointer-based streaming interface.

### 3. Model Instance Naming

| C# | C++ | Notes |
|----|-----|-------|
| `LoadedModelBase` | *(no equivalent)* | C++ doesn't need a generic base — only ONNX/GenAI models currently |
| `OnnxLoadedModel` | `GenAIModelInstance` | Renamed to reflect ORT GenAI specificity |
| `ModelManager` | `ModelLoadManager` | Renamed to clarify it handles load/unload, not catalog |

`GenAIModelInstance` owns the same resources as `OnnxLoadedModel`:
- `OgaModel` (the ORT GenAI model)
- `OgaTokenizer` (normal variant)
- `OgaTokenizer` (special-tokens variant, for tool calling)
- `OgaMultiModalProcessor` (optional, for vision models)
- Last activity timestamp (for idle unload policy)

Both are move-only / non-copyable. C# uses `IDisposable`; C++ uses RAII via `unique_ptr`.

### 4. Catalog Architecture

| C# | C++ | Notes |
|----|-----|-------|
| `IModelCatalog<T>` generic interface | `ICatalog` non-generic interface | C++ drops the generic; all catalogs produce `Model` |
| `BaseModelCatalog<T>` | `BaseModelCatalog` | Same role: lazy population, indexed lookup |
| `AggregateModelCatalog<T>` | *(not ported)* | C++ uses a single catalog with multiple sources internally |
| `CachedInfo` struct | `ModelIndex` (atomic `shared_ptr`) | C++ uses lock-free index swap for concurrent reads |
| `AsyncLock` | `std::mutex` + `std::lock_guard` | Different concurrency primitives |

C++ catalog uses **three indices** (by id, by alias, by name) stored in an
`atomic<shared_ptr<ModelIndex>>` that is rebuilt and swapped atomically on refresh. This
gives lock-free reads during catalog queries. The C# version uses `AsyncLock` around
reads/writes.

The C++ `Model` class has two modes:
- **Leaf:** Single model variant with its own `ModelInfo`
- **Container:** Multi-variant wrapper that delegates property access to a selected variant

### 5. Web Service Framework

| C# | C++ |
|----|-----|
| ASP.NET Core (`WebApplication`) | oatpp (standalone C++ HTTP framework) |
| Dependency injection via `IServiceProvider` | `ServiceContext` struct with non-owning references |
| Middleware pipeline | Direct handler dispatch |
| `IAsyncEnumerable` streaming | SSE via `ChunkedResponse` + background thread |

The C++ `ServiceContext` is a plain struct holding references to the Manager's subsystems
(catalog, model_load_manager, response_store, logger, telemetry, thread_tracker). It's
passed to all handlers on construction. This replaces the DI container pattern.

### 6. Contract Types

**C# (Original):** Uses the `Betalgo.Ranul.OpenAI` NuGet package for OpenAI request/response
types. Responses API types are custom in `Core/Responses/Contracts/`.

**C++ (Port):** All contract types are hand-defined structs with ADL `from_json`/`to_json`
serialization (using `nlohmann::json`).

| Contract Area | C++ Location |
|---------------|-------------|
| Chat Completions | `src/contracts/chat_completions.h` |
| Audio Transcriptions | `src/contracts/audio_transcriptions.h` |
| Responses API | `src/contracts/responses.h` + `responses_json.cc` |
| Responses converter | `src/inferencing/generative/openresponses/response_converter.h` |

**Design rule:** Contract types use typed structs, not raw JSON. The `ResponseConverter`
namespace provides pure functions to bridge between OpenAI Responses API types and the
internal `Request`/`Response` domain types.

### 7. Item System (New in C++)

The C# original does **not** have an `Item` type system. Chat messages are
`ChatCompletionCreateRequest.Messages` (OpenAI types). Audio data is passed as byte arrays.

The C++ port introduces a **discriminated union Item hierarchy** to provide a uniform data
model across the C API boundary:

| Item Type | Discriminator | Purpose |
|-----------|--------------|---------|
| `TextItem` | TEXT | Plain text, OpenAI Json (legacy SDK support) |
| `MessageItem` | MESSAGE | Chat message (role + content) |
| `AudioItem` | AUDIO | Audio file/stream data |
| `ImageItem` | IMAGE | Image data |
| `TensorItem` | TENSOR | Multi-dimensional numeric arrays |
| `BytesItem` | BYTES | Raw binary |
| `ToolCallItem` | TOOL_CALL | LLM tool invocation |
| `ToolResultItem` | TOOL_RESULT | Tool execution result |
| `ItemQueue` | QUEUE | Thread-safe streaming queue |

Items are the currency of `Request` and `Response` objects. The Session processes
`Request` → `Response`, each containing `vector<unique_ptr<Item>>`.

This design supports:
- The C ABI's opaque `flItem` handle with type-safe accessors
- Predictive inference (tensors) and generative inference (messages) through the same API
- Zero-copy streaming via `ItemQueue` with pinned memory from managed callers

#### ItemQueue — Streaming Items as Input or Output

`ItemQueue` is a thread-safe queue that is itself an `Item` (type `QUEUE`). Because it
participates in the item system, it can appear anywhere a regular item can — as an input
item in a `Request` or as the delivery mechanism in a streaming callback. Ownership of each
item transfers through the queue: the consumer that pops an item is responsible for
deleting it.

**Streaming output:** The `flStreamingCallbackData` passed to the streaming callback
contains an `flItemQueue*`. Each callback invocation pushes one item to the queue. This
guarantees ordering — items arrive in generation order regardless of threading. The queue
carries the same item types as the overall `Response` (e.g., `MessageItem` for text
tokens, `ToolCallItem` for tool invocations), so streaming output is a per-item view of
the final response.

**Streaming input:** A `Request` can contain an `ItemQueue` as one of its input items.
This enables scenarios like realtime audio:

1. The caller creates a `Request` with an `ItemQueue` input item.
2. An `AudioItem` is pushed to the queue first to signal the format/type of data that will
   be streamed.
3. As audio data becomes available (e.g., from a microphone), the caller pushes `BytesItem`
   instances to the queue.
4. When input is complete, the caller calls `ItemQueue_MarkFinished`.
5. The session processing the request reads from the queue, generating tokens as new data
   arrives, and completes the request once the queue is empty and marked as finished.

This is a key design point: `ItemQueue` unifies streaming input and streaming output
through the same item-based abstraction, avoiding separate streaming APIs for different
modalities.

### 8. Ownership & Lifecycle

| Concept | C# | C++ |
|---------|-----|-----|
| Manager singleton | `FoundryLocalCore._instance` + `AsyncLock` | `Manager::instance_` + `std::mutex` |
| Model references | `ConcurrentDictionary` values, GC cleanup | `map<string, unique_ptr<GenAIModelInstance>>`, explicit unload |
| Catalog entries | Class instances, GC cleanup | `vector<unique_ptr<Model>>` with stable pointers |
| Per-request state | `OnnxChatGenerator` created/disposed per call | Per-request generator, but `Session` persists |
| Streaming threads | `Task` + `CancellationToken` | `std::thread` + `StreamingThreadTracker` for join-on-shutdown |

C++ uses RAII throughout. The Manager documents its destruction order explicitly to prevent
dangling references between subsystems.

### 9. Configuration

Both use the same placeholder substitution (`{home}`, `{appname}`, `{appdata}`).

| C# | C++ |
|----|-----|
| `appsettings.json` + programmatic dict | Struct-based `Configuration` |
| `IConfiguration` (Microsoft.Extensions) | Direct struct fields |
| Properties with getters/setters | `std::optional<std::string>` fields |
| `NumModelDownloadThreads` (64 default, 8 Android) | *(not yet configurable)* |

C++ `Configuration::Validate()` expands all placeholders in-place and sets sensible defaults.

### 10. Download System

| C# | C++ |
|----|-----|
| Multiple download providers (Azure, HuggingFace, NIM, WCR) | Azure Blob only (primary path) |
| `IDownloadClientProvider` abstraction | `DownloadManager` with `BlobDownloader` + `ModelRegistryClient` |
| `IProgress<(fileName, percent)>` | `std::function<void(float)>` progress callback |
| Concurrent chunk downloads (configurable) | Implemented in `BlobDownloader` |
| `BlobDownloadState` for resumption | *(to be verified)* |
| `CrossProcessFileLock` for multi-process safety | *(to be verified)* |

The C++ download path is simpler because only Azure Blob sources are supported.
HuggingFace, NIM, and WCR providers were not ported.

---

## Name Mapping Reference

### Core Types

| C# (FoundryLocalCore) | C++ | Notes |
|------------------------|-----|-------|
| `FoundryLocalCore` | `Manager` | Singleton orchestrator |
| `IFoundryLocalCore` | *(no interface)* | C++ uses concrete Manager |
| `Configuration` | `Configuration` | Same name, struct vs class |
| `IModelCatalog<T>` | `ICatalog` | Non-generic |
| `BaseModelCatalog<T>` | `BaseModelCatalog` | Non-generic |
| `AzureModelCatalog` | `AzureModelCatalog` | Same name |
| `AggregateModelCatalog<T>` | *(not ported)* | Single catalog with multiple sources |
| `ModelManager` / `IModelManager` | `ModelLoadManager` | Renamed for clarity |
| `ChatClient` | `ChatSession` | Stateful session vs stateless client |
| `AudioClient` | `AudioSession` | Stateful session vs stateless client |
| `CoreException` | `fl::Exception` | Namespaced |

### Inferencing Types

| C# (Service/Providers/Onnx) | C++ | Notes |
|-----------------------------|-----|-------|
| `OnnxLoadedModel` | `GenAIModelInstance` | Renamed |
| `LoadedModelBase` | *(not needed)* | Only one model type in C++ |
| `OnnxChatGenerator` | `OnnxChatGenerator` | Same name, different lifecycle (C++ is per-request internal) |
| `ChatGenerator` (abstract) | `ChatGenerator` (interface) | Same role |
| `OnnxAudioGenerator` | `OnnxAudioGenerator` | Same name |
| `GenAIConfig` | `GenAIConfig` | Same name |
| `OnnxEP` (enum) | `ExecutionProvider` (enum) | Renamed, same values |

### Web Service Types

| C# (FoundryLocalCore/Service) | C++ | Notes |
|-------------------------------|-----|-------|
| `ServiceManager` | `WebService` | Renamed |
| `OpenAIApiProvider` | `ChatCompletionsHandler` | Split per-endpoint |
| `ModelApiProvider` | `ModelsHandlers` | Renamed |
| `ResponsesApi` | `ResponsesHandler` | Renamed |
| `ResponsesApiProvider` | *(merged into handler)* | |
| `WebApplicationFactory` | *(oatpp setup in WebService)* | Framework change |

### Responses API Types

| C# (Responses/Contracts) | C++ | Notes |
|--------------------------|-----|-------|
| `ResponseCreateRequest` | `ResponseCreateParams` | Renamed |
| `Response` | `ResponseObject` | Renamed to avoid clash with `fl::Response` |
| `ResponseClient` | `ResponseConverter` (namespace) | Stateless functions vs class |
| `ResponseInputConverter` | *(merged into ResponseConverter)* | |
| `IResponseStore` | `ResponseStore` | Concrete LRU, no interface |
| `InMemoryResponseStore` | `ResponseStore` | Same impl, no interface |

### Download Types

| C# (Downloader) | C++ | Notes |
|-----------------|-----|-------|
| `DownloadModelAsync` (delegate) | `DownloadManager::DownloadModel()` | Method vs delegate |
| `BlobDownloadState` | `BlobDownloader` | Azure blob download impl |
| `ModelCatalog<T>` (download) | `ModelRegistryClient` | Registry lookup |
| `InferenceModelMetadata` | `InferenceModelWriter` | Writes model metadata to cache |
| `DownloadProvider` (enum) | *(not needed)* | Only Azure Blob supported |

### EP Detection Types

| C# (Detector) | C++ | Notes |
|---------------|-----|-------|
| `EpDetector` | `EpDetector` (implements `IEpDetector`) | Real detection via ORT `GetAvailableProviders()` + `GetEpDevices()` |
| `IEpBootstrapper` | `IEpBootstrapper` (interface) | Bootstrapping interface for EP package download/registration |
| `CudaEpBootstrapper` | `CudaEpBootstrapper` | Downloads CUDA EP zip, extracts, prepends to PATH, registers with ORT |
| `WinMLEpBootstrapper` | `WinMLEpBootstrapper` | Discovers WinML EPs via `Microsoft.Windows.AI.MachineLearning.dll` catalog API. WinML 2.x reg-free runtime, Windows 10 19H1+ (build 18362). |

---

## Features Not Yet Ported

| Feature | C# Location | Status | Notes |
|---------|-------------|--------|-------|
| ~~Real EP detection & bootstrapping~~ | `Service/Providers/Detector/` | **Done** | Real EP detection, CUDA bootstrapping (download/extract/register), `ModelLoadManager` EP guard. |
| Telemetry | Various (`ITelemetry` usage) | Stubbed | Interface in place, placeholder impl |
| ~~Audio realtime streaming~~ | `AudioStreamingSession.cs` | **Done** | `AudioItem` format descriptor + `ItemQueue` push pattern. `WaitAndPop` blocking read. C# SDK `LiveAudioTranscriptionSession` wraps native streaming. |
| Session caching (web service) | *(new requirement)* | TODO | Cache sessions for continuous decoding |
| Android SSL cert handling | `AndroidCertificateChecker.cs` | Not ported | Platform-specific |
| Embeddings | `EmbeddingsGenerator.cs` | Not ported | Lower priority |

---

## New Concepts in C++ (Not in C# Original)

### C ABI Layer

The C++ SDK exposes a stable, versioned C ABI through `foundry_local_c.h`:
- Opaque handle types (`flApi`, `flCatalog`, `flModel`, `flSession`, etc.)
- Vtable-based sub-APIs (`flCatalogApi`, `flModelApi`, `flInferenceApi`, etc.)
- Status/error returns (`flStatus`) instead of exceptions
- Explicit ownership transfer conventions

This enables language bindings (C#, Python, JS, Rust) without COM or .NET interop.

### Item Type System

The discriminated item hierarchy (`TextItem`, `MessageItem`, `AudioItem`, `TensorItem`,
etc.) is new. The C# original used OpenAI-specific types directly. The item system:
- Provides a uniform data model for the C ABI
- Supports both generative inference (messages, text) and predictive inference (tensors)
- Enables zero-copy data transfer via `ItemQueue` and memory pinning

### Predictive Inference

The `InferenceSession` class for traditional ML inference (tensor in → tensor out) is new
in C++. The C# original was generative-only (chat + audio).

### Response Store with LRU + Cursor Pagination

The C++ `ResponseStore` is a thread-safe LRU cache with cursor-based pagination support
for the Responses API list endpoint. The C# `InMemoryResponseStore` is simpler but
functionally equivalent.

### Streaming Thread Tracker

`StreamingThreadTracker` manages background threads spawned for SSE streaming responses.
It ensures all threads are joined during shutdown. The C# equivalent relies on
`CancellationToken` propagation through the ASP.NET pipeline.

---

## Build System Comparison

| Aspect | C# | C++ |
|--------|-----|-----|
| Build system | MSBuild / .csproj | CMake + vcpkg |
| Package manager | NuGet | vcpkg (`vcpkg.json` manifest) |
| Target standard | .NET 8/9 | C++20 |
| JSON library | `System.Text.Json` / `Betalgo.Ranul.OpenAI` | `nlohmann::json` |
| HTTP framework | ASP.NET Core | oatpp |
| Logging | Serilog | spdlog |
| Testing | xUnit / MSTest | Google Test |
| Azure storage | `Azure.Storage.Blobs` (NuGet) | `azure-storage-blobs-cpp` (vcpkg) |

### C++ Build Options

```cmake
FOUNDRY_LOCAL_BUILD_TESTS=ON       # Unit tests (default ON)
FOUNDRY_LOCAL_BUILD_EXAMPLES=ON    # Example programs (default ON)
FOUNDRY_LOCAL_BUILD_SERVICE=ON     # Web service (requires oatpp) (default ON)
```

---

## Testing Strategy

| Aspect | C# | C++ |
|--------|-----|-----|
| Framework | xUnit / MSTest | Google Test |
| Test count | *(varies by project)* | 659 internal + 86 SDK API = 745 |
| Coverage | *(varies)* | 83% |
| Test structure | Single test project per assembly | `test/internal_api/` + `test/sdk_api/` |
| Model testing | Real models in CI | `FOUNDRY_TEST_DATA_DIR` env var for shared test data |

C++ tests are split into:
- **`test/internal_api/`** — Tests internal implementation without the C ABI boundary
- **`test/sdk_api/`** — Tests the public C/C++ API surface, including web service endpoints

---

## File Organization Comparison

| Concern | C# Location | C++ Location |
|---------|-------------|-------------|
| Manager/Core | `Core/FoundryLocalCore.cs` | `src/manager.h`, `src/manager.cc` |
| Configuration | `Core/Configuration.cs` | `src/configuration.h`, `src/configuration.cc` |
| Catalog interface | `Core/IModelCatalog.cs` | `src/catalog.h` |
| Azure catalog | `Core/AzureModelCatalog.cs` | `src/catalog/azure_model_catalog.h/.cc` |
| Catalog base | `Core/BaseModelCatalog.cs` | `src/catalog/base_model_catalog.h/.cc` |
| Model loading | `Core/ModelManager.cs` | `src/inferencing/generative/model_load_manager.h/.cc` |
| Loaded model | `Onnx/OnnxLoadedModel.cs` | `src/inferencing/generative/genai_model_instance.h/.cc` |
| Chat generation | `Onnx/OnnxChatGenerator.cs` | `src/inferencing/generative/chat/onnx_chat_generator.h/.cc` |
| Audio generation | `Core/OnnxAudioGenerator.cs` | `src/inferencing/generative/audio/onnx_audio_generator.h/.cc` |
| GenAI config | `Onnx/GenAIConfig.cs` | `src/inferencing/generative/genai_config.h/.cc` |
| Chat completions | `Core/Service/OpenAIApiProvider.cs` | `src/service/chat_completions_handler.h/.cc` |
| Responses API | `Core/Responses/ResponsesApi.cs` | `src/service/responses_handler.h/.cc` |
| Responses contracts | `Core/Responses/Contracts/*.cs` | `src/contracts/responses.h`, `src/contracts/responses_json.cc` |
| Response converter | `Core/Responses/ResponseClient.cs` | `src/inferencing/generative/openresponses/response_converter.h/.cc` |
| Response store | `Core/Responses/InMemoryResponseStore.cs` | `src/inferencing/generative/openresponses/response_store.h/.cc` |
| Tool calling | *(inline in OnnxChatGenerator)* | `src/inferencing/generative/toolcalling/` (separate dir) |
| Chat templates | *(inline/config)* | `src/inferencing/generative/chat/chat_template.h/.cc` |
| Download | `Downloader/*.cs` | `src/download/download_manager.h/.cc`, `src/download/blob_downloader.h/.cc` |
| EP detection | `Detector/EpDetector.cs` | `src/ep_detection/ep_detector.h/.cc` |
| EP version logging | `Shared/RuntimeVersionInfo.cs` | `src/ep_detection/runtime_version_info.h/.cc` |
| Web service | `Core/Service/ServiceManager.cs` | `src/service/web_service.h/.cc` |
| C ABI binding | *(N/A — no C ABI)* | `src/c_api.h/.cc`, `src/c_api_types.h` |
| Items | *(N/A — OpenAI types directly)* | `src/items/*.h` |
| Sessions | *(N/A — stateless)* | `src/inferencing/session/session.h/.cc` |

---

## Behavioral Differences to Be Aware Of

### Streaming Response Format

**C#:** Returns `ChatCompletionCreateResponse` objects via `IAsyncEnumerable`, each chunk is a
complete response delta.

**C++:** Pushes `Response` objects via callback. The handler converts to SSE events
(chat completions format or Responses API semantic events) before sending to the client.

### Error Handling

**C#:** Throws exceptions (`CoreException`, standard .NET exceptions). ASP.NET catches and
returns HTTP error responses.

**C++:** Internal code throws `fl::Exception`. The C API boundary catches all exceptions
and converts to `flStatus` error codes. Web service handlers catch and return JSON error
responses.

### Thread Safety Model

**C#:** `ConcurrentDictionary` for model registry. `AsyncLock` for catalog. `lock` for
audio session factory.

**C++:** `std::mutex` + `std::lock_guard` for model registry and catalog mutations.
`atomic<shared_ptr>` for lock-free catalog index reads. Manager construction/destruction
is mutex-protected.

### Model Variant Selection

**C# (Original):** The FoundryLocalCore layer had no explicit variant abstraction — model
IDs contained EP hints (e.g., "cuda-gpu", "dml-gpu") and `ModelManager` parsed these to
determine which EP to use. Each ID was a flat entry in the catalog.

The **C# SDK** (`fl.sdk/sdk/cs/src`) added the variant concept in its public API layer:
- `IModel` — public interface exposing `Variants` and `SelectVariant()`
- `Model` (internal) — container that grouped `ModelVariant` instances sharing the same
  alias. Delegated all operations (`IsCached`, `IsLoaded`, `Download`, `Load`, etc.) to
  `SelectedVariant`. Auto-selected the highest-priority cached variant on construction.
- `ModelVariant` (internal) — leaf implementation of `IModel` wrapping a single catalog
  entry with its own model ID, device type, and EP.
- `Catalog` — iterated the flat model list from Core, created `ModelVariant` per entry,
  and grouped them into `Model` containers by alias (`_modelAliasToModel` dict).

This meant the variant grouping and selection logic lived in the SDK layer (C#), duplicated
per language SDK.

**C++ (Port):** The variant concept is **pushed down into the C++ core** so it exists in
one common place, available to all language bindings through the C ABI. Instead of separate
`Model` and `ModelVariant` types, the C++ `Model` class implements both roles:

- **Container mode** (`Model::MakeContainer`) — groups variants sharing the same alias.
  All property access (`Info`, `IsCached`, `IsLoaded`, etc.) delegates to
  `selected_variant_`. Exposes `Variants()` (all contained leaves) and `SelectVariant()`.
- **Leaf mode** (`Model::FromModelInfo`) — a single model variant with its own `ModelInfo`,
  local path, and cached/loaded state. `Variants()` returns `{this}`.

The `BaseModelCatalog` handles grouping: when populating from the Azure catalog, it creates
leaf `Model` instances per catalog entry, then groups those with matching aliases into
container `Model` instances. The three indices (by id, by alias, by name) point to the
appropriate level — alias lookups return containers, id lookups return leaves.

This means the C# SDK v2 (`sdk_v2/cs/`) no longer needs its own `Model`/`ModelVariant`
classes — it wraps the native `flModel` handle directly, and variant selection goes through
the C ABI (`Model_GetVariants`, `Model_SelectVariant`).
