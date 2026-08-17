# Generating API Reference Docs

The Rust SDK uses `cargo doc` to generate API documentation from `///` doc comments in the source code.

## Viewing Docs Locally

To generate and open the API docs in your browser:

```bash
cd sdk_v2/rust
cargo doc --no-deps --open
```

This generates HTML documentation at `target/doc/foundry_local_sdk/index.html`.

## Public API Surface

The SDK re-exports all public types from the crate root. Key modules:

| Module / Type | Description |
|---|---|
| `FoundryLocalManager` | Entry point — SDK initialisation, web service lifecycle |
| `FoundryLocalConfig` | Configuration (app name, log level, service endpoint) |
| `Catalog` | Model discovery and lookup |
| `Model` | Grouped model (alias → best variant) |
| `DownloadBuilder` | Builder for model downloads (progress, cancellation) |
| `ChatClient` | OpenAI-compatible chat completions (sync + streaming) |
| `AudioClient` | OpenAI-compatible audio transcription (sync + streaming) |
| `CreateChatCompletionResponse` | Typed chat completion response (from `async-openai`) |
| `CreateChatCompletionStreamResponse` | Typed streaming chat chunk (from `async-openai`) |
| `AudioTranscriptionResponse` | Typed audio transcription response |
| `FoundryLocalError` | Error enum with variants for all failure modes |

## Notes

- Unlike the C# and JS SDKs which commit generated markdown docs, Rust's convention is to generate HTML docs on demand with `cargo doc`.
- Once the crate is published to crates.io, docs will be automatically hosted at [docs.rs](https://docs.rs).
- Use `--document-private-items` to include internal/private API in the generated docs:
  ```bash
  cargo doc --no-deps --document-private-items --open
  ```
