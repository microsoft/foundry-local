# Foundry Local C# samples

These seven samples use Foundry Local SDK **2.2.1** and .NET 9:

| Sample | What it demonstrates |
| --- | --- |
| [`hello-world`](hello-world) | Execution-provider discovery, model lifecycle, and native `ChatSession` inference |
| [`realtime-transcription`](realtime-transcription) | Live microphone PCM input with native `AudioSession` and `ItemQueue` streaming |
| [`embeddings`](embeddings) | Native `EmbeddingsSession` inference |
| [`tool-calling`](tool-calling) | Native tool definitions, `ToolCallItem`, and `ToolResultItem` |
| [`web-server`](web-server) | The built-in server and its discovered OpenAI-compatible endpoint |
| [`vision`](vision) | Native chat with text and a local image |
| [`byom`](byom) | Registering existing model assets in the local catalog and running native chat |

> [!IMPORTANT]
> These samples restore the published
> [Microsoft.AI.Foundry.Local 2.2.1 NuGet package](https://www.nuget.org/packages/Microsoft.AI.Foundry.Local/2.2.1),
> including its native runtime dependencies. No local SDK build is required. See the
> [get-started documentation](https://learn.microsoft.com/en-us/azure/foundry-local/get-started) for versioned setup
> guidance and additional examples.

## Prerequisites

- .NET 9 SDK
- A supported Foundry Local platform
- Internet access for the catalog, execution-provider, and model downloads
- A microphone on Windows for `realtime-transcription`

All samples set `Configuration.AppName` to `foundry_local_samples`, so they share
the same application data and model cache.

## Build

From this directory, build any sample. The SDK version is pinned centrally in `Directory.Packages.props`:

```shell
dotnet build hello-world/HelloWorld.csproj
dotnet build realtime-transcription/RealtimeTranscription.csproj
dotnet build embeddings/Embeddings.csproj
dotnet build tool-calling/ToolCalling.csproj
dotnet build web-server/WebServer.csproj
dotnet build vision/Vision.csproj
dotnet build byom/Byom.csproj
```

`nuget.config` restores the SDK and other dependencies from NuGet.org.

## Run

Run any project with `dotnet run --project <directory>`. For example:

```shell
dotnet run --project hello-world
dotnet run --project realtime-transcription
dotnet run --project vision -- VISION_MODEL_ALIAS /path/to/image.png
dotnet run --project byom -- /path/to/model-directory
```

For `vision`, use an alias whose catalog task is `vision-language-chat` and your own JPEG or PNG image.
For `byom`, use an already exported, compatible ONNX Runtime GenAI chat model directory containing
`genai_config.json`, weights, and tokenizer files. The sample unregisters the model without deleting its assets.

The realtime sample records 16 kHz mono PCM until you press Enter. The web-server
sample binds to an ephemeral loopback port, reads the actual URL discovered by
the SDK, and sends a chat request to its `/v1` endpoint.
