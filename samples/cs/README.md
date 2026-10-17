# 🚀 Foundry Local C# Samples

These samples use SDK v2 from `sdk_v2/cs` through the shared project reference in
`Directory.Build.props`. Native inference uses `ChatSession`, `AudioSession`, and
`EmbeddingsSession` with typed requests and items, rather than the deprecated
Betalgo-based clients. The HTTP samples retain the official OpenAI client or use
`HttpClient` to demonstrate the local web service.

The samples have no direct Betalgo dependency. The SDK currently still includes
Betalgo transitively for its deprecated compatibility clients.

## Samples

| Sample | Description |
|---|---|
| [native-chat-completions](native-chat-completions/) | Initialize the SDK, download a model, and run chat completions. |
| [embeddings](embeddings/) | Generate single and batch text embeddings using the Foundry Local SDK. |
| [audio-transcription-example](audio-transcription-example/) | Transcribe audio files using the Foundry Local SDK. |
| [live-audio-transcription](live-audio-transcription/) | Stream microphone PCM audio through an SDK v2 input queue. |
| [foundry-local-web-server](foundry-local-web-server/) | Set up a local OpenAI-compliant web server. |
| [foundry-local-web-server-responses-vision](foundry-local-web-server-responses-vision/) | Stream a vision (image understanding) response from the local web server using the Responses API. |
| [tool-calling-foundry-local-sdk](tool-calling-foundry-local-sdk/) | Use tool calling with native chat completions. |
| [tool-calling-foundry-local-web-server](tool-calling-foundry-local-web-server/) | Use tool calling with the local web server. |
| [model-management-example](model-management-example/) | Manage models, variant selection, and updates. |
| [tutorial-chat-assistant](tutorial-chat-assistant/) | Build an interactive chat assistant (tutorial). |
| [tutorial-document-summarizer](tutorial-document-summarizer/) | Summarize documents with AI (tutorial). |
| [tutorial-tool-calling](tutorial-tool-calling/) | Create a tool-calling assistant (tutorial). |
| [tutorial-voice-to-text](tutorial-voice-to-text/) | Transcribe and summarize audio (tutorial). |
| [verify-winml](verify-winml/) | Verify execution-provider registration and accelerated native chat. |
| [byom-cache-rescan](byom-cache-rescan/) | Register caller-owned model assets in the local catalog and run native chat. |


## Running a sample

1. Clone the repository:
   ```bash
   git clone https://github.com/microsoft/Foundry-Local.git
   cd Foundry-Local/samples/cs
   ```

2. Build the SDK v2 native runtime from the repository root:
   ```powershell
   python sdk_v2\cpp\build.py --configure --build --config RelWithDebInfo
   ```
   See the [C++ SDK build instructions](../../sdk_v2/cpp/README.md) for prerequisites.
   The C# SDK automatically finds the native build. Alternatively, supply
   `-p:FoundryLocalRuntimeVersion=<matching-runtime-package-version>` to use the
   `Microsoft.AI.Foundry.Local.Runtime` package, or
   `-p:FoundryLocalNativeBinDir=<native-binary-directory>` for an existing build.

3. Open and run a sample:
   ```bash
   cd native-chat-completions
   dotnet run
   ```

Sessions retain conversation history. Submit only new messages or tool results on
subsequent requests, dispose each streamed item and response, and dispose sessions
before unloading their models.
