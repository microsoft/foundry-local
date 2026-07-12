# 🚀 Foundry Local JavaScript Samples

These samples demonstrate how to use the Foundry Local JavaScript SDK (`foundry-local-sdk`) with Node.js.

## Prerequisites

- [Node.js](https://nodejs.org/) (v18 or later recommended)

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

## Running a Sample

1. Clone the repository:

   ```bash
   git clone https://github.com/microsoft/Foundry-Local.git
   cd Foundry-Local/samples/js
   ```

1. Navigate to a sample and install dependencies:

   ```bash
   cd native-chat-completions
   npm install
   ```

1. Run the sample:

   ```bash
   npm start
   ```

> [!TIP]
> Each sample's `package.json` includes the single `foundry-local-sdk` package. On **Windows**, it bundles WinML hardware acceleration automatically. Just run `npm install` — no separate package or flag is needed.
