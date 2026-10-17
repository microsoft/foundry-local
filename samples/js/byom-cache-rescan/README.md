# Foundry Local — BYOM local registration (JavaScript)

This sample demonstrates the supported **v2** bring-your-own-model flow for JavaScript:

1. Resolve and download a catalog model (`qwen2.5-0.5b`).
2. Clone its model directory to a new location.
3. Use `manager.getCatalog(CatalogType.Local)` to get the mutable local catalog.
4. Register the clone explicitly with:
   - a canonical model ID like `<alias>:1`
   - `ModelInfoBuilder().setStringProperty("task", "chat-completion")`
5. Load the registered model and run a typed `ChatSession`.
6. Unload, unregister, and delete the cloned directory.

This replaces the old cache-rescan demo. In v2, the supported BYOM path is
**explicit local catalog registration**, not dropping arbitrary directories into
the cache and hoping `getModel(alias)` discovers them.

## Run

```bash
cd samples/js/byom-cache-rescan
npm install
node app.js
```

## Key APIs

```js
const localCatalog = manager.getCatalog(CatalogType.Local);
using metadata = new ModelInfoBuilder();
metadata.setStringProperty("task", "chat-completion");

const model = await localCatalog.registerModel(modelPath, "my-local-model:1", metadata);
await localCatalog.unregisterModel("my-local-model:1");
```
