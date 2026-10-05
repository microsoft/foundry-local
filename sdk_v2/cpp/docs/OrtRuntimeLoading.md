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

### Linux and Android Library Lifetime

`libfoundry_local.so` exports only the two public entry points through an ELF
version script, keeping bundled static dependencies private. It is also linked
with `-z nodelete` so `dlclose` cannot unmap it before process exit. Bundled
OpenSSL registers pthread-local cleanup callbacks on caller-owned threads
(including Rust's Tokio blocking workers); those callbacks can run after the
last SDK handle is released.

This retains the library mapping and its static state, not the manager:
`Manager_Shutdown` and `Manager_Release` still run normally, and a new manager
can be created after all previous handles are released. Bindings must release
managers and derived handles normally rather than relying on library unloading
for cleanup.

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
| C# | `sdk_v2/cs/src/Detail/DllLoader.cs` | `PreloadOrtIfPresent(directory)` is called from each branch of `ResolveDll` (BaseDirectory, NuGet `runtimes/<rid>/native/`) right before `NativeLibrary.TryLoad(libfoundry_local)`. Idempotent across branches via static handle fields. |
| Python | `sdk_v2/python/src/foundry_local_sdk/_native/lib_loader.py` | `prepare_native_dependencies()` performs explicit `ctypes.CDLL` preload of ORT then GenAI. Handles are stored in a module-level list in `api.py` to prevent garbage collection. |
| JS / Rust | (TBD when those v2 SDKs come online) | Must implement the same preload sequence. |

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
  ORT-loading machinery. C# and Python bindings preload before every
  `foundry_local` load attempt.
