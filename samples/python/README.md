# 🚀 Foundry Local Python Samples

These samples demonstrate how to use Foundry Local with Python.

## Prerequisites

- [Python](https://www.python.org/) 3.11 or later
- A built [SDK v2 native runtime](../../sdk_v2/cpp/README.md). Set `FOUNDRY_LOCAL_LIB_DIR`
  to its binary directory when using an unbundled source build.

## Samples

| Sample | Description |
|--------|-------------|
| [native-chat-completions](native-chat-completions/) | Initialize the SDK and run native streaming chat with `ChatSession`. |
| [embeddings](embeddings/) | Generate single and batch text embeddings using the Foundry Local SDK. |
| [audio-transcription](audio-transcription/) | Transcribe audio files using the Whisper model. |
| [web-server](web-server/) | Start a local OpenAI-compatible web server and call it with the OpenAI Python SDK. |
| [web-server-responses](web-server-responses/) | Call a running local OpenAI-compatible web server with the Responses API, including streaming and tool calling. |
| [tool-calling](tool-calling/) | Tool calling with custom function definitions (get_weather, calculate). |
| [langchain-integration](langchain-integration/) | LangChain integration for building translation and text generation chains. |
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
   cd Foundry-Local/samples/python
   ```

2. Build/install the repo-local `sdk_v2/python` package first, then install the sample dependencies:

   ```bash
   cd ../../sdk_v2/python
   python -m pip install build
   python -m build --wheel --outdir dist/
   python -m pip install --find-links dist foundry-local-sdk==2.0.0.dev0

   cd ../../samples/python/native-chat-completions
   pip install -r requirements.txt
   ```

   This avoids silently pulling the published legacy `foundry-local-sdk` 1.2.x
   package from PyPI when working from this repo checkout.

3. Run the sample:

   ```bash
   python src/app.py
   ```

> [!TIP]
> The sample `requirements.txt` files require SDK v2 or later. Install the source-built
> package first while v2 is not yet published; legacy SDK packages cannot satisfy these requirements.
