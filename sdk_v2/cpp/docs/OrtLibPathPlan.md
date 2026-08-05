# Plan: Set `ORT_LIB_PATH` in every binding (converge GenAI's ORT resolution)

Status: **proposed** — for review. Small, one focused change per binding.

## Goal

Make ONNX Runtime GenAI reuse the *exact* `onnxruntime` shared library each
binding already preloaded, on Linux and macOS, using GenAI's own first-choice
mechanism: the `ORT_LIB_PATH` environment variable. Converge C#, Python, and JS
on one contract and remove the per-binding workarounds.

## Why

The binding-preload contract makes `foundry_local`'s load-time `DT_NEEDED`
resolve to our ORT. But GenAI does its **own** `dlopen` of ORT at `InitApi()`
time (lazy, on first model load), independent of that `DT_NEEDED`. Current
precedence in GenAI `src/models/onnxruntime_api.h` (Linux/macOS):

1. `ORT_LIB_PATH`            → `dlopen(<abs path>)`   ← only step that pins an exact file
2. `libonnxruntime.so`       → plain `dlopen` (Linux)
3. `libonnxruntime.so.1`     → plain `dlopen` (Linux)
4. `libonnxruntime.dylib`    → plain `dlopen` (macOS)

Steps 2–4 are plain `dlopen` (not `RTLD_NOLOAD`), so a leafname that doesn't
match the resident soname triggers a fresh filesystem search that can load a
**second** ORT image with a separate `OrtEnv` — after which host-registered
execution providers are invisible to GenAI.

This bites each binding differently today:

| Binding | GenAI reuses our ORT via | Problem |
|---|---|---|
| JS  | `ORT_LIB_PATH` (already set) | none |
| Python | macOS `libonnxruntime.dylib` symlink into the GenAI package dir | filesystem mutation; fails on read-only installs |
| C# | nothing — bare leafname luck | no guard against a stray `libonnxruntime.so` winning step 2 |

## Changes

### C# — `sdk_v2/cs/src/Detail/DllLoader.cs`
- In `PreloadOrtIfPresent`, after ORT is preloaded, set `ORT_LIB_PATH` to the
  **absolute path of the ORT file actually loaded** — only when unset.
- POSIX-only (Windows GenAI links ORT directly; the env var is ignored there).

### Python — `sdk_v2/python/src/foundry_local_sdk/_native/lib_loader.py`
- In `prepare_native_dependencies`, set `ORT_LIB_PATH` to `str(ort_path)` on
  POSIX when unset.
- **Remove** the macOS `libonnxruntime.dylib` symlink block — `ORT_LIB_PATH`
  replaces it and mutates no filesystem.

### JS — no change
- `sdk_v2/js/src/detail/native.ts` already sets `ORT_LIB_PATH` (`applyOrtLibPath`)
  to the versioned soname. It becomes the reference implementation.

### Docs
- `OrtRuntimeLoading.md`: document GenAI's independent resolution + `ORT_LIB_PATH`
  as the shared contract; refresh the implementations table (JS is no longer TBD).

## Rules (all bindings)

1. **POSIX-only** — no-op on Windows.
2. **Point at the exact absolute path of the ORT file preloaded**, not a
   directory. Because GenAI `dlopen`s it by absolute path, this sidesteps the
   versioned/unversioned soname question entirely.
3. **Never clobber a caller-provided `ORT_LIB_PATH`** — a host may be pinning
   its own ORT.
4. **Set before loading `foundry_local`** (before GenAI's lazy `InitApi`), early
   in startup to avoid the glibc `setenv`/`getenv` race.

## Relationship to onnxruntime-genai #2372

Complementary, not redundant:
- `ORT_LIB_PATH` — *our* bindings deterministically pinning the file. Works
  against **current** GenAI; checked first, short-circuits the leafname probes.
- #2372 (`RTLD_NOLOAD` probe of both names) — safety net for third-party hosts
  that load `foundry_local` without setting `ORT_LIB_PATH`.

Do both: ship `ORT_LIB_PATH` now; adopt #2372 when it lands so non-cooperating
hosts converge too.

## Out of scope
- No native-side ORT loading (still rejected — see `OrtRuntimeLoading.md`).
- No GenAI version bump required for the `ORT_LIB_PATH` change.

## Validation
- Linux/macOS: run C# + Python integration tests that load a model; confirm one
  ORT image (GenAI logs the loaded path with `ORTGENAI_LOG_ORT_LIB=1`).
- Confirm EP registration is visible to GenAI inference (a GPU/NPU model loads
  on the registered EP, not silently on CPU).
- macOS Python: confirm model load succeeds with the symlink code removed.
