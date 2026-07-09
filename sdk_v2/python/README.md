# Foundry Local Python SDK (v2)

The Foundry Local Python SDK is a native Python binding for the Foundry Local C++ SDK. It lets you discover, download, load, and run inference against local AI models — chat completions (streaming and non-streaming), tool calling, embeddings, and audio transcription — directly in-process via a [cffi](https://cffi.readthedocs.io/) binding to the Foundry Local native library. No separate service, no HTTP hop.

## Features

- **Model Catalog** – browse and search the Foundry Local model catalog
- **Model Management** – download, cache, load, and unload models
- **Chat Completions** – streaming and non-streaming, with first-class tool calling
- **Embeddings** – text embeddings via a typed tensor API
- **Audio Transcription** – speech-to-text (offline and live streaming)
- **Explicit EP Management** – discover, download, and register execution providers on demand
- **Built-in Web Service** – optional HTTP endpoint for multi-process scenarios
- **Native Performance** – cffi (API mode) binding to the Foundry Local native library

## Installation

```bash
pip install foundry-local-sdk
```

The wheel ships the Foundry Local native library — bundling the reg-free WinML 2.x runtime on Windows for hardware acceleration — and pulls the matching ONNX Runtime + ONNX Runtime GenAI runtime packages as dependencies.

### Building from source

The Python SDK is a cffi binding compiled against `sdk_v2/cpp/include/foundry_local/foundry_local_c.h`. The compiled extension calls into `foundry_local.{dll,so,dylib}` at runtime via `dlopen` — **the native library must already exist** before the wheel is usable. Build it first:

```bash
# Windows
sdk_v2\cpp\build.bat --skip_tests

# Linux / macOS
sdk_v2/cpp/build.sh --skip_tests
```

The output lands where `lib_loader.py` will discover it automatically:

- Windows (multi-config MSBuild): `sdk_v2/cpp/build/Windows/<Config>/bin/<Config>/foundry_local.dll`
- Linux: `sdk_v2/cpp/build/Linux/<Config>/bin/libfoundry_local.so`
- macOS: `sdk_v2/cpp/build/macOS/<Config>/bin/libfoundry_local.dylib`

To override the lookup, set `FOUNDRY_LOCAL_LIB_DIR` to a directory that contains `foundry_local.{dll,so,dylib}`.

Then build the wheel:

```bash
cd sdk_v2/python
python -m build --wheel
```

For editable installs during development:

```bash
pip install -e .
```

### Installing native runtime dependencies for development / CI

`foundry-local-install` is a convenience wrapper for end-user / CI environments that want the published wheel plus its ORT / ONNX Runtime GenAI runtime packages installed and verified in one step. It runs `pip install --upgrade foundry-local-sdk` from PyPI and then probes that `onnxruntime[_core]` and `onnxruntime_genai[_core]` import cleanly.

```bash
foundry-local-install

# Add --verbose to print resolved binary paths after installation.
```

> **Do not run this against a source-build / editable install.** It will overwrite your `pip install -e .` (or any locally-built wheel install) with the published PyPI version. The source-build flow above (`pip install -e .` or `pip install <local.whl>`) already pulls the matching ORT and GenAI runtime packages via pyproject dependencies — no extra step needed.

## Requirements

- Python 3.11 or newer (single `cp311-abi3` wheel works on every CPython ≥ 3.11)
- Windows (x64), Linux (x64), or macOS (arm64)

## Quick start

```python
from foundry_local_sdk import (
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    Request,
    RequestOptions,
    SearchOptions,
    TextItem,
)

# 1. Initialize
config = Configuration(app_name="MyApp")
FoundryLocalManager.initialize(config)
manager = FoundryLocalManager.instance

# 2. Pick and load a model
model = manager.catalog.get_model("qwen2.5-0.5b")
model.download(lambda pct: print(f"\rDownloading: {pct:.1f}%", end="", flush=True))
print()
model.load()

# 3. Run a chat request through a typed session
with ChatSession(model) as session:
    session.set_options(RequestOptions(search=SearchOptions(temperature=0.0, max_output_tokens=128)))

    with Request().add_item(MessageItem.user("Why is the sky blue?")) as req:
        with session.process_request(req) as response:
            for item in response:
                if isinstance(item, TextItem):
                    print(item.text)

# 4. Cleanup
model.unload()
```

Runnable end-to-end examples live under [`samples/python/`](https://github.com/microsoft/Foundry-Local/tree/main/samples/python).

## Usage

### Initialization

Create a `Configuration` and initialize the singleton `FoundryLocalManager`.

```python
from foundry_local_sdk import Configuration, FoundryLocalManager, LogLevel

config = Configuration(
    app_name="MyApp",
    model_cache_dir="/path/to/cache",                # optional
    log_level=LogLevel.INFORMATION,                   # optional (default: Warning)
)
FoundryLocalManager.initialize(config)
manager = FoundryLocalManager.instance
```

### Discovering models

```python
catalog = manager.catalog

# List all models in the catalog
models = catalog.list_models()

# Get a specific model by alias
model = catalog.get_model("qwen2.5-0.5b")

# Get a specific variant by ID
variant = catalog.get_model_variant("qwen2.5-0.5b-instruct-generic-cpu:4")

# Locally cached / currently loaded
cached = catalog.get_cached_models()
loaded = catalog.get_loaded_models()
```

### Inspecting model metadata

`IModel` exposes metadata properties from the catalog:

```python
model = catalog.get_model("phi-3.5-mini")

# Identity
print(model.id)             # e.g. "phi-3.5-mini-instruct-generic-gpu:3"
print(model.alias)          # e.g. "phi-3.5-mini"

# Context and token limits
print(model.context_length)  # e.g. 131072 (tokens), or None if unknown

# Modalities and capabilities
print(model.input_modalities)        # e.g. "text" or "text,image"
print(model.output_modalities)       # e.g. "text"
print(model.capabilities)            # e.g. "chat,completion"
print(model.supports_tool_calling)   # True, False, or None

# Cache / load state
print(model.is_cached)
print(model.is_loaded)
```

### Explicit EP management

```python
# Discover available EPs and registration status
eps = manager.discover_eps()
for ep in eps:
    print(f"{ep.name} - registered: {ep.is_registered}")

# Download and register all available EPs
result = manager.download_and_register_eps()
print(f"Success: {result.success}, Status: {result.status}")

# Download only specific EPs
result2 = manager.download_and_register_eps([eps[0].name])
```

#### Per-EP download progress

Pass a `progress_callback` to receive `(ep_name, percent)` updates as each EP downloads (`percent` is 0–100):

```python
current_ep = ""

def on_progress(ep_name: str, percent: float) -> None:
    global current_ep
    if ep_name != current_ep:
        if current_ep:
            print()
        current_ep = ep_name
    print(f"\r  {ep_name}  {percent:5.1f}%", end="", flush=True)

manager.download_and_register_eps(progress_callback=on_progress)
print()
```

Catalog access does not block on EP downloads. Call `download_and_register_eps()` when you need hardware-accelerated execution providers.

### Chat completions with `ChatSession`

```python
from foundry_local_sdk import (
    ChatSession, MessageItem, Request, RequestOptions, SearchOptions, TextItem,
)

model = manager.catalog.get_model("qwen2.5-0.5b")
model.load()

with ChatSession(model) as session:
    session.set_options(RequestOptions(search=SearchOptions(temperature=0.0, max_output_tokens=256)))

    # Non-streaming
    with Request().add_item(MessageItem.user("What is 7 multiplied by 6?")) as req:
        with session.process_request(req) as response:
            for item in response:
                if isinstance(item, TextItem):
                    print(item.text)

    # Streaming — yields Item instances as the model produces them
    session.set_streaming(True)
    with Request().add_item(MessageItem.user("Tell me a joke")) as req:
        for item in session.process_streaming_request(req):
            if isinstance(item, TextItem):
                print(item.text, end="", flush=True)
    print()

model.unload()
```

`ChatSession` is stateful across turns. `session.turn_count` reports how many requests have been processed; `session.undo_turns(n)` rewinds history.

### Multi-turn conversations

Each call to `process_request` extends the session's turn history. Build a new `Request` per turn:

```python
with ChatSession(model) as session:
    for prompt in ["Hi!", "What's your favorite color?", "Why?"]:
        with Request().add_item(MessageItem.user(prompt)) as req:
            with session.process_request(req) as resp:
                for item in resp:
                    if isinstance(item, TextItem):
                        print(f"> {prompt}\n{item.text}\n")
```

### Tool calling

Register tool definitions on the session, then watch for `ToolCallItem` in the response and reply with `ToolResultItem`:

```python
import json
from foundry_local_sdk import ToolCallItem, ToolResultItem

with ChatSession(model) as session:
    session.add_tool_definition(
        name="get_weather",
        description="Get the current weather for a city.",
        json_schema=json.dumps({
            "type": "object",
            "properties": {"city": {"type": "string"}},
            "required": ["city"],
        }),
    )

    with Request().add_item(MessageItem.user("What's the weather in Seattle?")) as req:
        with session.process_request(req) as resp:
            for item in resp:
                if isinstance(item, ToolCallItem):
                    args = json.loads(item.arguments)
                    result = {"temperature_c": 12, "conditions": "rain"}
                    # Send the result back on the next turn
                    with Request().add_item(
                        ToolResultItem(call_id=item.call_id, content=json.dumps(result))
                    ) as follow_up:
                        with session.process_request(follow_up) as final:
                            for it in final:
                                if isinstance(it, TextItem):
                                    print(it.text)
```

### Embeddings with `EmbeddingsSession`

`EmbeddingsSession` accepts `TextItem` inputs and returns one `TensorItem` per input containing the embedding vector. Sessions are stateless — reuse one session for many requests.

```python
from foundry_local_sdk import EmbeddingsSession, Request, TensorItem, TextItem

model = manager.catalog.get_model("qwen3-embedding-0.6b")
model.load()

with EmbeddingsSession(model) as session:
    # Single input
    with Request().add_item(TextItem("The quick brown fox")) as req:
        with session.process_request(req) as resp:
            tensor = next(it for it in resp if isinstance(it, TensorItem))
            print("Dimensions:", tensor.dimensions)
            print("First 5:", tensor.data[:5])

    # Batch input — one TextItem per string, one TensorItem out per input
    with Request() as req:
        for text in ["Machine learning", "Capital of France", "Rust language"]:
            req.add_item(TextItem(text))
        with session.process_request(req) as resp:
            for item in resp:
                if isinstance(item, TensorItem):
                    print(f"  dims={item.dimensions}")

model.unload()
```

### Audio transcription

`AudioSession` accepts `AudioItem` input (PCM bytes + sample rate / channels) and produces `TextItem` output. See [`samples/python/audio-transcription/`](https://github.com/microsoft/Foundry-Local/tree/main/samples/python/audio-transcription) and [`live-audio-transcription/`](https://github.com/microsoft/Foundry-Local/tree/main/samples/python/live-audio-transcription) for runnable end-to-end examples covering offline files and live PCM streaming through an `ItemQueue`.

### Web service (optional)

Start a built-in HTTP server for multi-process access:

```python
manager.start_web_service()
print(f"Listening on: {manager.urls}")

# ... use the service ...

manager.stop_web_service()
```

## API Reference

### Manager and configuration

| Class | Description |
|---|---|
| `Configuration` | SDK configuration (app name, cache dir, log level, web service settings) |
| `FoundryLocalManager` | Singleton entry point — initialization, catalog access, EP management, web service |
| `Catalog` | Model discovery — listing, lookup by alias / ID, cached and loaded queries |
| `IModel` | Model interface — identity, metadata, lifecycle (`download`, `load`, `unload`), variant selection |
| `EpInfo` | Discoverable execution provider info (`name`, `is_registered`) |
| `EpDownloadResult` | Result of EP download / registration (`success`, `status`, `registered_eps`, `failed_eps`) |
| `LogLevel` | Logging verbosity enum |

### Sessions

All sessions wrap a native `flSession*` and are context managers. Closing a session releases the native handle and aborts any in-flight streaming request.

| Class | Description |
|---|---|
| `Session` | Abstract base class. Provides `process_request`, `process_streaming_request`, `set_options`, `set_streaming`, and the context-manager / `_close` lifecycle. |
| `ChatSession` | For `chat-completion` and `vision-language-chat` models. Adds `add_tool_definition`, `turn_count`, `undo_turns`. |
| `EmbeddingsSession` | For `embeddings` models. Stateless — accepts `TextItem` inputs, returns one `TensorItem` per input. |
| `AudioSession` | For `automatic-speech-recognition` models. Accepts `AudioItem` input (and `ItemQueue` for live streaming), returns `TextItem`. |

Common session methods:

- `process_request(request) -> Response` — run synchronously, return the full response.
- `process_streaming_request(request) -> Iterator[Item]` — yield items as the model produces them. Requires `set_streaming(True)` first. Abandoning the iterator (`break`, exception, `gen.close()`) automatically cancels the request and joins the worker thread.
- `set_options(RequestOptions)` — apply session-level inference parameters (typed `SearchOptions` for sampling, optional `tool_choice`, and `additional_options` for passthrough).
- `set_streaming(enabled)` — install or remove the native streaming callback.

### Requests and responses

| Class | Description |
|---|---|
| `Request` | Owns an `flRequest*`. Build with `add_item(item)` (fluent — returns self). Use as a context manager so the native handle is released. `set_options(RequestOptions)` applies per-request overrides. |
| `Response` | Owns an `flResponse*`. Iterable over output items. Exposes `item_count`, `get_item(i)`, `finish_reason` (`FinishReason` enum), and `get_usage()` (`TokenUsage`). Read item data **inside** the response's `with` block — items returned by `get_item` borrow the response's handle. |
| `FinishReason` | `NONE`, `ERROR`, `STOP`, `LENGTH`, `TOOL_CALLS`. |
| `TokenUsage` | `prompt_tokens`, `completion_tokens`, `total_tokens`. |
| `RequestOptions` | Typed inference options passed to `set_options`. Wraps `search: SearchOptions` (sampling params: `temperature`, `top_p`, `top_k`, `max_output_tokens`, `frequency_penalty`, `presence_penalty`, `seed`, `early_stopping`, `do_sample`), `tool_choice: ToolChoice | None` (`AUTO`/`NONE`/`REQUIRED`), and `additional_options: dict[str, str]` as the passthrough escape hatch. |

### Items

`Item` is the wire-format type for everything that flows in or out of a session — message turns, individual modality parts, tool calls, embeddings.

| Class | Description |
|---|---|
| `Item` | Abstract base. `item_type` returns the `ItemType`. `Item.from_native(ptr, owns)` dispatches to the right subclass. |
| `TextItem` | UTF-8 text. `type` is a `TextItemType` (`DEFAULT`, `REASONING`, `OPENAI_JSON`). |
| `MessageItem` | A chat turn with a `MessageRole` (`SYSTEM` / `USER` / `ASSISTANT` / `TOOL` / `DEVELOPER`) and either a string or a list of part items (text, image, audio). Convenience factories: `MessageItem.system(content)`, `MessageItem.user(content)`, `MessageItem.assistant(content)`. Borrows native pointers from supplied parts — keep the parts alive for the message's lifetime. |
| `BytesItem` | Raw binary blob (e.g. a base64-decoded payload). |
| `ImageItem` | Image input for vision models. |
| `AudioItem` | Audio input (uri or bytes + `format` +  `sample_rate` + `channels`). |
| `ToolCallItem` | Emitted by the model when invoking a tool. Carries `call_id`, `name`, `arguments` (JSON string). |
| `ToolResultItem` | Caller-supplied tool result. Carries `call_id` and `content`. |
| `TensorItem` | Multi-dimensional tensor. Used for embedding output. Exposes `dimensions`, `data_type` (`TensorDataType`), and `data`. |
| `ItemQueue` | Streaming-input queue used with `AudioSession` for live PCM. The queue is itself an `Item` and is added to a `Request` with `transfer_ownership=False`. |

Enums: `ItemType`, `TextItemType`, `MessageRole`, `TensorDataType`.

### CLI entry point

| Function | CLI name | Description |
|---|---|---|
| `foundry_local_sdk._native.installer.main` | `foundry-local-install` | Install and verify native binaries (`--verbose` to print resolved paths) |

## Running tests

```bash
pip install -r requirements-dev.txt
python -m pytest test/ -v
```

See [test/README.md](test/README.md) for detailed test setup and structure.

## License

MIT — see `LICENSE.txt`.

## Links

- [Foundry Local on GitHub](https://github.com/microsoft/Foundry-Local)
- [Issues](https://github.com/microsoft/Foundry-Local/issues)
