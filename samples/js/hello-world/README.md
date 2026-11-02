# Hello world

This sample covers the complete native chat lifecycle: manager creation, execution-provider discovery and download,
catalog/model selection, cache-aware model download, model load, `ChatSession` inference, session disposal, model
unload, and manager disposal.

```bash
npm install
npm start
```

The default model alias is `qwen2.5-0.5b`. Pass another chat model alias as the first argument:

```bash
npm start -- phi-3.5-mini
```
