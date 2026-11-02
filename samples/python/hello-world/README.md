# Hello world

This end-to-end native sample creates the manager, discovers and registers
execution providers, opens the catalog, selects and cache-aware downloads a
chat model, loads it, streams the response with `ChatSession`, and
deterministically unloads and closes everything.

```bash
python -m pip install -r requirements.txt
python src/app.py
```
