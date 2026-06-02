---
description: Use when building or rebuilding the C++ SDK, picking a build output directory, or wiring native binaries into the C# / Python SDKs that load them by absolute path.
applyTo: sdk_v2/cpp/**
---

# C++ build hygiene — always use `build.py`, never bypass it

## Rule

**Always build the C++ SDK with `python build.py`.** Do not invoke `cmake --build` directly. Do not pass `--build_dir`. Either of those skips the platform segment in the output path and breaks downstream consumers that look for binaries at the canonical location.

| Command | Output path | Use? |
|---|---|---|
| `python build.py --build --config Debug` | `sdk_v2/cpp/build/Windows/Debug/bin/Debug/foundry_local.dll` | ✅ canonical |
| `python build.py --build_dir build --config Debug --build` | `sdk_v2/cpp/build/Debug/bin/Debug/foundry_local.dll` | ❌ wrong path |
| `cmake --build build/Debug --config Debug` | `sdk_v2/cpp/build/Debug/bin/Debug/foundry_local.dll` | ❌ wrong path |
| `cmake --build build/Windows/RelWithDebInfo --config RelWithDebInfo` | `sdk_v2/cpp/build/Windows/RelWithDebInfo/bin/RelWithDebInfo/foundry_local.dll` | ⚠️ correct path but bypasses configure & vcpkg sync — only OK for fast incremental rebuilds when `build.py --configure` has already run for that config |

## Why

`build.py` derives the build directory from the host platform: `build/<Windows|Linux|macOS|Android-<abi>>/<Config>`. The C# tests load the native via [foundry_local.native.cfg](../../sdk_v2/cs/test/FoundryLocal.Tests/bin/Debug/net9.0/win-x64/foundry_local.native.cfg) which hard-codes the platform-segmented path. Build outputs that land anywhere else are invisible to the C# test host and require manual `Copy-Item` to fix — which is fragile, easy to forget, and silently runs the C# tests against a stale ABI.

## Allowed exceptions

- **Fast incremental rebuild during inner-loop iteration**, after `build.py --configure --build` has already populated the canonical build dir for that config. Pass explicit phase flags to skip what you don't need:
  ```
  python build.py --build --config Debug                        # rebuild only, no reconfigure, no tests
  python build.py --build --test --config Debug                 # rebuild + run tests, no reconfigure
  cmake --build build/Windows/Debug --config Debug --target sdk_integration_tests --parallel 4
  ```
  `build.py --build` (optionally with `--test`) is the preferred form: it skips the expensive `--configure` step (much faster than the default `--configure --build --test`) while still using the canonical platform-segmented build dir. Always pass explicit phase flags when iterating — never re-run the bare `python build.py` (which reconfigures every time). Use the raw `cmake --build` form only when you want to build a single target. Never use a non-platform-segmented `-B` / build dir.
- **MSVC C1041 PDB-lock recovery**: retry the same `cmake --build` once with `--parallel 4`. Don't change the build dir.

## When verifying C# / Python tests against a fresh native

1. Build with `python build.py --build --config <Config>` so the binary lands at `build/Windows/<Config>/bin/<Config>/foundry_local.dll`.
2. C# tests will pick it up automatically via `foundry_local.native.cfg`. No copy needed.
3. Python tests need either:
   - `FOUNDRY_LOCAL_LIB_DIR` set to the canonical path, or
   - `pip install -e .` against the freshly-built SDK so the wheel-bundled `_native/<platform>/foundry_local.dll` is current.

If you find yourself running `Copy-Item foundry_local.dll`, stop — it means an earlier build went to the wrong directory. Rebuild with `build.py` (no `--build_dir`).
