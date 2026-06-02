# C# sample cleanup pattern

Always dispose `*Session` BEFORE calling `await model.UnloadAsync()`.

An implicit `using var session = new ChatSession(model);` (or `EmbeddingsSession`/`AudioSession`)
declared in `Main` outlives the `UnloadAsync()` call because the `using` scope only ends when
`Main` returns. The native layer then rejects the unload with:

```
model_load_manager.cc:190 fl::ModelLoadManager::UnloadModel
  cannot unload model '<id>': 1 session(s) still using it
```

Fix: call `session.Dispose();` (or scope the session in a `{ ... }` block) immediately before
`await model.UnloadAsync();`.
