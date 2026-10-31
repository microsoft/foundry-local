# Vision

Send text and a local image together to a native `ChatSession`. Choose a catalog
model with task `vision-language-chat` and supply your own JPEG or PNG.

After the SDK setup in the [Python samples README](../README.md):

```bash
python -m pip install -r requirements.txt
python src/app.py VISION_MODEL_ALIAS /path/to/image.png
```

The sample downloads the model only when it is not cached and unloads it after
inference. Images stay local; no HTTP service or imaging dependency is needed.
