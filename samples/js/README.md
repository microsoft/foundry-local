# Foundry Local JavaScript samples

These seven samples use the published `foundry-local-sdk` **2.2.1** package and run on Node.js 20 or later.

| Sample | What it demonstrates |
| --- | --- |
| [hello-world](hello-world/) | Discover execution providers, select/download/load a model, and run native `ChatSession` inference. |
| [realtime-transcription](realtime-transcription/) | Stream raw PCM audio through `ItemQueue` to a native `AudioSession`. |
| [embeddings](embeddings/) | Generate native text embeddings with `EmbeddingsSession`. |
| [tool-calling](tool-calling/) | Register a native chat tool, execute a returned `ToolCallItem`, and submit a `ToolResultItem`. |
| [web-server](web-server/) | Start the built-in server, discover its URL, and call its OpenAI-compatible endpoint. |
| [vision](vision/) | Send text and a local image to a native `ChatSession`. |
| [byom](byom/) | Register existing model assets in the local catalog and run native chat. |

> [!IMPORTANT]
> Every sample pins [`foundry-local-sdk` from npm](https://www.npmjs.com/package/foundry-local-sdk/v/2.2.1) to 2.2.1.
> Installation supplies the native runtime; no local SDK source build is required. See the
> [get-started documentation](https://learn.microsoft.com/en-us/azure/foundry-local/get-started) for versioned setup
> guidance and additional examples.

## Prerequisites

- Node.js 20 or later
- Native Foundry Local prerequisites for your platform
- Network access for execution-provider and model downloads when they are not already cached

Install and run any sample:

```bash
cd samples/js/hello-world
npm install
npm start
```

Samples that accept an optional model alias document it in their own README. The tool-calling sample intentionally
uses `qwen2.5-0.5b`, whose current catalog variants publish the metadata required by structured tool calling. The
non-web samples invoke the native in-process session APIs; only `web-server` starts the built-in HTTP service.
