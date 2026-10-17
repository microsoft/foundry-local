# 🎓 Foundry Local Documentation

Documentation for Foundry Local can be found in the following resources:

- [Microsoft Learn](https://learn.microsoft.com/azure/foundry-local/): This is the official documentation for Foundry Local, providing comprehensive guides, tutorials, and reference materials to help you get started and make the most of Foundry Local.
- SDK Reference:
    - [C API Reference](../sdk_v2/cpp/README.md#c-api): Use the stable C ABI directly through versioned function tables and opaque handles. Includes integration and resource-management guidance and links to the public C header.
    - [C++ SDK Reference](../sdk_v2/cpp/README.md): Install from NuGet and integrate the header-only C++17 RAII wrapper, with examples for chat, tool calling, embeddings, and audio transcription. No source build is required.
    - [C# SDK Reference](../sdk_v2/cs/README.md): This documentation provides detailed information about the C# SDK for Foundry Local, including API references, usage examples, and best practices for integrating Foundry Local into your applications.
    - [JavaScript SDK Reference](../sdk_v2/js/README.md): This documentation offers detailed information about the JavaScript SDK for Foundry Local, including API references, usage examples, and best practices for integrating Foundry Local into your web applications.
    - [Python SDK Reference](../sdk_v2/python/README.md): This documentation provides detailed information about the Python SDK for Foundry Local, including API references, usage examples, and best practices for integrating Foundry Local into your Python applications.
    - [Rust SDK Reference](../sdk_v2/rust/README.md): This documentation provides detailed information about the Rust SDK for Foundry Local, including API references, usage examples, and best practices for integrating Foundry Local into your Rust applications.
- [Foundry Local Lab](https://github.com/Microsoft-foundry/foundry-local-lab): This GitHub repository contains a lab designed to help you learn how to use Foundry Local effectively. It includes hands-on exercises, sample code, and step-by-step instructions to guide you through the process of setting up and using Foundry Local in various scenarios.

## Supported Capabilities

Foundry Local supports C, C++, C#, JavaScript, Python, and Rust. The runtime is implemented in C++ and exposes a stable C ABI used by the language bindings. C and C++ are supported application interfaces, not just implementation details: use the C API directly or the C++17 RAII wrapper over it.

Foundry Local is a unified local AI runtime that supports both **text generation** and **speech-to-text** through a single SDK:

| Capability | Model Aliases | SDK API |
|------------|--------------|---------|
| Chat Completions (Text Generation) | `phi-3.5-mini`, `qwen2.5-0.5b`, etc. | `model.createChatClient()` |
| Audio Transcription (Speech-to-Text) | `whisper-tiny` | `model.createAudioClient()` |

## Samples

- [C++: Native SDK Examples](../sdk_v2/cpp/examples/) — Chat completions, tool calling, embeddings, and real-time audio transcription
- [C++: Live Audio Transcription](../samples/cpp/live-audio-transcription/) — Microphone input and streaming transcription
- [JavaScript: Native Chat Completions](../samples/js/native-chat-completions/) — Chat completions using the native SDK API
- [JavaScript: Audio Transcription](../samples/js/audio-transcription-example/) — Speech-to-text with Whisper
- [JavaScript: Chat + Audio](../samples/js/chat-and-audio-foundry-local/) — Unified chat and audio in one app
- [JavaScript: Tool Calling](../samples/js/tool-calling-foundry-local/) — Function calling with local models
- [JavaScript: Electron Chat App](../samples/js/electron-chat-application/) — Desktop chat application
- [C#: Samples](../samples/cs/) — C# SDK examples including audio transcription
