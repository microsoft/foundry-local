# 🚀 Foundry Local Rust Samples

These samples demonstrate how to use the Rust binding for Foundry Local.

## Prerequisites

- [Rust](https://www.rust-lang.org/) 1.85 or later
- A built native SDK v2 runtime; set `FOUNDRY_LOCAL_LIB_DIR` to its binary directory.

## Samples

| Sample | Description |
|--------|-------------|
| [native-chat-completions](native-chat-completions/) | Non-streaming and streaming chat completions using `ChatSession`. |
| [embeddings](embeddings/) | Generate single and batch text embeddings using `EmbeddingsSession`. |
| [audio-transcription-example](audio-transcription-example/) | Audio transcription (non-streaming and streaming) using the Whisper model. |
| [foundry-local-webserver](foundry-local-webserver/) | Start a local OpenAI-compatible web server and call it with a standard HTTP client. |
| [foundry-local-webserver-responses-vision](foundry-local-webserver-responses-vision/) | Stream a vision (image understanding) response from the local web server using the Responses API. |
| [tool-calling-foundry-local](tool-calling-foundry-local/) | Tool calling with streaming responses, multi-turn conversation, and local tool execution. |
| [tutorial-chat-assistant](tutorial-chat-assistant/) | Build an interactive multi-turn chat assistant (tutorial). |
| [tutorial-document-summarizer](tutorial-document-summarizer/) | Summarize documents with AI (tutorial). |
| [tutorial-tool-calling](tutorial-tool-calling/) | Create a tool-calling assistant (tutorial). |
| [tutorial-voice-to-text](tutorial-voice-to-text/) | Transcribe and summarize audio (tutorial). |
| [live-audio-transcription](live-audio-transcription/) | Real-time microphone transcription using `AudioSession` and streamed PCM bytes. |
| [verify-winml](verify-winml/) | Verify execution providers and accelerated native chat. |
| [byom-cache-rescan](byom-cache-rescan/) | Register a CPU model clone explicitly in the local catalog. |

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
> Each sample references `sdk_v2/rust` directly. The native SDK handles platform-specific execution providers;
> no separate Rust `winml` feature is required.
