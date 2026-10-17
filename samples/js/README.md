# 🚀 Foundry Local JavaScript Samples

These samples demonstrate how to use the Foundry Local JavaScript SDK (`foundry-local-sdk`) with Node.js.

## Prerequisites

- [Node.js](https://nodejs.org/) 20 or later
- A built [SDK v2 native runtime](../../sdk_v2/cpp/README.md). The JavaScript build uses the
  C++ `RelWithDebInfo` build output to link its addon and stage native dependencies.

## Samples

| Sample | Description |
|--------|-------------|
| [native-chat-completions](native-chat-completions/) | Initialize the SDK, download a model, and run non-streaming and streaming chat completions. |
| [embeddings](embeddings/) | Generate single and batch text embeddings using the Foundry Local SDK. |
| [audio-transcription-example](audio-transcription-example/) | Transcribe audio files using the Whisper model with streaming output. |
| [chat-and-audio-foundry-local](chat-and-audio-foundry-local/) | Unified sample demonstrating both chat and audio transcription in one application. |
| [electron-chat-application](electron-chat-application/) | Full-featured Electron desktop chat app with voice transcription and model management. |
| [copilot-sdk-foundry-local](copilot-sdk-foundry-local/) | GitHub Copilot SDK integration with Foundry Local for agentic AI workflows. |
| [langchain-integration-example](langchain-integration-example/) | LangChain.js integration for building text generation chains. |
| [tool-calling-foundry-local](tool-calling-foundry-local/) | Tool calling with custom function definitions and streaming responses. |
| [web-server-example](web-server-example/) | Start a local OpenAI-compatible web server and call it with the OpenAI SDK. |
| [web-server-responses-vision-example](web-server-responses-vision-example/) | Stream a vision (image understanding) response from the local web server using the Responses API. |
| [tutorial-chat-assistant](tutorial-chat-assistant/) | Build an interactive multi-turn chat assistant (tutorial). |
| [tutorial-document-summarizer](tutorial-document-summarizer/) | Summarize documents with AI (tutorial). |
| [tutorial-tool-calling](tutorial-tool-calling/) | Create a tool-calling assistant (tutorial). |
| [tutorial-voice-to-text](tutorial-voice-to-text/) | Transcribe and summarize audio (tutorial). |
| [live-audio-transcription](live-audio-transcription/) | Stream microphone PCM through an `AudioSession`. |
| [verify-winml](verify-winml/) | Verify execution providers and accelerated native chat. |
| [byom-cache-rescan](byom-cache-rescan/) | Register a CPU model clone explicitly in the local catalog. |

## Running a Sample

1. Clone the repository:

   ```bash
   git clone https://github.com/microsoft/Foundry-Local.git
   cd Foundry-Local/samples/js
   ```

1. Build the SDK v2 source package, then install a sample's dependencies:

   ```bash
   cd ../../sdk_v2/js
   npm install
   npm run build
   cd ../../samples/js/native-chat-completions
   npm install
   ```

1. Run the sample:

   ```bash
   npm start
   ```

> [!TIP]
> Native JS samples in this repo pin `foundry-local-sdk` to the local `sdk_v2/js`
> source package via a `file:` dependency, so `npm install` uses the in-repo v2
> SDK instead of the published legacy package. On **Windows**, the v2 package
> bundles WinML hardware acceleration automatically; no separate `-winml`
> package is needed.
