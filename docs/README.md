# 🎓 Foundry Local Documentation

Documentation for Foundry Local can be found in the following resources:

- [Microsoft Learn](https://learn.microsoft.com/azure/foundry-local/): This is the official documentation for Foundry Local, providing comprehensive guides, tutorials, and reference materials to help you get started and make the most of Foundry Local.
- SDK Reference:
    - [C# SDK Reference](../sdk/cs/README.md): This documentation provides detailed information about the C# SDK for Foundry Local, including API references, usage examples, and best practices for integrating Foundry Local into your applications.
    - [JavaScript SDK Reference](../sdk/js/README.md): This documentation offers detailed information about the JavaScript SDK for Foundry Local, including API references, usage examples, and best practices for integrating Foundry Local into your web applications.
    - [Python SDK Reference](../sdk/python/README.md): This documentation provides detailed information about the Python SDK for Foundry Local, including API references, usage examples, and best practices for integrating Foundry Local into your Python applications.
    - [Rust SDK Reference](../sdk/rust/README.md): This documentation provides detailed information about the Rust SDK for Foundry Local, including API references, usage examples, and best practices for integrating Foundry Local into your Rust applications.
- [Foundry Local Lab](https://github.com/Microsoft-foundry/foundry-local-lab): This GitHub repository contains a lab designed to help you learn how to use Foundry Local effectively. It includes hands-on exercises, sample code, and step-by-step instructions to guide you through the process of setting up and using Foundry Local in various scenarios.

## Supported Capabilities

Foundry Local is a unified local AI runtime that supports both **text generation** and **speech-to-text** through a single SDK:

| Capability | Model Aliases | SDK API |
|------------|--------------|---------|
| Chat Completions (Text Generation) | `phi-3.5-mini`, `qwen2.5-0.5b`, etc. | `model.createChatClient()` |
| Audio Transcription (Speech-to-Text) | `whisper-tiny` | `model.createAudioClient()` |

## Samples

- [JavaScript: Native Chat Completions](../samples/js/native-chat-completions/) — Chat completions using the native SDK API
- [JavaScript: Audio Transcription](../samples/js/audio-transcription-example/) — Speech-to-text with Whisper
- [JavaScript: Chat + Audio](../samples/js/chat-and-audio-foundry-local/) — Unified chat and audio in one app
- [JavaScript: Tool Calling](../samples/js/tool-calling-foundry-local/) — Function calling with local models
- [JavaScript: Electron Chat App](../samples/js/electron-chat-application/) — Desktop chat application
- [C#: Samples](../samples/cs/) — C# SDK examples including audio transcription
