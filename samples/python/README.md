# Foundry Local Python samples

These seven samples use the published `foundry-local-sdk` **2.2.1** package. Except for
`web-server`, inference runs in-process without an HTTP service.

| Sample | What it demonstrates |
| --- | --- |
| [`hello-world`](hello-world/) | Manager, execution providers, catalog/model lifecycle, and native chat |
| [`realtime-transcription`](realtime-transcription/) | Live microphone PCM input and native streaming transcription |
| [`embeddings`](embeddings/) | Native batched embeddings with `EmbeddingsSession` |
| [`tool-calling`](tool-calling/) | Native chat tool definitions, calls, and results |
| [`web-server`](web-server/) | Built-in server and its OpenAI-compatible endpoint |
| [`vision`](vision/) | Native chat with text and a local image |
| [`byom`](byom/) | Registering local model assets and running native chat |

> [!IMPORTANT]
> Every sample pins [`foundry-local-sdk` from PyPI](https://pypi.org/project/foundry-local-sdk/2.2.1/) to 2.2.1.
> Installation supplies the native runtime; no local SDK source build is required. See the
> [get-started documentation](https://learn.microsoft.com/en-us/azure/foundry-local/get-started) for versioned setup
> guidance and additional examples.

## Prerequisites

- Python 3.11 or later.
- A supported Foundry Local platform.
- Microphone access for `realtime-transcription`.

## Run a sample from the repository

Each directory pins the released SDK in `requirements.txt`:

```bash
cd samples/python/hello-world
python -m venv .venv
source .venv/bin/activate        # Windows: .venv\Scripts\activate
python -m pip install -r requirements.txt
python src/app.py
```

Use the same commands from any of the other sample directories. The first run
may download execution providers and a model. Subsequent runs reuse the local
cache.

All samples set the SDK application name to `foundry_local_samples`.
