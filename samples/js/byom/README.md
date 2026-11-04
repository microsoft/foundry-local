# Bring your own model

Register an existing model directory in the local catalog, run a native chat request,
then unregister it without deleting its assets.

Supply an already exported, compatible ONNX Runtime GenAI chat model directory containing
`genai_config.json`, weights, and tokenizer files. The model's config selects its execution provider;
the sample does not convert, copy, or download model assets.

After the SDK setup in the [JavaScript samples README](../README.md):

```bash
npm install
npm start -- /path/to/model-directory
```

Registration uses `sample-byom:1` with task `chat-completion`. This is explicit local-catalog
registration, not a rescan of the public model cache.
