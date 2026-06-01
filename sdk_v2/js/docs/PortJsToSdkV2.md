# Foundry Local JavaScript SDK (v2) — Design

This document captures the architectural design and decisions for the v2
JavaScript / TypeScript SDK under `sdk_v2/js/`. The SDK is a native Node.js
binding for the Foundry Local **C++ SDK** and replaces the legacy `sdk/js/`
package, which spoke a now-removed .NET command-dispatch protocol.

The agent that owns implementation is [`JsCoder`](../../../.github/agents/JsCoder.agent.md).
Architecture decisions are owned by [`DearLeader`](../../../.github/agents/DearLeader.agent.md).
The canonical C ABI is [`foundry_local_c.h`](../../cpp/include/foundry_local/foundry_local_c.h)
and the C++ wrapper is [`foundry_local_cpp.h`](../../cpp/include/foundry_local/foundry_local_cpp.h).

For build / test / debug instructions see the [package README](../README.md).
For implementation conventions see
[`.github/instructions/js-sdk-v2.instructions.md`](../../../.github/instructions/js-sdk-v2.instructions.md)
and
[`.github/instructions/js-sdk-v2-items.instructions.md`](../../../.github/instructions/js-sdk-v2-items.instructions.md).

---

## Goals

1. Provide a native Node.js binding for Foundry Local that uses the same C
   ABI the C# and Python SDKs use.
2. Preserve the **public TypeScript API** shape of `sdk/js/` so existing
   consumers can recompile against `foundry-local-sdk@2.x` with minimal
   source changes (legacy class names are re-exported as stubs; full
   behavioural parity is not implemented — see [Current state](#current-state)).
3. Eliminate the legacy `.NET`-side command-dispatch ABI and the JS-side
   `CoreInterop` shim.
4. Add a new typed surface (`Session` / `ChatSession` / `AudioSession` /
   `EmbeddingsSession`, `Request`, `Response`, `Item` hierarchy) that
   mirrors the C# and Python v2 SDKs.
5. Stay small. The legacy package was a Node-API C addon for a reason —
   `koffi` and `ffi-napi` cost tens of MB at install time, which is
   unacceptable for an embedded SDK.

## Non-goals

- Browser support. This SDK loads a native binary; it is Node-only.
- Polyfilling sync wrappers on top of async I/O. There are no sync entry
  points in the v2 surface — every C ABI call that can block dispatches to
  a libuv worker and resolves a Promise.
- A new HTTP transport. Any HTTP client the legacy compat surface needs
  remains pure TypeScript talking HTTP to the embedded web service.

---

## Architecture

Five composing layers, top-down:

```
┌───────────────────────────────────────────────────────────────────┐
│  5. Legacy v1-compatible surface (stubs only — not implemented)   │
│     FoundryLocalManager, ChatClient, ResponsesClient, AudioClient,│
│     EmbeddingClient, LiveAudioTranscriptionSession,               │
│     ModelLoadManager, Configuration, getOutputText                │
├───────────────────────────────────────────────────────────────────┤
│  4. v2 public surface                                             │
│     Manager, Catalog, Model, Session, ChatSession, AudioSession,  │
│     EmbeddingsSession, Request, Response, ItemQueue, Item union   │
├───────────────────────────────────────────────────────────────────┤
│  3. TypeScript detail layer                                       │
│     Typed handle classes that own native pointers, AsyncIterable  │
│     stream adapters, AbortSignal plumbing, error mapping          │
├───────────────────────────────────────────────────────────────────┤
│  2. node-addon-api C++ addon                                      │
│     Napi::ObjectWrap<T> over std::unique_ptr<foundry_local::X>,   │
│     ThreadSafeFunction streaming bridge, AsyncWorker async paths  │
├───────────────────────────────────────────────────────────────────┤
│  1. foundry_local C++ wrapper (foundry_local_cpp.h)               │
│     RAII handles, exception-based error handling, typed accessors │
│  0. foundry_local C ABI (foundry_local_c.h)                       │
│     Versioned vtable, opaque handles, status returns              │
└───────────────────────────────────────────────────────────────────┘
```

### Why layer on the C++ wrapper instead of going straight to the C ABI

- The wrapper already solves RAII, error → exception mapping, and typed
  content accessors. Bypassing it forces every addon source file to redo
  that work, and it diverges from how the C# and Python SDKs are organised.
- The wrapper has no third-party dependencies, no ABI surface of its own,
  and is header-only. The addon links against the same `foundry_local`
  native library every other SDK loads.
- If the addon needs something the wrapper does not expose, the answer is
  to extend the wrapper (via `@ApiExpert`), not to drop down to the C ABI.

### Native addon language: C++20 + node-addon-api

- `node-addon-api` is the Node-maintained C++ wrapper over N-API. It is
  header-only, version-stable across Node majors, and adds roughly
  50–150 KB to the addon binary.
- The addon translation units compile at **C++20** — this matches node-gyp's
  default for MSVC and avoids the `D9025: /std:c++17 overridden with
  /std:c++20` warning. The **wrapper header** (`foundry_local_cpp.h`) stays
  C++17-consumable so external C++ projects on older toolchains can use it.
- `NAPI_VERSION=8`, `NAPI_DISABLE_CPP_EXCEPTIONS=0` (we use C++ exceptions
  internally and translate at the JS boundary).

---

## Async model

All v2 entry points that wrap a C ABI call are async. The addon wraps the
underlying `foundry_local::*` C++ call in a `Napi::AsyncWorker` (or the
TSFN-based streaming variant), dispatches on the libuv worker pool, and
resolves a Promise on completion. There are no sync entry points in the v2
surface — read-only accessors that don't perform I/O (`Model.getInfo`,
`Model.isCached`, etc.) are exposed as plain sync TS getters because the
underlying C ABI call is a memory copy, not I/O.

---

## Streaming

- `Session.processRequestStreaming` returns an `AsyncIterable<Item>`. Each
  native streaming-callback push lands on a `Napi::ThreadSafeFunction`
  acquired in the session's constructor and released when the iterable is
  closed.
- Cancellation: each async API accepts an `AbortSignal`. The signal is
  bound to `Request::Cancel()`, which the C++ wrapper translates into a
  cancellation signal observed by the streaming callback.
- Live PCM input (audio transcription with chunks arriving over time) is
  expressed by adding an `AudioItem` descriptor to the `Request` and
  pushing PCM bytes through a paired `ItemQueue`. The session consumes the
  queue while the streaming callback emits result items. This mirrors the
  C++ and Python implementations.

---

## API surface

### v2 (primary)

Mirrors the C# and Python v2 SDKs:

- `Manager`, `Catalog`, `Model`, `ModelInfo`
- `Session`, `ChatSession`, `AudioSession`, `EmbeddingsSession`
- `Request`, `Response`, `ItemQueue`
- `Item` discriminated union plus the `Item` factory namespace
  (`Item.text`, `Item.message`, `Item.imageFromUri`, `Item.imageFromData`,
  `Item.audioFromUri`, `Item.audioFromData`, `Item.audioDescriptor`,
  `Item.toolCall`, `Item.toolResult`, `Item.bytes`, `Item.tensor`)
- `FlErrorCode`, `FoundryLocalError`, `isFoundryLocalError`
- `ToolDefinition`, `FinishReason`, `TokenUsage`, `MessageRole`, etc.

### Legacy v1-compatible names (stubs only)

Re-exported from `src/index.ts` so consumers depending on the v1 class
names get a clear runtime error rather than an import failure. None of
these have implementations. If full behavioural parity is needed, build it
on the v2 layer:

- `FoundryLocalManager`, `ChatClient`, `ResponsesClient`, `AudioClient`,
  `EmbeddingClient`, `LiveAudioTranscriptionSession`, `ModelLoadManager`,
  `Configuration`, `getOutputText`

### Removed

- `CoreInterop` (was `@internal` in v1 — never part of the public contract).
- The `executeCommand` / `executeCommandAsync` / `executeCommandWithBinary` /
  `executeCommandStreaming` plumbing. Replaced by typed N-API entries per
  operation.

---

## Package & distribution

- Single npm package: `foundry-local-sdk@2.x`. Supersedes the legacy `1.x`
  line under the same name. Hard cut at the major version.
- Node 20+. ESM-only (no CommonJS dual build).
- Native addon: built with `node-gyp` against `binding.gyp`.
- **Prebuilds bundled in the published tarball.** CI builds the C++ SDK
  and the addon for every (platform × arch), drops each
  `(.node addon + foundry_local.{dll,so,dylib})` pair into
  `prebuilds/<platform>-<arch>/`, then `npm pack`s a single tarball
  containing all variants. At install time, `npm install` just unpacks the
  tarball — there is no postinstall download step, no separate artifact
  host, and no network access beyond the normal npm fetch. At runtime, the
  loader picks the matching `prebuilds/<process.platform>-<process.arch>/`
  subdirectory. If a consumer is on an unsupported platform, the addon
  load fails with a clear error; there is no automatic source-build
  fallback in the published package.
- **Dev / source builds load the native from the canonical C++ build
  dir.** Per
  [cpp-build.instructions.md](../../../.github/instructions/cpp-build.instructions.md),
  `python sdk_v2/cpp/build.py --configure --build --config RelWithDebInfo`
  is the contract; the addon is built locally and `script/copy-native.mjs`
  copies
  `sdk_v2/cpp/build/<Windows|Linux|macOS>/<Config>/bin/<Config>/[lib]foundry_local.{dll,so,dylib}`
  and its ORT/GenAI siblings into `sdk_v2/js/prebuilds/<platform>-<arch>/`
  next to the `.node` addon. At runtime the addon does a fixed sibling-file
  `LoadLibrary` / `dlopen` — no path discovery, no `DllLoader`-equivalent.
- **ONNX Runtime / ORT-GenAI discovery** follows the legacy contract:
  `foundry_local` depends on `onnxruntime` and `onnxruntime-genai`, and
  locating those at runtime is the user's responsibility (via the legacy
  `libraryPath` field, env vars, and the Linux `RTLD_DEEPBIND` pre-load
  shim in `addon.cc` for `libcrypto` / `libssl`). See
  [ort-loading-contract.instructions.md](../../../.github/instructions/ort-loading-contract.instructions.md).

---

## Repository layout

```
sdk_v2/js/
├── package.json
├── tsconfig.json
├── tsconfig.build.json
├── biome.json                  # lint + format (single tool)
├── vitest.config.ts            # test runner + coverage
├── binding.gyp                 # node-gyp build for the C++ addon
├── README.md                   # developer onboarding
├── docs/
│   └── PortJsToSdkV2.md        # this file
├── native/
│   └── src/                    # C++ addon (node-addon-api, C++20)
│       ├── addon.cc            # NAPI module init + RTLD_DEEPBIND shim
│       ├── addon_data.h        # per-instance class references
│       ├── catalog.{h,cc}      # Napi::ObjectWrap<Catalog>
│       ├── errors.{h,cc}       # foundry_local::Error → Napi::Error
│       ├── items.{h,cc}        # JS object ↔ foundry_local::Item conversion
│       ├── item_queue.{h,cc}   # Napi::ObjectWrap<ItemQueue>
│       ├── manager.{h,cc}      # Napi::ObjectWrap<Manager>
│       ├── model.{h,cc}        # Napi::ObjectWrap<Model>
│       ├── promise_worker.h    # AsyncWorker helpers with strong owner refs
│       ├── request.{h,cc}      # Napi::ObjectWrap<Request>
│       └── session.{h,cc}      # sessions + streaming TSFN bridge
├── src/
│   ├── index.ts                # public exports (v2 surface + legacy stubs)
│   ├── detail/                 # addon loader, error normalization, native types
│   ├── manager.ts
│   ├── catalog.ts
│   ├── model.ts
│   ├── request.ts
│   ├── response.ts
│   ├── session.ts              # Session, ChatSession, AudioSession, EmbeddingsSession
│   ├── items.ts                # Item union + factory namespace
│   └── item-queue.ts
├── test/                       # vitest (integration-heavy)
│   ├── _fixtures/              # cacheOnlyManager, realModelManager
│   ├── manager.test.ts
│   ├── manager-dispose.test.ts
│   ├── catalog.test.ts
│   ├── model.test.ts
│   ├── model-lifecycle.test.ts
│   ├── items.test.ts
│   ├── item-queue.test.ts
│   ├── chat-session.test.ts
│   ├── embeddings-session.test.ts
│   ├── audio-session.test.ts
│   ├── audio-session-streaming.test.ts
│   └── streaming.test.ts
└── script/
    ├── copy-native.mjs         # dev: copy foundry_local + ORT siblings into prebuilds/
    ├── pack-prebuilds.mjs      # CI: stage foundry_local only into prebuilds/ before npm publish
    └── gyp/                    # tiny node helpers invoked from binding.gyp
```

---

## Test strategy

Follows the **testing-trophy** model:

- **Foundation:** TypeScript strict mode + Biome catch most surface bugs
  without runtime cost.
- **Unit (thin):** Pure TS helpers covered with Vitest. No native calls.
- **Integration (heaviest layer):** Drive the real addon against a real
  Foundry Local native library. The integration tests split into two
  flavors:
  - **Cache-only tests** — use an in-memory fake catalog
    (`test/_fixtures/cacheOnlyManager.ts`) to exercise validation paths
    without loading any model. Always run.
  - **Real-model tests** — construct a real `Manager` against a model
    cache, load a model, and run inference. Gated on the
    `FOUNDRY_TEST_DATA_DIR` environment variable; reported as `skipped`
    (not `passed`) when unset, so the distinction is visible in the
    summary.

Test stack pinned:

- **Runner / assertions:** `vitest` + `@vitest/coverage-v8`
- **Lint + format:** `biome` (single tool — no ESLint, no Prettier)

---

## Current state

| Area                                                              | Status                                        |
|-------------------------------------------------------------------|-----------------------------------------------|
| C++ addon scaffolding + error mapping                             | Implemented                                   |
| `Manager`, `Catalog`, `Model`                                     | Implemented                                   |
| `Request`, `Response`, `ItemQueue`                                | Implemented                                   |
| `Item` discriminated union + factories                            | Implemented (all 8 subtypes, both directions) |
| `ChatSession` (non-streaming + streaming)                         | Implemented                                   |
| `EmbeddingsSession`                                               | Implemented                                   |
| `AudioSession` (URI, in-memory, streaming PCM)                    | Implemented                                   |
| `AbortSignal` cancellation                                        | Implemented                                   |
| Cross-SDK behavioural parity                                      | Verified against C++/C#/Python test fixtures  |
| `Model.download` progress callback                                | Not surfaced (add if needed)                  |
| `Model.removeFromCache` / `selectVariant` / `getInputOutputInfo`  | Not surfaced (add if needed)                  |
| Legacy v1 compatibility classes                                   | Stubs only — throw at construction            |
| CI prebuild packaging (`.pipelines/sdk_v2/`)                      | Not implemented                               |

Anything marked **not surfaced** is a deliberate scope decision, not an
oversight — the C++ wrapper exposes the underlying call but no JS consumer
scenario has needed it. Wire it up via the standard
`Napi::ObjectWrap<Model>` + `PromiseWorker` pattern in `native/src/model.cc`.

---

## Open follow-ups

- A sibling `@JsReviewer` agent mirroring `@Reviewer` for the JS / TS
  surface, if review volume justifies it.
- CI prebuild packaging job — needs platform-matrix decisions and an
  `.npmrc` story before it can land.
