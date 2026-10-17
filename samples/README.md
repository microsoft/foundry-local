# Foundry Local Samples

Explore complete working examples that demonstrate how to use Foundry Local — an end-to-end local AI solution that runs entirely on-device. These samples cover chat completions, embeddings, audio transcription, tool calling, LangChain integration, and more.

> **New to Foundry Local?** Check out the [main README](../README.md) for an overview and quickstart, or visit the [Foundry Local documentation](https://learn.microsoft.com/azure/foundry-local/) on Microsoft Learn.

## Samples by Language

| Language | Samples | Description |
|----------|---------|-------------|
| [**C#**](cs/) | 15 | .NET SDK samples including native chat, embeddings, audio transcription, tool calling, model management, web server, vision via Responses API, tutorials, and WinML EP verification. Uses WinML on Windows for hardware acceleration. |
| [**JavaScript**](js/) | 17 | Node.js SDK samples including native chat, embeddings, audio transcription, Electron desktop app, Copilot SDK integration, LangChain, tool calling, web server, vision via Responses API, tutorials, and WinML EP verification. |
| [**Python**](python/) | 15 | Python samples built on the current `sdk_v2` typed session APIs for native chat, embeddings, audio transcription, tool calling, BYOM local registration, tutorials, plus web-service/OpenAI-compatible integrations where that is the sample’s intent. |
| [**Rust**](rust/) | 13 | Rust samples built on the current `sdk_v2` typed session APIs for native chat, embeddings, audio transcription, tool calling, BYOM local registration, tutorials, web server, vision via Responses API, and WinML EP verification. |
| [**C++**](cpp/) | 1 | C++ sample showing `sdk_v2` real-time audio transcription with an `AudioSession`, audio format descriptor, and streamed `ItemQueue` input. |

Additional [C++ SDK examples](../sdk_v2/cpp/examples/) cover chat completions, tool calling, embeddings, and real-time audio transcription. See the [C/C++ SDK guide](../sdk_v2/cpp/README.md#install-from-nuget) for NuGet installation and integration instructions, or the [C API guide](../sdk_v2/cpp/README.md#c-api) to integrate the runtime directly from C.
