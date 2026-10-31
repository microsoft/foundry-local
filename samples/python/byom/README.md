# Bring your own model

Register an existing model directory in the local catalog, run native chat, then
unregister it without deleting its assets.

Supply an already exported, compatible ONNX Runtime GenAI chat model directory
containing `genai_config.json`, weights, and tokenizer files. Provider selection
comes from that config; the sample does not convert, copy, or download model assets.

After the SDK setup in the [Python samples README](../README.md):

```bash
python -m pip install -r requirements.txt
python src/app.py /path/to/model-directory
```

The sample registers `sample-byom:1` with task `chat-completion` in the local
catalog, rather than rescanning the public model cache.
