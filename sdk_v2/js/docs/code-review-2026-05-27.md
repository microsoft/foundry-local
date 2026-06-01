# JS SDK v2 Code Review — 2026-05-27

Review of `sdk_v2/js` as a port of `sdk/js` (v1). v2 must be backwards-compatible with v1 (not a breaking change), with the only allowed exception being types made `Disposable` for correct cleanup.

## Summary

**Status: All findings resolved or closed.** v2 is cleared as a non-breaking port of v1 for the public surface and TypeScript types in scope.

The native addon code is well-implemented with proper N-API patterns (ThreadSafeFunction, buffer pinning, disposed-state guards, manager-pinning for dependent objects). The architecture is sound. The original review surfaced several backwards-compatibility breaks and one resource-management issue; all have since been fixed in source or explicitly closed as out of scope / not-a-bug (see per-issue Status below).

Compatibility policy applied in this review update:
- Public surface + TypeScript type compatibility are both in scope
- Internal (`@internal`) exports are out of scope
- Cross-language behavior was compared against v1 C#, v1 Python, and v1 C++ for live audio session lifecycle

---

## Critical Issues

### ~~1. Missing `createResponsesClient` method on IModel~~ — NOT A BUG

> **Resolved:** The Responses API (`createResponsesClient`, `ResponsesClient`, `ResponsesClientSettings`, `getOutputText`, and all associated types) was accidentally added to the v1 SDK and is being deliberately excluded from v2. This is not a backwards-compatibility break that needs fixing.

---

### ~~2. `LiveAudioTranscriptionSession.dispose()` is sync in v2 but async in v1~~ — FIXED

| | |
|---|---|
| **File** | `sdk_v2/js/src/openai/liveAudioSession.ts:161` |
| **Severity** | High |
| **Status** | ✅ Fixed |

v1's `LiveAudioTranscriptionSession.dispose()` is `async` (`async dispose(): Promise<void>`, line 400-408), calling `await this.stop()` for graceful shutdown. v2's is synchronous (`dispose(): void`, line 161-181), just marking the queue finished and calling `#completeStream()` without awaiting the processing promise. This is both:
- A breaking signature change (callers who `await session.dispose()` get different behavior)
- A potential resource leak (native resources may not be fully cleaned up)

**Resolution:** v2 now uses async `dispose(): Promise<void>`, awaits shutdown best-effort, and adds both `[Symbol.dispose]` and `[Symbol.asyncDispose]`.

---

### ~~3. `LiveAudioTranscriptionSession` cannot be restarted after `stop()`~~ — FIXED

| | |
|---|---|
| **File** | `sdk_v2/js/src/openai/liveAudioSession.ts:52` |
| **Severity** | High |
| **Status** | ✅ Fixed |

v2 only allows `start()` from `Created` state. After `stop()`, state becomes `Stopped`, and `start()` throws.

This differs from v1 JS/Python/C# behavior where calling `start()` after `stop()` is allowed for the same session object:
- v1 JS gates on `started` only and sets it back to `false` in `stop()`
- v1 Python gates on `_started` only and sets it back to `False` in `stop()`
- v1 C# gates on `_started` only and sets it back to `false` in `StopAsync()`

v1 C++ live-audio session is one-shot (`Created -> Started -> Stopped`), but v1 JS/Python/C# are aligned on restartability. Since this review targets JS v1 compatibility, this is a breaking behavioral change.

**Resolution:** `start()` now allows restart from `Stopped` and reinitializes per-run stream/native state.

---

## Medium Issues

### ~~4. Missing `providerType` and `modelSettings` fields on ModelInfo~~ — FIXED

| | |
|---|---|
| **File** | `sdk_v2/js/src/types.ts:9` |
| **Severity** | Medium |
| **Status** | ✅ Fixed |

v2 `ModelInfo` is missing fields present in v1:
- `providerType: string` (v1 `types.ts:37`)
- `modelSettings?: ModelSettings | null` (v1 `types.ts:42`)

Code accessing `model.info.providerType` will get `undefined` in v2 instead of a string value.

**Resolution:** `providerType` and `modelSettings` were restored in v2 compatibility shape.

Additional compatibility updates applied:
- `promptTemplate?: PromptTemplate | null` is retained and marked deprecated (matching C# v2 guidance)
- `runtime?: Runtime | null` restored
- `cached: boolean` restored
- `DeviceType` enum restored

---

### ~~5. Missing v1 internal exports (`ModelVariant`, `CoreInterop`, `Configuration`, `ModelLoadManager`)~~ — OUT OF SCOPE

| | |
|---|---|
| **File** | `sdk_v2/js/src/index.ts` |
| **Severity** | N/A |
| **Status** | ✅ Closed |

Per updated scope, internal symbols are excluded from compatibility requirements.

---

### ~~6. Streaming item queue — fragile wake pattern (correctness OK, maintainability concern)~~ — FIXED

| | |
|---|---|
| **File** | `sdk_v2/js/src/session.ts:108-231` |
| **Severity** | Low (informational) |
| **Status** | ✅ Fixed |

The `streamItems` function manages shared `queue`/`waiter` state between the native TSFN callback and the async iterator. The code IS correct (the iterator checks `queue.length > 0` before waiting), but the pattern is non-obvious. If the native `processStreamingRequest` fires `onItem` synchronously during setup, items are queued but no wakeup is needed since the iterator hasn't started waiting yet.

**Resolution:** Added a local comment at the enqueue site documenting why synchronous startup callbacks are safe.

---

### ~~7. `LiveAudioTranscriptionSession` missing `Symbol.dispose`~~ — FIXED

| | |
|---|---|
| **File** | `sdk_v2/js/src/openai/liveAudioSession.ts:23` |
| **Severity** | Medium |
| **Status** | ✅ Fixed |

`LiveAudioTranscriptionSession` has `dispose()` but no `[Symbol.dispose]()`, unlike `Session`, `ChatClient`, `AudioClient`, `EmbeddingClient`, and `ItemQueue` which all implement it. This prevents `using` syntax from working with this class, which is inconsistent with the rest of the SDK.

**Resolution:** Added `[Symbol.dispose]` and `[Symbol.asyncDispose]` on `LiveAudioTranscriptionSession`.

---



## Positive Findings

The following areas are well-implemented:

- **N-API addon**: Correct `Napi::ThreadSafeFunction` usage, proper buffer pinning via `napi_create_reference` with TSFN-bounced cleanup
- **Disposed-state guards**: All ObjectWrap methods check disposed state before proceeding
- **Manager-pinning**: Session holds Manager ref, preventing premature GC of the native manager
- **TypeScript types**: Well-structured discriminated unions for Items (per instructions)
- **Resource management**: Most classes properly implement `Disposable` with `Symbol.dispose`
- **Async patterns**: Correct use of promises and async iterators for streaming

---

## Checklist

- [x] Issue 1: ~~N/A — Responses API deliberately excluded (was accidental in v1)~~
- [x] Issue 2: ~~Make `LiveAudioTranscriptionSession.dispose()` async (or add async-dispose equivalent)~~
- [x] Issue 3: ~~Allow restart after `stop()` (or explicitly document intentional break)~~
- [x] Issue 4: ~~Add `providerType` and `modelSettings` to ModelInfo (and review remaining type drift)~~
- [x] Issue 5: ~~Internal exports are out of scope~~
- [x] Issue 6: ~~Add clarifying comment to streaming queue~~
- [x] Issue 7: ~~Add `Symbol.dispose` to `LiveAudioTranscriptionSession`~~
