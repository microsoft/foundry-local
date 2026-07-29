# 🚀 Foundry Local C# Samples

These samples demonstrate how to use the Foundry Local C# SDK. Each sample uses the `Microsoft.AI.Foundry.Local` NuGet package, which bundles the WinML runtime on Windows automatically.

## Samples

| Sample | Description |
|---|---|
| [native-chat-completions](native-chat-completions/) | Initialize the SDK, download a model, and run chat completions. |
| [embeddings](embeddings/) | Generate single and batch text embeddings using the Foundry Local SDK. |
| [audio-transcription-example](audio-transcription-example/) | Transcribe audio files using the Foundry Local SDK. |
| [foundry-local-web-server](foundry-local-web-server/) | Set up a local OpenAI-compliant web server. |
| [foundry-local-web-server-responses-vision](foundry-local-web-server-responses-vision/) | Stream a vision (image understanding) response from the local web server using the Responses API. |
| [tool-calling-foundry-local-sdk](tool-calling-foundry-local-sdk/) | Use tool calling with native chat completions. |
| [tool-calling-foundry-local-web-server](tool-calling-foundry-local-web-server/) | Use tool calling with the local web server. |
| [model-management-example](model-management-example/) | Manage models, variant selection, and updates. |
| [tutorial-chat-assistant](tutorial-chat-assistant/) | Build an interactive chat assistant (tutorial). |
| [tutorial-document-summarizer](tutorial-document-summarizer/) | Summarize documents with AI (tutorial). |
| [tutorial-tool-calling](tutorial-tool-calling/) | Create a tool-calling assistant (tutorial). |
| [tutorial-voice-to-text](tutorial-voice-to-text/) | Transcribe and summarize audio (tutorial). |


## Running a sample

1. Clone the repository:
   ```bash
   git clone https://github.com/microsoft/Foundry-Local.git
   cd Foundry-Local/samples/cs
   ```

2. Open and run a sample:
   ```bash
   cd native-chat-completions
   dotnet run
   ```

