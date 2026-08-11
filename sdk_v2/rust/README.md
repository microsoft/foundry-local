# Foundry Local Rust SDK (v2)

The Foundry Local Rust SDK provides an async Rust interface for running AI models locally on your machine. Discover, download, load, and run inference — all without cloud dependencies.

> **v2 — built on the Foundry Local C++ engine.** This is the v2 binding, layered on the
> `foundry_local` C ABI (the same native engine used by the v2 Python and C# SDKs). Its public
> API is **fully backwards-compatible** with the v1 Rust SDK (`sdk/rust`) — existing code
> compiles and runs unchanged.

## Features

- **Local-first AI** — Run models entirely on your machine with no cloud calls
- **Model catalog** — Browse and discover available models; check what's cached or loaded
- **Automatic model management** — Download, load, unload, and remove models from cache
- **Chat completions** — OpenAI-compatible chat API with both non-streaming and streaming responses
- **Embeddings** — Generate text embeddings via OpenAI-compatible API
- **Audio transcription** — Transcribe audio files locally with streaming support
- **Tool calling** — Function/tool calling with streaming, multi-turn conversation support
- **Response format control** — Text, JSON, JSON Schema, and Lark grammar constrained output
- **Multi-variant models** — Models can have multiple variants (e.g., different quantizations) with automatic selection of the best cached variant
- **Embedded web service** — Start a local HTTP server for OpenAI-compatible API access
- **WinML support** — Automatic execution provider download on Windows for NPU/GPU acceleration
- **Configurable inference** — Control temperature, max tokens, top-k, top-p, frequency penalty, random seed, and more
- **Async-first** — Every operation is `async`; designed for use with the `tokio` runtime
- **Safe FFI** — Dynamically loads the native Foundry Local engine (`foundry_local`) with a safe Rust wrapper

## Prerequisites

- **Rust** 1.70+ (stable toolchain)
- The native `foundry_local` engine (plus its ONNX Runtime / GenAI dependencies) available at
  build or run time — see [Native binary](#native-binary)

## Installation

```sh
cargo add foundry-local-sdk
```

Or add to your `Cargo.toml`:

```toml
[dependencies]
foundry-local-sdk = "2"
```

You also need an async runtime. Most examples use [tokio](https://crates.io/crates/tokio):

```toml
[dependencies]
tokio = { version = "1", features = ["rt-multi-thread", "macros"] }
tokio-stream = "0.1"       # for StreamExt on streaming responses
```

### Feature Flags

| Feature   | Description |
|-----------|-------------|
| `nightly` | Resolve the latest nightly build of the Core package from the ORT-Nightly feed. |

Enable features in `Cargo.toml`:

```toml
[dependencies]
foundry-local-sdk = { version = "2", features = ["nightly"] }
```

> **Windows hardware acceleration (WinML):** No feature flag or separate package is required.
> The single `Microsoft.AI.Foundry.Local.Runtime` package bundles the reg-free WinML 2.x runtime
> on Windows automatically, and the SDK pre-loads it alongside `foundry_local`. WinML execution
> providers (NPU/GPU) are then discovered and downloaded at runtime — see
> [Explicit EP Management](#explicit-ep-management). On macOS and Linux this is a no-op.

### Explicit EP Management

You can explicitly discover and download execution providers:

```rust
use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalManager};

let manager = FoundryLocalManager::create(FoundryLocalConfig::new("my_app"))?;

// Discover available EPs and their status
let eps = manager.discover_eps()?;
for ep in &eps {
    println!("{} — registered: {}", ep.name, ep.is_registered);
}

// Download and register all available EPs
let result = manager.download_and_register_eps(None).await?;
println!("Success: {}, Status: {}", result.success, result.status);

// Download only specific EPs
let result = manager.download_and_register_eps(Some(&[eps[0].name.as_str()])).await?;
```

#### Per-EP download progress

Use `download_and_register_eps_with_progress` to receive typed `(ep_name, percent)` updates
as each EP downloads (`percent` is 0.0–100.0):

```rust
use std::sync::{Arc, Mutex};

let current_ep = Arc::new(Mutex::new(String::new()));
let ep = Arc::clone(&current_ep);
manager.download_and_register_eps_with_progress(None, move |ep_name: &str, percent: f64| {
    let mut current = ep.lock().unwrap();
    if ep_name != current.as_str() {
        if !current.is_empty() {
            println!();
        }
        *current = ep_name.to_string();
    }
    print!("\r  {}  {:5.1}%", ep_name, percent);
}).await?;
println!();
```

#### Cancelling model and EP downloads

Use a shared `Arc<AtomicBool>` with the download builders. Set the flag from another task or signal handler to stop the in-progress download.

```rust
use std::sync::{
    atomic::AtomicBool,
    Arc,
};

// manager and model already initialized
let cancel_flag = Arc::new(AtomicBool::new(false));
// call cancel_flag.store(true, ...) from another task or signal handler to cancel

manager
    .download_and_register_eps_builder()
    .cancel(Arc::clone(&cancel_flag))
    .run()
    .await?;
model
    .download_builder()
    .cancel(Arc::clone(&cancel_flag))
    .run()
    .await?;
```

Catalog access does not block on EP downloads. Call `download_and_register_eps` when you need hardware-accelerated execution providers.

## Quick Start

```rust
use foundry_local_sdk::{
    ChatCompletionRequestMessage, ChatCompletionRequestSystemMessage,
    ChatCompletionRequestUserMessage, FoundryLocalConfig, FoundryLocalManager,
};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // 1. Initialize the manager — loads native libraries and starts the engine
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("my_app"))?;

    // 2. Get a model from the catalog and load it
    let model = manager.catalog().get_model("phi-3.5-mini").await?;
    model.load().await?;

    // 3. Create a chat client and run inference
    let client = model.create_chat_client()
        .temperature(0.7)
        .max_tokens(256);

    let messages: Vec<ChatCompletionRequestMessage> = vec![
        ChatCompletionRequestSystemMessage::from("You are a helpful assistant.").into(),
        ChatCompletionRequestUserMessage::from("What is the capital of France?").into(),
    ];

    let response = client.complete_chat(&messages, None).await?;
    println!("{}", response.choices[0].message.content.as_deref().unwrap_or(""));

    // 4. Clean up
    model.unload().await?;

    Ok(())
}
```

## Usage

### Browsing the Model Catalog

The `Catalog` lets you discover what models are available, which are already cached locally, and which are currently loaded in memory.

```rust
let catalog = manager.catalog();

// List all available models
let models = catalog.get_models().await?;
for model in &models {
    println!("{} (id: {})", model.alias(), model.id());
}

// Look up a specific model by alias
let model = catalog.get_model("phi-3.5-mini").await?;

// Look up a specific variant by its unique model ID
let variant = catalog.get_model_variant("phi-3.5-mini-generic-gpu-4").await?;

// See what's already downloaded
let cached = catalog.get_cached_models().await?;

// See what's currently loaded in memory
let loaded = catalog.get_loaded_models().await?;
```

### Model Lifecycle

Each model may have multiple variants (different quantizations, hardware targets). The SDK auto-selects the best available variant, preferring cached versions. All models are represented by the `Model` type.

```rust
let model = catalog.get_model("phi-3.5-mini").await?;

// Inspect available variants
println!("Selected: {}", model.id());
for v in model.variants() {
    println!("  {} (info.cached: {})", v.id(), v.info()?.cached);
}
```

Download, load, and unload:

```rust
// Download with progress reporting
model.download(Some(|progress: f64| {
    print!("\r{progress:.1}%");
    std::io::Write::flush(&mut std::io::stdout()).ok();
})).await?;

// Or use the builder when combining progress, cancellation, or future options
let cancel_flag = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
model.download_builder()
    .progress(|progress| {
        print!("\r{progress:.1}%");
        std::io::Write::flush(&mut std::io::stdout()).ok();
    })
    .cancel(cancel_flag.clone())
    .run()
    .await?;

// Load into memory
model.load().await?;

// Unload when done
model.unload().await?;

// Remove from local cache entirely
model.remove_from_cache().await?;
```

### Chat Completions

The `ChatClient` follows the OpenAI Chat Completion API structure.

```rust
let client = model.create_chat_client()

// Configure generation settings (fluent builder)
    .temperature(0.7)
    .max_tokens(256)
    .top_p(0.9)
    .frequency_penalty(0.5);

// Non-streaming completion
let response = client.complete_chat(
    &[
        ChatCompletionRequestSystemMessage::from("You are a helpful assistant.").into(),
        ChatCompletionRequestUserMessage::from("Explain Rust's ownership model.").into(),
    ],
    None,
).await?;

println!("{}", response.choices[0].message.content.as_deref().unwrap_or(""));
```

### Streaming Responses

For real-time token-by-token output, use streaming:

```rust
use tokio_stream::StreamExt;

let mut stream = client.complete_streaming_chat(
    &[ChatCompletionRequestUserMessage::from("Write a short poem about Rust.").into()],
    None,
).await?;

while let Some(chunk) = stream.next().await {
    let chunk = chunk?;
    if let Some(content) = &chunk.choices[0].delta.content {
        print!("{content}");
    }
}

// Errors from the native core are delivered as stream items —
// no separate close() call needed.
```

### Tool Calling

Define functions the model can call and handle the multi-turn conversation:

```rust
use foundry_local_sdk::{
    ChatCompletionRequestMessage, ChatCompletionRequestToolMessage,
    ChatCompletionTools, ChatFinishReason, ChatToolChoice,
};
use serde_json::json;

// Define available tools
let tools: Vec<ChatCompletionTools> = serde_json::from_value(json!([{
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": "Get the current weather for a location",
        "parameters": {
            "type": "object",
            "properties": {
                "location": { "type": "string", "description": "City name" }
            },
            "required": ["location"]
        }
    }
}]))?;

let client = model.create_chat_client()
    .max_tokens(512)
    .tool_choice(ChatToolChoice::Auto);

let mut messages: Vec<ChatCompletionRequestMessage> = vec![
    ChatCompletionRequestUserMessage::from("What's the weather in Seattle?").into(),
];

// First request — model may call a tool
let response = client.complete_chat(&messages, Some(&tools)).await?;
let choice = &response.choices[0];

if choice.finish_reason == Some(ChatFinishReason::ToolCalls) {
    if let Some(tool_calls) = &choice.message.tool_calls {
        for tc in tool_calls {
            // Execute the tool (your application logic)
            let result = execute_tool(&tc.function.name, &tc.function.arguments);

            // Add assistant message with tool calls, then the tool result
            messages.push(serde_json::from_value(json!({
                "role": "assistant",
                "content": null,
                "tool_calls": [{ "id": tc.id, "type": "function",
                    "function": { "name": tc.function.name,
                                  "arguments": tc.function.arguments } }]
            }))?);
            messages.push(ChatCompletionRequestToolMessage {
                content: result.into(),
                tool_call_id: tc.id.clone(),
            }.into());
        }

        // Continue the conversation with tool results
        let final_response = client.complete_chat(&messages, Some(&tools)).await?;
        println!("{}", final_response.choices[0].message.content.as_deref().unwrap_or(""));
    }
}
```

Tool calling also works with streaming via `complete_streaming_chat` — accumulate tool call fragments during streaming and check for `ChatFinishReason::ToolCalls`.

### Response Format Options

Control the output format of chat completions:

```rust
use foundry_local_sdk::ChatResponseFormat;

// Plain text (default)
let client = model.create_chat_client()
    .response_format(ChatResponseFormat::Text);

// Unstructured JSON output
let client = model.create_chat_client()
    .response_format(ChatResponseFormat::JsonObject);

// JSON constrained to a schema
let client = model.create_chat_client()
    .response_format(ChatResponseFormat::JsonSchema(r#"{
        "type": "object",
        "properties": {
            "name": { "type": "string" },
            "age": { "type": "integer" }
        },
        "required": ["name", "age"]
    }"#.to_string()));

// Output constrained by a Lark grammar (Foundry extension)
let client = model.create_chat_client()
    .response_format(ChatResponseFormat::LarkGrammar(grammar.to_string()));
```

### Embeddings

Generate text embeddings using the `EmbeddingClient`:

```rust
let embedding_client = model.create_embedding_client();

// Single input
let response = embedding_client
    .generate_embedding("The quick brown fox jumps over the lazy dog")
    .await?;
let embedding = &response.data[0].embedding; // Vec<f32>
println!("Dimensions: {}", embedding.len());

// Batch input
let batch_response = embedding_client
    .generate_embeddings(&["The quick brown fox", "The capital of France is Paris"])
    .await?;
// batch_response.data[0].embedding, batch_response.data[1].embedding
```

### Audio Transcription

Transcribe audio files locally using the `AudioClient`:

```rust
let model = manager.catalog().get_model("whisper-tiny").await?;
model.load().await?;

let audio_client = model.create_audio_client()
    .language("en");

// Non-streaming transcription
let result = audio_client.transcribe("recording.wav").await?;
println!("{}", result.text);
```

#### Streaming Transcription

```rust
use tokio_stream::StreamExt;

let mut stream = audio_client.transcribe_streaming("recording.wav").await?;
while let Some(chunk) = stream.next().await {
    print!("{}", chunk?.text);
}
```

### Embedded Web Service

Start a local HTTP server that exposes an OpenAI-compatible REST API:

```rust
manager.start_web_service().await?;
let urls = manager.urls()?;
println!("Service running at: {:?}", urls);

// Any OpenAI-compatible client or tool can now connect to the endpoint.
// ...

manager.stop_web_service().await?;
```

### Chat Client Settings

All settings are configured via chainable builder methods on `ChatClient`:

| Method | Type | Description |
|--------|------|-------------|
| `temperature(v)` | `f64` | Sampling temperature (0.0–2.0; higher = more random) |
| `max_tokens(v)` | `u32` | Maximum number of tokens to generate |
| `top_p(v)` | `f64` | Nucleus sampling probability (0.0–1.0) |
| `top_k(v)` | `u32` | Top-k sampling parameter (Foundry extension) |
| `frequency_penalty(v)` | `f64` | Frequency penalty |
| `presence_penalty(v)` | `f64` | Presence penalty |
| `n(v)` | `u32` | Number of completions to generate |
| `random_seed(v)` | `u64` | Random seed for reproducible results (Foundry extension) |
| `response_format(v)` | `ChatResponseFormat` | Output format (Text, JsonObject, JsonSchema, LarkGrammar) |
| `tool_choice(v)` | `ChatToolChoice` | Tool selection strategy (None, Auto, Required, Function) |

## Error Handling

All fallible operations return `foundry_local_sdk::Result<T>`, which is an alias for `std::result::Result<T, FoundryLocalError>`.

```rust
use foundry_local_sdk::FoundryLocalError;

match manager.catalog().get_model("nonexistent").await {
    Ok(model) => { /* use model */ }
    Err(FoundryLocalError::ModelOperation { reason }) => {
        eprintln!("Model error: {reason}");
    }
    Err(FoundryLocalError::CommandExecution { reason }) => {
        eprintln!("Core engine error: {reason}");
    }
    Err(e) => {
        eprintln!("Unexpected error: {e}");
    }
}
```

### Error Variants

| Variant | Description |
|---------|-------------|
| `LibraryLoad { reason }` | The native core library could not be loaded |
| `CommandExecution { reason }` | A command executed against native core returned an error |
| `InvalidConfiguration { reason }` | The provided configuration is invalid |
| `ModelOperation { reason }` | A model operation failed (load, unload, download, etc.) |
| `HttpRequest(reqwest::Error)` | An HTTP request to an external service failed |
| `Serialization(serde_json::Error)` | JSON serialization/deserialization failed |
| `Validation { reason }` | A validation check on user-supplied input failed |
| `Io(std::io::Error)` | An I/O error occurred |
| `Internal { reason }` | An internal SDK error (e.g. poisoned lock) |

## Configuration

The SDK is configured via `FoundryLocalConfig` when creating the manager:

```rust
use foundry_local_sdk::{FoundryLocalConfig, LogLevel};

let config = FoundryLocalConfig::new("my_app")
    .log_level(LogLevel::Info)
    .model_cache_dir("/path/to/cache")
    .web_service_urls("http://127.0.0.1:5000");

let manager = FoundryLocalManager::create(config)?;
```

| Setting | Builder method | Default | Description |
|---------|---------------|---------|-------------|
| App name | `new(name)` | **(required)** | Your application name |
| App data dir | `.app_data_dir(dir)` | `~/.{app_name}` | Application data directory |
| Model cache dir | `.model_cache_dir(dir)` | `{app_data_dir}/cache/models` | Where models are stored locally |
| Logs dir | `.logs_dir(dir)` | `{app_data_dir}/logs` | Log output directory |
| Log level | `.log_level(level)` | `Warn` | `Trace`, `Debug`, `Info`, `Warn`, `Error`, `Fatal` |
| Web service URLs | `.web_service_urls(urls)` | `None` | Bind address for the embedded web service |
| Service endpoint | `.service_endpoint(url)` | `None` | URL of an existing external service to connect to |
| Library path | `.library_path(path)` | Auto-discovered | Path to the native `foundry_local` library (or its directory) |
| Additional settings | `.additional_setting(k, v)` | `None` | Extra key-value settings passed to Core |
| Logger | `.logger(impl Logger)` | `None` | Application logger (stub — not yet wired) |

## How It Works

### Native binary

The SDK loads the `foundry_local` native library (and its ONNX Runtime / GenAI dependencies) at
runtime. The `build.rs` build script can obtain it in two ways, controlled by environment variables:

| Variable | Purpose |
|----------|---------|
| `FOUNDRY_LOCAL_NATIVE_BIN_DIR` | Copy native binaries from a local C++ build output directory (the dev path). Mirrors the C# `FoundryLocalNativeBinDir`. |
| `FOUNDRY_LOCAL_RUNTIME_VERSION` | Download the Runtime NuGet package (`Microsoft.AI.Foundry.Local.Runtime`) plus ONNX Runtime / GenAI for the target RID. On Windows this package bundles the reg-free WinML 2.x runtime. |

If neither is set at build time, the library is resolved at **runtime** from (in order):

1. `FoundryLocalConfig::library_path` — a path to the `foundry_local` library file or its directory.
2. The `FOUNDRY_LOCAL_LIB_DIR` environment variable.
3. The directory of the running executable.
4. The system loader search path.

> **Migration note:** in v1, `FoundryLocalConfig::library_path` pointed at the
> `Microsoft.AI.Foundry.Local.Core` library. In v2 it points at the `foundry_local` library (or its
> containing directory). The builder method and signature are unchanged.

ONNX Runtime and GenAI are pre-loaded before `foundry_local` so the dynamic loader resolves the
engine's dependencies regardless of rpath/search-path setup. On platforms where GenAI honours it,
`ORT_LIB_PATH` is set to the pre-loaded ONNX Runtime.

### Runtime Loading

At runtime, the SDK uses `libloading` to dynamically load the `foundry_local` library, resolve the
API function table via `FoundryLocalGetApi`, and cache the sub-API tables. No static linking or
system-wide installation is required.

## Platform Support

| Platform        | RID          | Status |
|-----------------|--------------|--------|
| Windows x64     | `win-x64`    | ✅     |
| Windows ARM64   | `win-arm64`  | ✅     |
| Linux x64       | `linux-x64`  | ✅     |
| Linux ARM64     | `linux-arm64`| ✅     |
| macOS ARM64     | `osx-arm64`  | ✅     |

## Running Examples

Sample applications are available in [`samples/rust/`](../../samples/rust/):

| Sample | Description |
|--------|-------------|
| `native-chat-completions` | Non-streaming and streaming chat completions |
| `tool-calling-foundry-local` | Function/tool calling with multi-turn conversations |
| `audio-transcription-example` | Audio transcription (non-streaming and streaming) |
| `foundry-local-webserver` | Embedded OpenAI-compatible REST API server |

Run a sample with:

```sh
cd samples/rust
cargo run -p native-chat-completions
```

## License

Microsoft Software License Terms — see [LICENSE](../../LICENSE) for details.
