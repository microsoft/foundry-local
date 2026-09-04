# sdk_v2 pipeline reference

Reference for the CI/CD pipeline that builds, tests, and packs the
`sdk_v2/` C++ runtime, C# SDK, Python SDK, and Rust SDK. Documents the
as-built shape of the pipeline, the architectural decisions behind it, and the
operational details (artifact names, version contracts, pinned dependency
versions) needed to maintain it.

## Scope

In scope: `sdk_v2/cpp` (native runtime), `sdk_v2/cs/src` (C# SDK),
`sdk_v2/python` (Python SDK), `sdk_v2/rust` (Rust SDK). The C++ SDK is the
native runtime dependency for C# (via NuGet), Python (bundled in the wheel),
and Rust (assembled by the crate build script), replacing the legacy
`foundry-local-core` from neutron-server.

JS is documented separately in `sdk_v2-js-pipeline-plan.md`. A standalone
`foundry-local-runtime` Python wheel remains out of scope.

## Top-level pipelines

There is **no dedicated sdk_v2 top-level pipeline**. The sdk_v2 stage graph
is emitted by a single coordinator template,
`.pipelines/v2/templates/stages-sdk-v2.yml`, which is included by the
existing `.pipelines/foundry-local-packaging.yml`. sdk_v2 is built
unconditionally on every run (there is no v2 gate); the v1 legacy stages
are gated separately via `.pipelines/v1/templates/stages-sdk-v1.yml`.

## Supported platforms

| Platform        | Pool                              | Build | Test | Notes                                   |
|-----------------|-----------------------------------|-------|------|-----------------------------------------|
| Windows x64     | `onnxruntime-Win-CPU-2022`        | ✅    | ✅   | Also stages public headers              |
| Windows ARM64   | `onnxruntime-Win-CPU-2022`        | ✅    | ❌   | Cross-compiled from x64 host            |
| Linux x64       | `onnxruntime-Ubuntu2404-AMD-CPU`  | ✅    | ✅   | manylinux 2.28 container, CPU-only      |
| Linux ARM64     | `onnxruntime-linux-ARM64-CPU-2019`| ✅    | ✅   | manylinux 2.28 container, CPU-only      |
| macOS ARM64     | `AcesShared` (Sequoia)            | ✅    | ✅   | Native                                  |

## Architectural decisions (locked)

1. **C# Runtime version pinning.** SDK csproj is built with
   `/p:FoundryLocalRuntimeVersion=$(sdkVersion)` where `sdkVersion` is the
   same string used to pack the Runtime nupkg. SDK and Runtime always ship
   as a matched pair.
2. **Single C# package bundles WinML.** The SDK csproj references one
   `Microsoft.AI.Foundry.Local.Runtime` package, which carries the reg-free
   WinML 2.x runtime on Windows. There is no separate WinML SKU or
   `UseWinML` switch.
3. **Python SDK wheel bundles `foundry_local` directly** at
   `_native/<rid>/foundry_local.{ext}`. No separate `foundry-local-runtime`
   wheel. Trade-off accepted: SDK and runtime upgrade together. Non-SDK
   consumers get native libs from the NuGet.

   ORT and GenAI native libraries are **not** bundled in the wheel; they are
   resolved at install time from public PyPI (see decision 9). The wheel
   only contains `foundry_local` plus its non-ORT runtime closure.
4. **Test stages set `FOUNDRY_TEST_DATA_DIR`** to the checked-out
   `test-data-shared` path for all three languages. C++ stages set it
   directly; Python's `conftest.py` reads it; C# `Utils.cs` prefers it
   over `appsettings.Test.json`.
5. **Parameter naming.** sdk_v2 templates use `flNugetDir` (not `flcNugetDir`).
   `flc` was Foundry Local Core, which we are replacing.
6. **Shared `compute_version` stage.** Defined inline in
   `.pipelines/foundry-local-packaging.yml`. It is emitted once at the top
   level and writes a `version-info` pipeline artifact that contains
   per-track files (`sdkVersion.txt`, `pyVersion.txt`, plus `*.v1.txt`
   counterparts). Both the sdk_v2 and sdk_v1 coordinators read from this
   artifact rather than recomputing a timestamp.
7. **Single Python wheel name; custom PEP 517 backend rewrites ORT pins.**
   One wheel, `foundry-local-sdk`, bundles the WinML runtime on Windows. The
   build backend at `sdk_v2/python/_build_backend/__init__.py` wraps
   `setuptools.build_meta` solely to rewrite the ORT/GenAI pins (decision 8);
   it no longer rewrites the project name.
8. **Single source of truth for ORT/GenAI versions.** ORT and GenAI versions
   live in `sdk_v2/deps_versions.json`. The file shape is
   `{ "onnxruntime": { "version": "..." }, "onnxruntime-genai": { "version": "..." } }`.
   Consumers:
   - **C++ build:** `sdk_v2/cpp/cmake/Find{OnnxRuntime,OnnxRuntimeGenAI}.cmake`
     read versions via `file(READ)` + `string(JSON ... GET ...)`. The
     `if(NOT ORT_VERSION)` guard is preserved so `-DORT_VERSION=...`
     overrides still work.
   - **Python wheel build:** `pyproject.toml` declares ORT pins with
     sentinel `==0.0.0`; the build backend rewrites the pins from JSON
     at wheel-build time. If the backend is ever bypassed, `pip install`
     fails fast with "no matching version" (intentional loud failure).

   Bumping ORT/GenAI is a one-file edit.
9. **ORT/GenAI come from public PyPI.** No private feed plumbing required
   for the wheel install path. Every platform uses `onnxruntime` and
   `onnxruntime-genai-core` (`onnxruntime` and `onnxruntime_genai_core` imports).
10. **C++ staging step is the policy authority for native payload contents.**
    `steps-build-{windows,linux,macos}.yml` stage the **full runtime closure**
    of `foundry_local` into the `cpp-native-<rid>` artifact, with explicit
   inclusions/exclusions. The Python wheel build copies the artifact contents
   needed by its wheel, while C# packing applies its own explicit runtime
   payload allow-list in `sdk_v2/cpp/nuget/pack.py`. That allow-list includes
   the Windows import library (`foundry_local.lib`) for applications that
   link against `foundry_local.dll`.

    Each staging step copies an explicit allow-list, not a glob: just the
    redistributable `foundry_local` library (`.dll` + `.pdb` + `.lib` on
    Windows, `libfoundry_local.so` / `.dylib` elsewhere). vcpkg statically
    links the rest of the closure (ORT/GenAI/azure-*/curl/ssl/zlib/spdlog/fmt)
    into `foundry_local` itself, so nothing else needs to travel. ORT/GenAI
    are resolved separately at runtime — from pip on the Python side and from
    NuGet on the C# side.

    On Windows the allow-list also includes the delay-loaded
    `Microsoft.Windows.AI.MachineLearning.dll` (the reg-free WinML 2.x
    runtime, ~922 KB, Microsoft-signed). It is the single payload difference
    that used to distinguish the WinML SKU; bundling it unconditionally is
    what lets one package serve every consumer.

    Each step fails loudly if its primary library is missing.
11. **Python runtime ORT discovery.** `lib_loader.py::prepare_native_dependencies()`
    preloads ORT and GenAI by absolute path. Windows also registers their DLL
    directories; macOS creates the unversioned ORT symlink required by GenAI
    ([onnxruntime#27263](https://github.com/microsoft/onnxruntime/issues/27263)).

    Wired into `_native/api.py` between `find_library()` and the cffi
    extension import. Idempotent and silent on failure.
12. **`foundry-local-install` CLI.** Declared via `[project.scripts]` in
    `pyproject.toml`, backed by
    `sdk_v2/python/src/foundry_local_sdk/_native/installer.py`. Flags:
    `--verbose`. Verifies installed packages via
    `importlib.util.find_spec` to avoid triggering DLL load during
    verification.

## Template layout

```
.pipelines/v2/
└── templates/
    ├── stages-sdk-v2.yml             # Coordinator: native + C# + Python + JS + Rust
    ├── stages-build-native.yml       # 5 native build stages + C++/NuGet pack stages
    ├── stages-cs.yml                 # C# build + test
    ├── stages-python.yml             # Python build + test
    ├── stages-js.yml                 # JS build + test + combined npm pack
    ├── stages-rust.yml               # Rust build+test (5 platforms) + crate pack
    ├── steps-prefetch-nuget.yml      # ORT/GenAI/WinML NuGet pre-fetch (pwsh + bash)
    ├── steps-build-windows.yml       # arch: x64 | arm64 (always bundles WinML)
    ├── steps-build-linux.yml
    ├── steps-build-macos.yml
    ├── steps-build-cs.yml            # restore + build + ESRP-sign + pack + ESRP-sign nupkg
    ├── steps-test-cs.yml             # restore + build + run tests
    ├── steps-build-python.yml        # pass-through copy + python -m build --wheel
    ├── steps-test-python.yml         # install wheel + pytest
    ├── steps-build-rust.yml          # toolchain + native assembly + fmt/clippy/build/test
    └── steps-pack-nuget.yml          # Runs sdk_v2/cpp/nuget/pack.py
```

The repo-shared `.pipelines/templates/checkout-steps.yml` is reused for
`test-data-shared` (LFS) checkout via the `FoundryLocalCore-SP` Azure CLI
service connection.

## Stage dependency graph

```
compute_version
   |
   |-- cpp_build_win_x64 ----------+
   |-- cpp_build_win_arm64 --------|
   |-- cpp_build_linux_x64 --------|
   |-- cpp_build_linux_arm64 ------+--> pack_nuget --> cs_build --+--> cs_test_win_x64
   |-- cpp_build_osx_arm64 --------+                              |--> cs_test_linux_x64
   |                                                              |--> cs_test_linux_arm64
   |                                                              +--> cs_test_osx_arm64
   |
   |-- python_build_win_x64 ------> python_test_win_x64
   |-- python_build_win_arm64 ----> (build-only)
   |-- python_build_linux_x64 ----> python_test_linux_x64
   |-- python_build_linux_arm64 --> python_test_linux_arm64
   +-- python_build_osx_arm64 ----> python_test_osx_arm64
   |
   |-- rust_build_win_x64 ---------+  (build + unit + integration)
   |-- rust_build_win_arm64 -------|  (cross build-only)
   |-- rust_build_linux_x64 -------+--> rust_pack_crate
   |-- rust_build_linux_arm64 -----|
   +-- rust_build_osx_arm64 -------+
```

* All build stages are independent (`dependsOn: [compute_version]`) and run
  in parallel.
* The pack stage runs on every build (PR and `main`).
* The Windows native build always bundles the reg-free WinML 2.x runtime,
  which links against the same `ortVersion` as every other platform — there
  is no separate WinML-aligned ORT pin or build flavor.
* Tests run on `cpp_build_win_x64`, both Linux native stages, and
  `cpp_build_osx_arm64`. The ARM64 Windows stage cross-compiles from an x64
  host and skips tests.

## Per-stage artifacts

Published via 1ES `templateContext.outputs` (no manual `PublishPipelineArtifact`).

| Stage                       | Artifact name                  | Contents                                                   |
|-----------------------------|--------------------------------|------------------------------------------------------------|
| `compute_version`           | `version-info`                 | `sdkVersion.txt`, `pyVersion.txt`, `flcVersion.txt`        |
| `cpp_build_win_x64`         | `cpp-native-win-x64`           | `foundry_local.dll`, `.pdb`, `.lib` + `Microsoft.Windows.AI.MachineLearning.dll` |
| `cpp_build_win_x64`         | `cpp-native-include`           | Public headers (sourced once, from win-x64)                |
| `cpp_build_win_arm64`       | `cpp-native-win-arm64`         | `foundry_local.dll`, `.pdb`, `.lib` + `Microsoft.Windows.AI.MachineLearning.dll` |
| `cpp_build_linux_x64`       | `cpp-native-linux-x64`         | `libfoundry_local.so`                                      |
| `cpp_build_linux_arm64`     | `cpp-native-linux-arm64`       | `libfoundry_local.so` (CPU-only)                           |
| `cpp_build_osx_arm64`       | `cpp-native-osx-arm64`         | Developer ID-signed `libfoundry_local.dylib`               |
| `cpp_pack_nuget`            | `cpp-nuget`                    | `Microsoft.AI.Foundry.Local.Runtime.<version>.nupkg` (bundles WinML on Windows) |
| `rust_pack_crate`           | `rust-sdk-v2`                  | `foundry-local-sdk-<version>.crate` (platform-independent source crate) |

## Rust SDK

The Rust SDK (`foundry-local` crate) is a **source package**, unlike the
per-platform wheel / npm tarball / NuGet. There is no per-platform binary to
hand off, so:

* Each platform stage (`rust_build_<rid>`) rebuilds from source, running
  `cargo fmt --check`, `cargo clippy -D warnings`, `cargo build`, then the unit
  (`cargo test --lib`) and integration (`cargo test --tests`) suites. A single,
  platform-independent `.crate` is produced once by `rust_pack_crate`.
* **Native for tests.** The `cpp-native-<rid>` artifact carries only
  `foundry_local` (plus the WinML DLL on Windows). ORT/GenAI are *not*
  vcpkg-static (only azure/spdlog/fmt/openssl/curl/zlib/brotli are absorbed into
  `foundry_local`), so `steps-build-rust.yml` extracts `onnxruntime` +
  `onnxruntime-genai` from the pinned `Microsoft.ML.OnnxRuntime.Foundry` /
  `Microsoft.ML.OnnxRuntimeGenAI.Foundry` NuGet packages (`runtimes/<rid>/native/`)
  into one directory alongside `foundry_local`, handed to `build.rs` via
  `FOUNDRY_LOCAL_NATIVE_BIN_DIR`. build.rs copies every platform library into
  `OUT_DIR` and bakes that path, so unit and integration tests resolve the native
  by the baked path. The pinned versions match the C++ native build inputs
  (`cppOrtVersion` / `cppGenaiVersion`).
* **win-arm64** is cross-compiled (`cargo clippy --target aarch64-pc-windows-msvc`,
  check-only), matching the build-only treatment it gets in the C# / Python
  matrices.
* Integration tests run with `--test-threads=1`: the native core enforces a
  single `FoundryLocalManager` per process, so tests that each create one must
  not run concurrently. They consume the same `test-data-shared` models as the
  C# / Python suites (`FOUNDRY_TEST_DATA_DIR`).

## Versioning

The `compute_version` stage emits three flavors of the same base version:

* `sdkVersion` — semver for NuGet/JS/C#/Rust  (e.g. `0.1.0-dev.202605111234`)
* `pyVersion`  — PEP 440 for Python           (e.g. `0.1.0.dev202605111234`)
* `flcVersion` — Core/native-style            (e.g. `0.1.0-dev-202605111234-abc12345`)

The C++ pack stage and the Rust build/pack stages consume `sdkVersion.txt`;
the Python build stage consumes `pyVersion.txt`; `flcVersion.txt` exists for
Core publishing.

`sdkVersion` is baked into the native binary via the cmake cache variable
`FOUNDRY_LOCAL_VERSION_STRING`, so `FoundryLocalGetVersionString()` returns
the same string that appears in the `.nupkg` filename. Each platform build
stage:

1. Depends on `compute_version` and downloads the `version-info` artifact.
2. Reads `sdkVersion.txt` and sets `FOUNDRY_LOCAL_VERSION_STRING`.
3. Passes the combined defines to `build.py --cmake_extra_defines`.

Local developer builds (no `-D` override) use the `PROJECT_VERSION` from
`sdk_v2/cpp/CMakeLists.txt` as the fallback.

Pipeline parameters allow override:

* `version`        — base version, default `0.1.0`
* `prereleaseId`   — `none` (default) or any string (`rc1`, `beta`, …)
* `isRelease`      — boolean, drops the dev/timestamp suffix
* `buildConfig`    — CMake config, default `RelWithDebInfo`

## Dependency versions

The pipeline pins ORT / GenAI / WinML versions and downloads the NuGet
packages from **public nuget.org** (`https://www.nuget.org/api/v2/package`)
before invoking CMake. This matches Foundry Local Core's own
[nuget.config](../templates/build-core-steps.yml), which maps everything
except `Microsoft.Telemetry*` to nuget.org. Pre-fetching serves two
purposes:

1. **Version pinning** — the `KEY=PATH` pairs are passed via
   `--cmake_extra_defines` (`ORT_FETCH_URL`, `GENAI_FETCH_URL`,
   `WINML_EP_CATALOG_FETCH_URL`) so the cmake defaults in
   `FindOnnxRuntime.cmake` / `FindOnnxRuntimeGenAI.cmake` are never
   silently substituted.
2. **Stage isolation** — the build step no longer needs network access to
   the package feed once prefetch has completed.

Versions are pipeline-level variables, currently:

* `ortVersion`        `1.28.0`   (`Microsoft.ML.OnnxRuntime`)
* `genaiVersion`      `0.15.2` for releases; selected ORT-Nightly version for non-release CI
  (`Microsoft.ML.OnnxRuntimeGenAI.Foundry`)
* `winmlVersion`      `2.1.70`    (`Microsoft.Windows.AI.MachineLearning`, WinML 2.x reg-free)

These must be kept in sync with the cmake defaults and with
`sdk_v2/deps_versions.json` (decision 8). When bumping, update both places
in the same PR.

The shared download logic is in `steps-prefetch-nuget.yml` and exposes both
a PowerShell and a bash implementation behind a `shell` parameter. The
PowerShell build steps consume its `cmakeFetchDefines` variable; Linux
container builds use the downloaded package paths directly.

WinML downloads `Microsoft.Windows.AI.MachineLearning` directly from
nuget.org as a single self-contained reg-free package — no transitive
WinAppSDK Foundation resolution needed. The bash branch fails fast if
`includeWinml=true` is ever passed — WinML is Windows-only.

## Test data

All three SDKs honor `FOUNDRY_TEST_DATA_DIR` (env var) pointing at a
checked-out `test-data-shared` working tree. CI test stages check out
`test-data-shared` (LFS) via `.pipelines/templates/checkout-steps.yml` and
set the env var. SDK *build* stages do not need test-data-shared.

Test data is fetched on every stage that runs tests:

* `cpp_build_win_x64`
* `cpp_build_linux_x64`
* `cpp_build_linux_arm64`
* `cpp_build_osx_arm64`
* All `cs_test_*` and `py_test_*` stages

Skipped on `cpp_build_win_arm64` (cross-compile, no test execution on the
host).

## Build commands

All platforms invoke the standard build driver (no out-of-band CMake calls):

```bash
python build.py --configure --build [--test] --config <buildConfig> \
                --cmake_extra_defines $(cmakeFetchDefines)
```

Windows x64 explicitly passes `--cmake_generator "Visual Studio 17 2022"`.
Windows ARM64 adds `--arm64`. macOS expects `cmake`/`ninja`/`pkg-config` and
falls back to Homebrew if any are missing.

Linux x64 and ARM64 run the same `build.py` command inside the matching
`quay.io/pypa/manylinux_2_28_<arch>` container. The native-build stage uses a
small derived image with the required native build prerequisites. Python
wheels are built in the policy container and passed through `auditwheel repair`;
ORT and GenAI shared libraries remain external because their declared pip
dependencies provide them.

Build output directories follow `build.py`'s convention:
`sdk_v2/cpp/build/<Windows|Linux|macOS>/<Config>/...`.

## Triggers

* `pr: [main, releases/*]` — all build and pack stages run. The shared macOS
   native dylib is Developer ID-signed before its pipeline artifact is published.
* No CI trigger on push for the initial rollout.

## What's deliberately deferred

* **macOS notarization.** Developer ID signing is enforced before packaging,
   but Apple notary submission is not yet wired into the release pipeline.
* **Publishing.** No push to NuGet/internal feeds from this pipeline.
* **macOS x64.** Add when there's a customer ask.
* **Code coverage upload.** `run_coverage.ps1` exists for local use but is
  not wired into CI.

## Things explicitly out of scope

- No `foundry-local-runtime` standalone wheel.
- No multi-repo `Foundry-Local`/`test-data-shared` path-juggling logic in
  the sdk_v2 templates — sdk_v2 paths are repo-relative.
- No private Azure DevOps feed dependency for the Python wheel install
  path — ORT/GenAI come from public PyPI (decision 9).

## Future work (non-blocking)

- **Static linkage of vcpkg deps.** Bundling currently adds ~12 DLLs /
  ~14 MB to each Windows wheel. Static-linking the easy candidates
  (spdlog, fmt) and discussing the harder ones (openssl, curl, oatpp)
  would shrink the wheel and remove the "transitive vcpkg dep silently
  breaks the wheel" risk class.
- **Local-dev `_native/<rid>/` cleanup.** `build.py` populates the in-tree
  staging dir but doesn't prune stale ORT DLLs from prior runs. Pipeline
  is unaffected; only impacts inner-loop devs.
- **macOS delocate.** Revisit if macOS wheel publishing introduces a
  compatibility or dependency-bundling requirement.

## Files

* Coordinator: [stages-sdk-v2.yml](templates/stages-sdk-v2.yml)
* Native build/pack: [templates/stages-build-native.yml](templates/stages-build-native.yml)
* C# stages: [templates/stages-cs.yml](templates/stages-cs.yml)
* Python stages: [templates/stages-python.yml](templates/stages-python.yml)
* Rust stages: [templates/stages-rust.yml](templates/stages-rust.yml), [templates/steps-build-rust.yml](templates/steps-build-rust.yml)
* Pre-fetch: [templates/steps-prefetch-nuget.yml](templates/steps-prefetch-nuget.yml)
* Pack tool: [sdk_v2/cpp/nuget/pack.py](../../sdk_v2/cpp/nuget/pack.py)
* Top-level pipeline: [foundry-local-packaging.yml](../foundry-local-packaging.yml)
