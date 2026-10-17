# Foundry Local — BYOM local registration (Python)

This sample demonstrates the supported **v2** bring-your-own-model flow for
Python:

1. Resolve and download a catalog model (`qwen2.5-0.5b`).
2. Clone its model directory to a new location.
3. Use `manager.get_catalog(CatalogType.LOCAL)` to get the mutable local catalog.
4. Register the clone explicitly with:
   - a canonical model ID like `<alias>:1`
   - `ModelInfoBuilder().set_string_property("task", "chat-completion")`
5. Load the registered model and run a typed `ChatSession`.
6. Unload, unregister, and delete the cloned directory.

This replaces the old cache-rescan demo. In v2, the supported BYOM path is
**explicit local catalog registration**, not dropping arbitrary directories into
the cache and expecting `get_model(alias)` to discover them.

## Run

```bash
cd samples/python/byom-cache-rescan
pip install -r requirements.txt
python src/app.py
```
