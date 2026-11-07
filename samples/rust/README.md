# Foundry Local Rust samples

These seven samples use the published Foundry Local Rust SDK **2.2.1**.
All crates pin `foundry-local-sdk = "=2.2.1"` and set the application name to
`foundry_local_samples`.

> [!IMPORTANT]
> These samples use [`foundry-local-sdk` from crates.io](https://crates.io/crates/foundry-local-sdk/2.2.1).
> `.cargo/config.toml` also selects native Runtime 2.2.1, which the SDK acquires during the build.
> No local SDK source build is required. See the
> [get-started documentation](https://learn.microsoft.com/en-us/azure/foundry-local/get-started) for versioned setup
> guidance and additional examples.

## Prerequisites

- A supported Foundry Local platform
- The Rust toolchain

## Samples

| Directory | Demonstrates |
| --- | --- |
| [`hello-world`](hello-world/) | Manager setup, execution-provider registration, catalog/model lifecycle, and native chat inference |
| [`realtime-transcription`](realtime-transcription/) | Native microphone streaming with `AudioSession` and `ItemQueue` |
| [`embeddings`](embeddings/) | Batched embeddings and similarity comparison with the native `EmbeddingsSession` |
| [`tool-calling`](tool-calling/) | Native `ChatSession` tool definitions, tool-call items, and tool-result items |
| [`web-server`](web-server/) | Starting the built-in service, discovering its URL, calling an OpenAI-compatible endpoint, and stopping it |
| [`vision`](vision/) | Sending text and a local image through a native `ChatSession` |
| [`byom`](byom/) | Registering existing local model assets and running native chat |

## Running a Sample

Run a package from this directory:

```bash
cd samples/rust
cargo run -p hello-world
```

Replace `hello-world` with any package name from the table. Real-time
transcription uses the default microphone and runs until Ctrl+C:

```bash
cargo run -p realtime-transcription
```

Vision accepts a catalog alias with task `vision-language-chat` and a local JPEG or PNG image. BYOM accepts an
already exported, compatible ONNX Runtime GenAI chat model directory containing `genai_config.json`, weights, and
tokenizer files:

```bash
cargo run -p vision -- VISION_MODEL_ALIAS /path/to/image.png
cargo run -p byom -- /path/to/model-directory
```

The BYOM sample unregisters its model on normal completion without deleting your files.

The first run may download execution providers and model assets. Later runs
reuse cached models.
