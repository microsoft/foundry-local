# Vision

Send a text question and a local image together in a native `ChatSession` request.
Choose a catalog model with task `vision-language-chat` and supply your own JPEG or PNG.

After the SDK setup in the [JavaScript samples README](../README.md):

```bash
npm install
npm start -- VISION_MODEL_ALIAS /path/to/image.png
```

The sample downloads the model only when it is not cached and unloads it after inference.
Images stay local; no HTTP service or image-conversion dependency is needed.
