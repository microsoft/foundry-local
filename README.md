<div align="center">
  <picture align="center">
    <source media="(prefers-color-scheme: dark)" srcset="media/icons/foundry_local_white.svg">
    <source media="(prefers-color-scheme: light)" srcset="media/icons/foundry_local_black.svg">
    <img alt="Foundry Local icon." src="media/icons/foundry_local_black.svg" height="100" style="max-width: 100%;">
  </picture>
<div id="user-content-toc">
  <ul align="center" style="list-style: none;">
    <summary>
      <h1>Foundry Local</h1><br>
     <h3><a href="https://aka.ms/foundry-local-installer">Download</a> | <a href="https://aka.ms/foundry-local-docs">Documentation</a> | <a href="https://aka.ms/foundry-local-discord">Discord</a></h3>
    </summary>
  </ul>
</div>

## Ship on-device AI inside your app

</div>

Foundry Local is an **end-to-end local AI solution** for building applications that run entirely on the user's device. It provides native SDKs (C, C++, C#, JavaScript, Python, and Rust), a curated catalog of optimized models, and automatic hardware acceleration — all in a lightweight package (~20 MB). The compact size makes it easy to integrate into your application and distribute to end users.

The runtime is implemented in C++ and exposes a stable C ABI that underpins the other language bindings. C and C++ applications can use it directly: the [C API](sdk_v2/cpp/README.md#c-api) provides versioned function tables and opaque handles, while the [C++ SDK](sdk_v2/cpp/README.md) provides a header-only C++17 wrapper with RAII resource management.

Conversations never leave the device, responses start immediately with zero network latency, and your app works offline. No per-token costs, no API keys, no backend infrastructure to maintain, and no Azure subscription required.

Foundry Local may collect usage data and send it to Microsoft to help improve our products and services. See the [privacy statement](sdk_v2/cpp/docs/Privacy.md) for more details.

### Key Features

- **Lightweight runtime** — The runtime handles model acquisition, hardware acceleration, model management, and inference (via [ONNX Runtime](https://onnxruntime.ai/)). 

- **Curated model catalog** — A catalog of high-quality models optimized for on-device use across a wide range of consumer hardware. The catalog covers chat completions (for example, GPT OSS, Qwen, DeepSeek, Mistral and Phi) and audio transcription (for example, Whisper). Every model goes through extensive quantization and compression to deliver the best balance of quality and performance. Models are versioned, so your application can pin to a specific version or automatically receive updates.

- **Automatic hardware acceleration** — Foundry Local detects the available hardware on the user's device and selects the best execution provider and device (NPU, GPU or CPU).

- **Smart model management** — Foundry Local handles the full lifecycle of models on end-user devices. Models download automatically on first use, are cached locally for instant subsequent launches, and the best-performing variant is selected for the user's specific hardware.

- **OpenAI-compatible API** — Supports OpenAI request and response formats including the [OpenAI Responses API format](https://developers.openai.com/api/reference/resources/responses). If your application already uses the OpenAI SDK, point it to a Foundry Local endpoint with minimal code changes.

- **Optional local server** — An OpenAI-compatible web server for serving models to multiple processes, integrating with tools like LangChain, or experimenting through REST calls. For most embedded application scenarios, use the SDK directly — it runs inference in-process without the overhead of a separate server.


## 🚀 Quickstart

> [!TIP]
> The following shows a quickstart for Python and JavaScript. C, C++, C#, and Rust are also supported. Take a look at the [samples](/samples/) for more details.

<details open>
<summary><strong>JavaScript</strong></summary>

1. Install the SDK:

    ```bash
    npm install --save-exact foundry-local-sdk@2.2.1
    ```
2. Run your first chat completion:

    ```javascript
    import { ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';

    const manager = FoundryLocalManager.create({ appName: 'foundry_local_samples' });
    const model = await manager.catalog.getModel('qwen2.5-0.5b');
    try {
        if (!model.isCached) await model.download();
        await model.load();
        try {
            const session = new ChatSession(model);
            try {
                const request = new Request().addItem(
                    Item.userMessage('What is the golden ratio?')
                );
                for await (const item of session.processStreamingRequest(request)) {
                    if (item.type === 'text') process.stdout.write(item.text);
                }
            } finally {
                session.dispose();
            }
        } finally {
            await model.unload();
        }
    } finally {
        manager.dispose();
    }
    ```

</details>


<details open>
<summary><strong>Python</strong></summary>

1. Install the SDK:

    ```bash
    pip install foundry-local-sdk==2.2.1
    ```
2. Run your first chat completion:

    ```python
    from foundry_local_sdk import (
        ChatSession, Configuration, FoundryLocalManager,
        MessageItem, Request,
    )

    with FoundryLocalManager(Configuration(app_name="foundry_local_samples")) as manager:
        ep_result = manager.download_and_register_eps()
        if not ep_result.success:
            raise RuntimeError(f"EP registration failed: {ep_result.failed_eps}")
        model = manager.catalog.get_model("qwen2.5-0.5b")
        if model is None:
            raise RuntimeError("Model not found")
        if not model.is_cached:
            model.download()
        model.load()
        try:
            with ChatSession(model) as session:
                with Request().add_item(MessageItem.user("What is the golden ratio?")) as request:
                    with session.process_request(request) as response:
                        for item in response:
                            if isinstance(item, MessageItem):
                                print(item.get_simple_text())
        finally:
            model.unload()
    ```

</details>


### 💬 Audio Transcription (Speech-to-Text)

The SDK also supports native audio transcription in C, C++, C#, JavaScript, Python, and Rust. See the
[`realtime-transcription` samples](samples/) and the
[Foundry Local get-started documentation](https://learn.microsoft.com/en-us/azure/foundry-local/get-started).

## 📦 Samples

Explore complete working examples in the [`samples/`](samples/) folder:

| Language | Samples | Highlights |
| --- | --- | --- |
| [**C++**](samples/cpp/) | 7 | Hello world, real-time transcription, embeddings, tool calling, web server, vision, and BYOM |
| [**C#**](samples/cs/) | 7 | Hello world, real-time transcription, embeddings, tool calling, web server, vision, and BYOM |
| [**JavaScript**](samples/js/) | 7 | Hello world, real-time transcription, embeddings, tool calling, web server, vision, and BYOM |
| [**Python**](samples/python/) | 7 | Hello world, real-time transcription, embeddings, tool calling, web server, vision, and BYOM |
| [**Rust**](samples/rust/) | 7 | Hello world, real-time transcription, embeddings, tool calling, web server, vision, and BYOM |

C developers can use the [C API guide](sdk_v2/cpp/README.md#c-api) and [public C header](sdk_v2/cpp/include/foundry_local/foundry_local_c.h) to integrate the same runtime directly.

## 🖥️ CLI

The Foundry Local CLI lets you explore models and experiment interactively.

**Install (public preview):**

Download the asset for your platform from the [`cli-preview-0.11.0`](https://github.com/microsoft/Foundry-Local/releases/tag/cli-preview-0.11.0) GitHub release.

**Run a model:**

```bash
foundry run qwen2.5-0.5b
```

**List available models:**

```bash
foundry model list
```

> For the full CLI reference and advanced usage, see the [CLI documentation on Microsoft Learn](https://learn.microsoft.com/en-us/azure/foundry-local/reference/reference-cli).


## Reporting Issues

Please report issues or suggest improvements in the [GitHub Issues](https://github.com/microsoft/Foundry-Local/issues) section.

## 🎓 Learn More

- [Foundry Local Documentation](https://learn.microsoft.com/en-us/azure/foundry-local/) on Microsoft Learn
- [Foundry Local Lab](https://github.com/Microsoft-foundry/foundry-local-lab) — Hands-on exercises and step-by-step instructions

## ❔ Frequently asked questions

### Can inference be cancelled?

Inference cancellation is cooperative. Low-level requests can be cancelled with `Request.Cancel()`, while streaming APIs provide language-specific cancellation mechanisms. Cancellation may not take effect until the current generation step completes, and inference requests do not currently have a built-in timeout.

### Is Foundry Local a web server and CLI tool?

No. Foundry Local is an **end-to-end local AI solution** that your application ships with. It handles model acquisition, hardware acceleration, and inference inside your app process through the SDK. The optional web server and CLI are available for development workflows, but the core product is the local AI runtime and SDK that you integrate directly into your application.

### Why doesn't Foundry Local support every available model?

Foundry Local is designed for shipping production applications, not for general-purpose model experimentation. The model catalog is intentionally curated to include models that are optimized for specific application scenarios, tested across a range of consumer hardware, and small enough to distribute to end users. This approach ensures that every model in the catalog delivers reliable performance when embedded in your application — rather than offering a broad selection of models with unpredictable on-device behavior.

### Can Foundry Local run on a server?

Foundry Local is optimized for hardware-constrained devices where a single user accesses the model at a time. While you can technically install and run it on server hardware, it isn't designed as a server inference stack.

Server-oriented runtimes like [vLLM](https://docs.vllm.ai/en/latest/) or [Triton Inference Server](https://github.com/triton-inference-server/server) are built for multi-user scenarios — they handle concurrent request queuing, continuous batching, and efficient GPU sharing across many simultaneous clients. Foundry Local doesn't provide these capabilities. Instead, it focuses on lightweight, single-user inference with automatic hardware detection, KV-cache management, and model lifecycle handling that make sense for client applications.

If you need to serve models to multiple concurrent users, use a dedicated server inference framework. Use Foundry Local when the model runs on the end user's own device.


### What platforms are supported?

Foundry Local supports Windows, macOS (Apple silicon), and Linux.


## ⚖️ License

Foundry Local SDK is licensed under the MIT license. For more details, see the [LICENSE](LICENSE) file.
Foundry Local CLI is licensed under the Microsoft Software License Terms. For more details, read the [LICENSE](LICENSE) file.

Individual models made available for use with Foundry Local are subject to the each model's license terms, notices, and use restrictions. Refer to the model's documentation or download/listing page for the applicable terms before using or redistributing a model.
