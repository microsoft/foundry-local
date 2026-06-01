---
description: Use when implementing or modifying the v2 JavaScript/TypeScript SDK under sdk_v2/js/, including the Node-API C++ addon, TS layers, or package scaffolding.
applyTo: sdk_v2/js/**
---

# JS SDK v2 — Implementation Guide

The canonical plan is [sdk_v2/js/docs/PortJsToSdkV2.md](../../sdk_v2/js/docs/PortJsToSdkV2.md). Follow it.
Key invariants:

## Formatting

- **Line width is 120 characters** (set in `sdk_v2/js/biome.json`). Use the full width. Do **not** wrap
  short string literals, single-argument calls, or short `throw new X(...)` expressions onto multiple lines
  just because some other tool's default (Prettier 80, etc.) would. Single-line is preferred whenever the
  result fits in 120 columns.
- **120 chars applies to comments too** — JSDoc blocks, line comments, doc-block prose. Biome does **not**
  reformat comment text, so wrapping is on you. Do not wrap prose at ~80 chars out of habit. A single-line
  `/** ... */` is preferred when it fits; multi-line JSDoc lines should also fill toward 120 before wrapping.
  If a reviewer (or the user) flags a narrow-wrapped comment, fix every comment in the file you touched —
  do not just fix the one called out.

## Architecture

- **5 layers:** C ABI → C++ wrapper (`foundry_local_cpp.h`) → node-addon-api C++17 addon → TS detail layer →
  public TS surface (v2 + preserved legacy).
- **Addon talks to the C++ wrapper, not the C ABI directly.** If the wrapper is missing something you need,
  escalate to `@ApiExpert` — do not bypass.
- **Sync + async are separate native entries.** Sync calls the wrapper inline on the JS thread. Async wraps
  the same call in `Napi::AsyncWorker`. No JS-layer Promise wrapping of sync calls.
- **Streaming:** `Napi::ThreadSafeFunction` bridges the wrapper's `std::function` streaming callback to the
  JS event loop. Surface as `AsyncIterable`.

## Reference SDKs (the answer key)

The legacy JS `CoreInterop.executeCommand("X.Y", ...)` → C++ wrapper mapping has already been worked out by
the C# and Python v2 SDKs. For any legacy call site in `sdk/js/src/`, find the equivalent method on the
matching class in:

- `sdk_v2/cs/src/` (primary reference — same .NET-era command vocabulary)
- `sdk_v2/python/src/foundry_local_sdk/` (secondary reference)

Mirror that method's wrapper-call sequence in the addon / TS layer. Do not invent a new mapping.

## Package conventions

- Package name: `foundry-local-sdk` (v2.0.0). ESM only. Node 20+.
- `NAPI_VERSION=8`, `NAPI_DISABLE_CPP_EXCEPTIONS=0`.
- Build: `node-gyp` + `prebuildify` for prebuilt binaries published via `postinstall`.
- Tests: Vitest + `@vitest/coverage-v8`. Lint/format: Biome.
- Never hand-edit `prebuilds/`.

## C++ standard for the addon

- The addon TUs (`native/src/*.cc`) build at **C++20**. This is an internal build choice — the addon
  ships as a `.node` binary, no consumer compiles against its sources. C++20 also matches node-gyp's
  current MSBuild default, so explicitly requesting it avoids the D9025 "overriding /std" warning per TU.
- The **C++ wrapper header** (`foundry_local_cpp.h`) is the only API surface that must stay
  **C++17-consumable**, because external C++ consumers in any toolchain need to be able to
  `#include` it. Anything that goes into the wrapper header (or any header it transitively pulls in
  from `include/`) must be C++17. Implementation files and the JS addon are free to use C++20.
- The Windows binding uses `LanguageStandard: "stdcpp20"` rather than `/std:c++20` in
  `AdditionalOptions` so the canonical MSBuild property emits the flag exactly once. Stacking a second
  `/std:` via `AdditionalOptions` produces D9025 per TU.

## Public surface

- New v2 types live alongside preserved legacy classes (`ChatClient`, `LiveAudioTranscriptionSession`, etc.).
  Legacy classes are re-implemented on top of the v2 types — same shape, new internals.

## Native binary loading

- `foundry_local.{dll,so,dylib}` is **bundled** in the npm package next to the `.node` addon under
  `prebuilds/<platform>-<arch>/`. The addon loads it via a fixed sibling-file `dlopen`/`LoadLibrary` — no
  path discovery. In dev / source builds, the file is copied out of `sdk_v2/cpp/build/<Platform>/<Config>/`
  into the same `prebuilds/` layout.
- The C# `Detail/DllLoader.cs` is **not** a reference for this — it solves a NuGet runtime-assets layout
  that does not apply to npm.
- ONNX Runtime / ORT-GenAI discovery is **inherited from the legacy JS SDK** (`Configuration.libraryPath`,
  env vars, the Linux `RTLD_DEEPBIND` pre-load of `libcrypto`/`libssl` from `foundry_local_napi.c`). Same
  contract, ported into the new addon's module-init code. See
  [ort-loading-contract.instructions.md](ort-loading-contract.instructions.md).

## Session class hierarchy

- **`Session` is an `abstract class`. It has no public constructor and no native JS-constructor export.**
  Users must construct a modality-specific subclass (`ChatSession`, `AudioSession`, `EmbeddingsSession`).
  The base carries the shared inference plumbing (`processRequest`, `processStreamingRequest`, `setOptions`,
  `dispose`, `disposed`, `[Symbol.dispose]`); subclasses add only modality-specific methods. Method names
  mirror the C++/C#/Python SDKs (`ProcessRequest` / `ProcessRequestAsync` / `process_request`) for
  cross-language consistency — do not rename to `send`/`stream` even though they would be more idiomatic
  to JS.
- **`Model` is an input to a Session, not a factory for it.** Do not add `model.createXSession()` methods.
  The C++ wrapper API does not have them either — `new ChatSession(model)` mirrors
  `foundry_local::ChatSession sess(model);`. Factory methods on `Model` create unnecessary coupling and
  force `Model` to grow with every new session type. They were removed deliberately.
- **Native side: only modality-specific session ObjectWraps get a JS-callable constructor.** The base
  `Session` C++ ObjectWrap was deleted entirely — `ChatSession` (and future Audio/Embeddings) ObjectWraps
  each independently hold their own `std::unique_ptr<foundry_local::XSession>`. Do not re-introduce a base
  ObjectWrap unless multiple derived classes need to share C++ implementation.
- **Subclass constructors do an `instanceof Model` guard** on their argument and throw `TypeError` with a
  "expected a Model" message on bad input. The native ctor receives the unwrapped `NativeModel` and
  constructs the underlying `foundry_local::XSession` synchronously.
- **Manager-pin lifetime** is propagated from the JS `Model` (via `Model::manager()` accessor) into the
  session ObjectWrap during the native ctor, so disposing the originating `Manager` does not invalidate
  active sessions.
