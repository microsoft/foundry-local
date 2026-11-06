# Foundry Local Samples

These focused examples demonstrate the Foundry Local v2 SDKs with the same seven capabilities in every supported
language. Except for the web-server sample, inference runs natively in the application process.

> **New to Foundry Local?** Start with the [main README](../README.md), then choose a language below.

> [!IMPORTANT]
> All samples use the published Foundry Local SDK **2.2.1** packages from NuGet, npm, PyPI, or crates.io.
> No local SDK source build is required. Follow the setup instructions for your language below. For more examples, see the
> [Foundry Local get-started documentation](https://learn.microsoft.com/en-us/azure/foundry-local/get-started).

## Languages

| Language | SDK | Samples |
|----------|-----|---------|
| [C++](cpp/) | [`sdk_v2/cpp`](../sdk_v2/cpp/) | 7 |
| [C#](cs/) | [`sdk_v2/cs`](../sdk_v2/cs/) | 7 |
| [JavaScript](js/) | [`sdk_v2/js`](../sdk_v2/js/) | 7 |
| [Python](python/) | [`sdk_v2/python`](../sdk_v2/python/) | 7 |
| [Rust](rust/) | [`sdk_v2/rust`](../sdk_v2/rust/) | 7 |

## Capabilities

Every language contains:

| Sample | Demonstrates |
|--------|--------------|
| `hello-world` | Manager creation, execution-provider setup, catalog/model selection, download, load, native chat inference, and cleanup |
| `realtime-transcription` | Native, in-process streaming speech transcription |
| `embeddings` | Native embedding generation and similarity comparison |
| `tool-calling` | Native tool definitions, tool execution, and multi-turn continuation |
| `web-server` | Starting and calling the built-in OpenAI-compatible HTTP server |
| `vision` | Sending a local image and text together in a native chat request |
| `byom` | Registering existing local model assets, running native chat, and unregistering |

All samples use `foundry_local_samples` as the application name so they share the same local model cache.
Each sample registers execution providers before selecting its model. Hello world uses automatic model selection
and a streaming chat request about the sky; embeddings compares three sentences about cats and Saturn; tool calling
requests Paris weather and returns a mock result before a final answer with tools disabled. Microphone capture and
resource cleanup use the conventions of each language.

## Vision and BYOM inputs

`vision` takes a `vision-language-chat` model alias from the catalog and a path to your own image, such as a JPEG or
PNG. The image is read locally; no image download, base64 encoding, or HTTP service is needed.

`byom` takes a directory containing an already exported, compatible ONNX Runtime GenAI chat model, including
`genai_config.json`, model weights, and tokenizer files. It does not convert or download model assets. Each example
registers `sample-byom:1` in the **local** catalog with task `chat-completion`, then unregisters it on normal
completion without deleting your files. The provider configuration comes from the model's `genai_config.json`.
