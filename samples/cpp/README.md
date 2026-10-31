# Foundry Local C++ samples

These seven samples use the Foundry Local SDK **2.2.1** public C++ API:

| Sample | Demonstrates |
|---|---|
| `hello-world` | Complete manager, execution-provider, catalog, model, session, and cleanup lifecycle |
| `realtime-transcription` | Native streaming audio transcription from a microphone or synthetic PCM |
| `embeddings` | Native `EmbeddingsSession` inference with `qwen3-embedding-0.6b` |
| `tool-calling` | Native `ChatSession`, tool-call items, and tool-result items |
| `web-server` | Built-in server lifecycle and an OpenAI-compatible HTTP request |
| `vision` | Native multimodal chat with text and a local image |
| `byom` | Explicit local-catalog registration and inference with existing model assets |

The non-web samples run inference directly in process. The web-server sample is intentionally the only sample that
routes inference through HTTP.

> [!IMPORTANT]
> These samples use the published
> [Runtime 2.2.1 NuGet package](https://www.nuget.org/packages/Microsoft.AI.Foundry.Local.Runtime/2.2.1)
> for both headers and native libraries. No local SDK source build is required. See the
> [get-started documentation](https://learn.microsoft.com/en-us/azure/foundry-local/get-started) for versioned setup
> guidance and additional examples.

## Prerequisites

- CMake 3.20 or newer
- A C++20 compiler
- The [NuGet CLI](https://learn.microsoft.com/en-us/nuget/reference/nuget-exe-cli-reference)
- The platform prerequisites listed in [`sdk_v2/cpp/README.md`](../../sdk_v2/cpp/README.md)
- Optional: PortAudio development files for live microphone capture

## Build

From `samples/cpp`, restore Runtime 2.2.1 and its matching native dependencies:

```sh
nuget install Microsoft.AI.Foundry.Local.Runtime -Version 2.2.1 -OutputDirectory build/packages/2.2.1 -Source https://api.nuget.org/v3/index.json
```

Configure and build the samples:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo --parallel
```

The sample project uses the restored packages in `build/packages/2.2.1` and copies the native libraries beside each
executable. For a different package directory, pass its location:

```sh
cmake -S . -B build -DFOUNDRY_LOCAL_PACKAGES_DIR=/path/to/packages/2.2.1
```

For cross-compilation, set `FOUNDRY_LOCAL_RID` to the target runtime identifier, such as `win-arm64`.
On Linux or macOS, keep the copied shared libraries in the loader search path if required by your environment.

## Run

Executable locations depend on the CMake generator. With a single-config generator:

```sh
./build/hello-world
./build/realtime-transcription
./build/realtime-transcription --synthetic
./build/embeddings
./build/tool-calling
./build/web-server
./build/vision VISION_MODEL_ALIAS /path/to/image.png
./build/byom /path/to/model-directory
```

For `vision`, choose a catalog alias with task `vision-language-chat`. For `byom`, supply a compatible, already
exported ONNX Runtime GenAI chat model directory containing `genai_config.json`, weights, and tokenizer files.
BYOM registration is removed on normal completion; the model files are never deleted.

When PortAudio is available at configure time, `realtime-transcription` records five seconds from the default
microphone. Pass `--synthetic` to force the fallback. Without PortAudio, the sample automatically streams a generated
16 kHz mono PCM signal; because it contains no speech, the resulting transcript may be empty.

The first run may download execution providers and model assets. Later runs reuse the application cache associated
with the exact app name `foundry_local_samples`.
