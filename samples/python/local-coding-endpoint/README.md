# Qwen coding endpoint

Serve a local Qwen3.8 DFlash2 model through Foundry Local and send concurrent coding requests through its
OpenAI-compatible endpoint.

## Setup

Use Python 3.11 or later. Keep the Foundry Local package source and version placeholders below until the package is
published.

```bash
python -m venv .venv
source .venv/bin/activate
python -m pip install --extra-index-url "https://<PACKAGE-SOURCE>/simple" \
  "foundry-local-sdk==<FOUNDRY-LOCAL-VERSION>" \
  "onnxruntime==1.30.0" \
  "onnxruntime-genai-core==0.16.0"
python -m pip install -r requirements.txt
```

Use a CUDA-capable machine and select the GPU before launching:

```bash
export CUDA_VISIBLE_DEVICES=0
```

## Start the endpoint

`MODEL_DIR` must be a filesystem directory containing `genai_config.json`.

```bash
python launcher.py --model "$MODEL_DIR"
```

The launcher downloads and registers the CUDA execution provider, registers and loads the model as
`qwen38-dflash2-coding:1`, and starts `http://127.0.0.1:5272/v1`. Press Ctrl+C to stop it. The launcher then unloads
and unregisters the model without deleting the model files.

## Concurrent OpenAI requests

With the launcher still running, in another shell:

```bash
source .venv/bin/activate
python openai_client.py
```
