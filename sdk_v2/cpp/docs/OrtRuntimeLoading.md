# ORT Runtime Library Loading Strategy

## Problem

`foundry_local` (the native shared library) has load-time dependencies on
`onnxruntime` and `onnxruntime-genai`. The OS dynamic loader resolves these
*before any code in `foundry_local` executes*, so the native library cannot
preload its own dependencies — by the time a hypothetical `foundry_local_init()`
could run, the load has already either succeeded or failed.

Two failure modes follow from this:

1. **Linux/macOS:** the dynamic linker walks `RPATH`/`RUNPATH`/`LD_LIBRARY_PATH`/
   default search paths to satisfy the `DT_NEEDED libonnxruntime.so.1` entry. If
   the SDK directory isn't on any of those paths (e.g., the consumer loaded
   `libfoundry_local.so` by absolute path from a NuGet `runtimes/<rid>/native/`
   layout), the load fails with
   `libonnxruntime.so.1: cannot open shared object file: No such file or directory`.
2. **Windows:** the loader walks the standard DLL search order. If a system or
   sibling-application `onnxruntime.dll` is on `PATH`, it can win over the
   co-located copy and cause API-version mismatches at runtime.

## Approach: Bindings Preload ORT Before Loading `foundry_local`

Every language binding (C#, Python, JS, Rust, …) is responsible for loading
`onnxruntime` then `onnxruntime-genai` *by absolute path* before loading
`foundry_local`. The native library has zero ORT-loading machinery — no
delay-load, no configuration knob, no eager-load. ORT and GenAI are ordinary
load-time link dependencies and the bindings make sure they're already resident
when `foundry_local` hits the loader.

This works because the OS loader maintains a process-wide loaded-module table
keyed by SONAME (POSIX) or base name (Windows). Once a module is in that table,
subsequent `DT_NEEDED` / import-table entries referring to the same name resolve
to the resident instance — no filesystem search, no `RPATH`/`PATH` involvement.

### Load Order Contract

Every binding implements this sequence:

```
1. Resolve the directory containing the native bits
   (BaseDirectory, NuGet runtimes/<rid>/native/, wheel _native/, etc.)
2. dlopen / LoadLibrary onnxruntime by absolute path        ← resident as SONAME / base name
3. dlopen / LoadLibrary onnxruntime-genai by absolute path  ← its NEEDED ORT resolves to (2)
4. dlopen / LoadLibrary foundry_local by absolute path      ← its NEEDED ORT resolves to (2)
5. FoundryLocalGetApi(...) etc.
```

Order is mandatory: GenAI's `DT_NEEDED` references ORT, so ORT must be loaded
first. Steps 2–3 are best-effort: if the files aren't present (e.g., a stripped
deployment that ships ORT via a separate package), the binding logs and
continues; step 4 will then surface a clearer load error.

## GenAI's Own ORT Resolution (`ORT_LIB_PATH`)

The preload contract above only governs `foundry_local`'s **load-time**
`DT_NEEDED`. On Linux and macOS, `onnxruntime-genai` does its *own* `dlopen` of
ORT at `InitApi()` time (lazy — fired by the first model load), independent of
that `DT_NEEDED`. Its precedence (`src/models/onnxruntime_api.h`) is:

```
1. $ORT_LIB_PATH            → dlopen(<abs path>)   ← the only step that pins an exact file
2. libonnxruntime.so        → plain dlopen (Linux)
3. libonnxruntime.so.1      → plain dlopen (Linux)
4. libonnxruntime.dylib     → plain dlopen (macOS)
```

Steps 2–4 use a **plain `dlopen`, not `RTLD_NOLOAD`**, so a leafname that does
not match the resident soname triggers a fresh filesystem search that can load a
*second* ORT image with a separate `OrtEnv`. When that happens, the execution
providers `foundry_local` registered on its `OrtEnv` are invisible to GenAI's
inference (see `Manager::Create()` — it assumes `CreateEnv` returns the same
singleton GenAI uses).

**Contract:** every binding sets `ORT_LIB_PATH` to the *absolute path of the ORT
file it preloaded*, on POSIX, before loading `foundry_local`. This makes GenAI
`dlopen` that exact file (which resolves to the already-resident instance),
sidestepping the versioned/unversioned soname mismatch entirely. Rules:

- **POSIX-only.** On Windows GenAI links ORT directly and ignores the variable.
- **Point at the exact file, not a directory.**
- **Never clobber a caller-provided `ORT_LIB_PATH`** — a host may be pinning its
  own ORT. Note the variable is process-global and inherited by child processes.

> onnxruntime-genai
> [#2372](https://github.com/microsoft/onnxruntime-genai/pull/2372) adds an
> `RTLD_NOLOAD` probe (both versioned and unversioned names) *before* the plain
> `dlopen` fallback, so GenAI reuses a resident ORT even when `ORT_LIB_PATH` is
> unset. It complements this contract — it's the safety net for third-party
> hosts that load `foundry_local` without setting `ORT_LIB_PATH`. `ORT_LIB_PATH`
> stays the deterministic path for our own bindings and works against current
> GenAI without a version bump.

## Why Not a Native-Side Loader?

Earlier iterations tried two native-side designs:

- **`/DELAYLOAD` + `Configuration::SetRuntimeLibraryPath` + `EagerLoadOrtDlls()`.**
  Worked on Windows because MSVC's delay-load rewrites the import table into
  thunks that resolve on first call, giving the SDK a window to load ORT itself
  during `Manager::Create()`. Has no portable equivalent on POSIX —
  `DT_NEEDED` is always eager. The Linux/macOS half of the design was never
  implemented, leaving bindings on those platforms broken.
- **`$ORIGIN` / `@loader_path` RPATH + co-location.** Works for in-tree builds
  where the consumer binary sits next to `libfoundry_local.so`. Fails for
  layouts where the SDK is loaded by absolute path from a directory that isn't
  on any default search path (NuGet `runtimes/`), and is fragile when the
  shipped ORT filename doesn't match the SONAME the loader is looking for
  (`libonnxruntime.so` vs. `libonnxruntime.so.1`).

The binding-preload contract works identically on Windows, Linux, and macOS,
needs no native machinery, and survives any deployment layout that the binding
can already locate `foundry_local` in.

## Co-location Stays as the Build-Tree Default

The build system still copies `onnxruntime` and `onnxruntime-genai` next to
`foundry_local` via post-build commands in `CMakeLists.txt`. This keeps unit
tests, integration tests, and example binaries zero-configuration: the test
process loads them via the OS default search (the binary's own directory)
without needing any explicit preload. Bindings only have to do the preload
dance when they're loading `foundry_local` from somewhere other than the
process's default search locations.

## Implementations

| Binding | File | Function |
|---------|------|----------|
| C# | `sdk_v2/cs/src/Detail/DllLoader.cs` | `PreloadOrtIfPresent(directory)` is called from each `ResolveDll` branch (redirect file, BaseDirectory, NuGet `runtimes/<rid>/native/`) right before `NativeLibrary.TryLoad(libfoundry_local)`. Idempotent across branches via static handle fields. **`ORT_LIB_PATH`: not yet set** — see [OrtLibPathPlan.md](OrtLibPathPlan.md). |
| Python | `sdk_v2/python/src/foundry_local_sdk/_native/lib_loader.py` | `prepare_native_dependencies()` performs explicit `ctypes.CDLL` (`RTLD_GLOBAL`) preload of ORT then GenAI from the sibling `onnxruntime` / `onnxruntime-genai-core` PyPI packages. Handles kept alive in `api.py`. **macOS today uses a `libonnxruntime.dylib` symlink** into the GenAI package dir to satisfy GenAI's leafname `dlopen`; to be replaced by `ORT_LIB_PATH` — see [OrtLibPathPlan.md](OrtLibPathPlan.md). |
| JS | `sdk_v2/js/src/detail/native.ts` | `preloadOrtIfPresent()` preloads ORT then GenAI via the standalone `foundry_local_preload.node` addon (`dlopen`/`LoadLibraryExW`; Node 23+ rejects `process.dlopen` for non-addon libs). Ships the versioned soname and **sets `ORT_LIB_PATH`** to it (`applyOrtLibPath`) — the reference implementation of the GenAI-resolution contract. |
| Rust | (TBD when the v2 SDK comes online) | Must implement the same preload sequence + `ORT_LIB_PATH`. |

## History

- **Initial (C++ port):** load-time linking + co-location. Works in-tree but
  not for non-co-located deployments.
- **First C# SDK v2 fix:** `DllLoader` preloaded ORT from a known path before
  loading `foundry_local`. Worked but was C#-specific.
- **`/DELAYLOAD` + `RuntimeLibraryPath` + `EagerLoadOrtDlls`:** native-side
  loader on Windows; intended for parity with POSIX but the POSIX half was
  never implemented. Removed when the binding-preload contract was adopted as
  the cross-platform answer.
- **Current:** binding-owned preload on all platforms. Native lib has no
  ORT-loading machinery. C#, Python, and JS bindings preload before every
  `foundry_local` load attempt.
- **GenAI resolution (`ORT_LIB_PATH`):** JS pins GenAI's independent ORT
  `dlopen` via `ORT_LIB_PATH`. C# (leafname luck) and Python (macOS symlink) are
  being converged onto the same env-var contract — see
  [OrtLibPathPlan.md](OrtLibPathPlan.md). Complements onnxruntime-genai #2372.
