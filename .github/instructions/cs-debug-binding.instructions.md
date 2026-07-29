---
description: Use when changing the C ABI (foundry_local_c.h) and validating C# tests, or when C# tests fail with stale-ABI symptoms (struct/layout mismatches, dispatch falling through unexpectedly).
applyTo: sdk_v2/cs/**
---

# C# Tests Auto-Load the C++ **RelWithDebInfo** Build

The C# project (`sdk_v2/cs/src/Microsoft.AI.Foundry.Local.csproj`) auto-detects
`sdk_v2/cpp/build/Windows/RelWithDebInfo/bin/RelWithDebInfo/foundry_local.dll`
for inner-loop dev and writes a `foundry_local.native.cfg` redirect file. The
test project copies this redirect to its output, so `dotnet test` always loads
the **RelWithDebInfo** native binary. The auto-detect block only checks the
RelWithDebInfo config (one entry per OS) — it never probes a Debug build.

To override (e.g. point at a Debug build or a NuGet runtime package), set
`FoundryLocalNativeBinDir` or `FoundryLocalRuntimeVersion` explicitly; otherwise
the RelWithDebInfo auto-detect wins.

## Implication

When you change the C ABI surface (e.g. `foundry_local_c.h`, struct layouts,
function signatures in `c_api.cc`), you **must rebuild RelWithDebInfo** before
running C# tests:

```powershell
cd sdk_v2/cpp
python build.py --build --config RelWithDebInfo
```

This is the same config the C++ integration test suite uses, so a single
RelWithDebInfo build serves both.

## Symptoms of a stale RelWithDebInfo DLL

- C# tests fail with native errors that don't match current C++ source line numbers
- Dispatch falls through unexpectedly (e.g. JSON pass-through stops working
  because the old DLL still expects the old item type tag)
- Marshalling appears correct in C# but the native side reads garbage
