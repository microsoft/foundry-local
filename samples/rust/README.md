# 🚀 Foundry Local Rust Samples

These samples demonstrate how to use the Rust binding for Foundry Local.

## Prerequisites

- [Rust](https://www.rust-lang.org/) 1.70.0 or later

## Samples

| Sample | Description |
|--------|-------------|
| [native-chat-completions](native-chat-completions/) | Non-streaming and streaming chat completions using the native chat client. |
| [embeddings](embeddings/) | Generate single and batch text embeddings using the native embedding client. |
| [audio-transcription-example](audio-transcription-example/) | Audio transcription (non-streaming and streaming) using the Whisper model. |
| [foundry-local-webserver](foundry-local-webserver/) | Start a local OpenAI-compatible web server and call it with a standard HTTP client. |
| [foundry-local-webserver-responses-vision](foundry-local-webserver-responses-vision/) | Stream a vision (image understanding) response from the local web server using the Responses API. |
| [tool-calling-foundry-local](tool-calling-foundry-local/) | Tool calling with streaming responses, multi-turn conversation, and local tool execution. |
| [tutorial-chat-assistant](tutorial-chat-assistant/) | Build an interactive multi-turn chat assistant (tutorial). |
| [tutorial-document-summarizer](tutorial-document-summarizer/) | Summarize documents with AI (tutorial). |
| [tutorial-tool-calling](tutorial-tool-calling/) | Create a tool-calling assistant (tutorial). |
| [tutorial-voice-to-text](tutorial-voice-to-text/) | Transcribe and summarize audio (tutorial). |
| [live-audio-transcription-example](live-audio-transcription-example/) | Real-time microphone transcription using the `nemotron` model. *(Requires SDK live-transcription API — not yet available.)* |

## Running a Sample

1. Clone the repository:

   ```bash
   git clone https://github.com/microsoft/Foundry-Local.git
   cd Foundry-Local/samples/rust
   ```

2. Run a sample:

   ```bash
   cargo run -p native-chat-completions
   ```

   Or navigate to a sample directory and run directly:

   ```bash
   cd native-chat-completions
   cargo run
   ```

> [!TIP]
> Each sample's `Cargo.toml` uses `[target.'cfg(windows)'.dependencies]` to automatically enable the `winml` feature on Windows for broader hardware acceleration. On macOS and Linux, the standard SDK is used. No manual configuration needed.
