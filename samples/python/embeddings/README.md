# Embeddings

Generates a batch of embeddings in-process with the native
`EmbeddingsSession` API, decodes the returned float tensors, and compares the
cosine similarities of three sentences.

```bash
python -m pip install -r requirements.txt
python src/app.py
```
