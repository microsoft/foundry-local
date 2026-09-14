# Local coding endpoint

Serve an existing ONNX Runtime GenAI coding model through Foundry Local, then send concurrent requests through
either the OpenAI-compatible endpoint or the typed Foundry Local API. The sample does not download or delete model
assets.

## Qualified setup

Use Python 3.11 or later. The integrated SDK requires ONNX Runtime 1.30.0 and ONNX Runtime GenAI 0.16.0.

> [!IMPORTANT]
> The package source and SDK version below are placeholders until the release containing the public local-catalog
> BYOM API is published. Replace both placeholders with values from that release; do not treat this as a currently
> published pin.

```bash
python -m venv .venv
source .venv/bin/activate
python -m pip install --extra-index-url "https://<PACKAGE-SOURCE>/simple" \
  "foundry-local-sdk==<VERSION-CONTAINING-LOCAL-CATALOG-BYOM>" \
  "onnxruntime-gpu==1.30.0" \
  "onnxruntime-genai-cuda==0.16.0"
python -m pip install -r requirements.txt
```

If the released `foundry-local-sdk` provides a qualified CUDA dependency bundle, install that bundle instead of
separately installing the three packages above, while retaining ORT 1.30.0 and ORT GenAI 0.16.0.

On the qualified single-GPU setup, expose GPU 0 before launching:

```bash
export CUDA_VISIBLE_DEVICES=0
```

This makes CUDA device selection deterministic. It does not install a CUDA driver or toolkit.

## Start the endpoint

`MODEL_DIR` must be a filesystem directory containing `genai_config.json`.

```bash
python launcher.py --model "$MODEL_DIR"
```

The launcher downloads/registers `CUDAExecutionProvider` by default. Use `--skip-cuda-ep` only when it is already
registered or CUDA setup is intentionally managed elsewhere. The default model ID is `local-coding-cuda:1` and the
default endpoint is `http://127.0.0.1:5272/v1`; use `--model-id`, `--host`, and `--port` to change them.

The launcher uses only the public local-catalog BYOM API. It registers the directory only when that model ID is not
already registered, loads the model, starts the endpoint, and waits for Ctrl+C or SIGTERM. Shutdown stops the
endpoint, unloads the model, unregisters registrations created by this process, and closes the manager. Registration
is non-owning: unregistering never deletes the model directory. The sample registers Qwen-compatible tool-call and
reasoning capabilities so coding harnesses receive structured tool calls and separate reasoning instead of raw model
markup. Foundry resolves the corresponding delimiter tokens through the ONNX Runtime GenAI tokenizer APIs. Change
the capability values when using a model package with different behavior.

## Concurrent OpenAI requests

With the launcher still running, in another shell:

```bash
source .venv/bin/activate
python openai_client.py
```

## Concurrent typed requests

Stop the endpoint launcher first, then run the standalone typed example:

```bash
source .venv/bin/activate
export CUDA_VISIBLE_DEVICES=0
python typed_client.py --model "$MODEL_DIR"
```

Each worker creates and closes its own `ChatSession`; no mutable session is shared between concurrent tasks. The
typed example owns its own manager/model lifecycle and performs the same non-owning, temporary local registration.
