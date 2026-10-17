# BYOM registration with SDK v2 (C#)

This sample registers a bring-your-own-model while the manager is running, looks
it up in the local catalog, runs streaming chat, unloads it, and unregisters it.
The directory name is retained from the earlier cache-rescan sample; SDK v2 uses
explicit registration instead of discovering arbitrary folders in the public cache.

Follow the [shared C# setup](../README.md), then run `dotnet run` here.

The sample downloads `qwen2.5-0.5b` if needed and clones its assets using hardlinks
where supported. It registers the clone with:

```csharp
var localCatalog = await mgr.GetCatalogAsync(CatalogType.Local);
await localCatalog.RegisterModelAsync(cloneDir, $"{byomAlias}:1",
    new ModelInfoBuilder().SetStringProperty(ModelInfoPropertyKeys.Task, "chat-completion"));
var model = await localCatalog.GetModelAsync(byomAlias);
```

Local registrations borrow their asset directories. Unregistering does not delete
assets; this sample removes only its own clone during cleanup.

Exit code 0 means the lifecycle completed, 1 means source assets were unavailable,
and 3 reports an error during registration or inference.
