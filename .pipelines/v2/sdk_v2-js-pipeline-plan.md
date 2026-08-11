# sdk_v2 JS pipeline — final status

Status of the `sdk_v2/js` package's CI wiring (see PR
[#748](https://github.com/microsoft/Foundry-Local/pull/748)). Companion to
[sdk_v2-pipeline-plan.md](sdk_v2-pipeline-plan.md), which documents the
C++, C#, and Python stages.

## Scope (as shipped)

- Per-platform build of the Node-API addon
  (`foundry_local_node.node`, `foundry_local_preload.node`) against the
  matching `cpp-native-<rid>` artifact.
- Per-platform integration tests via `vitest run`, gated by
  `FOUNDRY_TEST_DATA_DIR`.
- A single combined npm tarball (`foundry-local-sdk-*.tgz`) bundling
  prebuilds for every supported platform.

Out of scope (deferred):

- **npm publishing.** Pipeline produces the `.tgz` as an artifact only.

## Supported platforms

| Platform        | Native build stage         | JS build | JS test | Notes                          |
|-----------------|----------------------------|----------|---------|--------------------------------|
| Windows x64     | `cpp_build_win_x64`        | ✅        | ✅       |                                |
| Windows ARM64   | `cpp_build_win_arm64`      | ✅        | ❌       | Cross-compiled on x64 host     |
| Linux x64       | `cpp_build_linux_x64`      | ✅        | ✅       |                                |
| Linux ARM64     | `cpp_build_linux_arm64`    | ✅        | ✅       | Native on `onnxruntime-linux-ARM64-CPU-2019`; CPU-only ORT package |
| macOS ARM64     | `cpp_build_osx_arm64`      | ✅        | ✅       | Native on AcesShared Sequoia   |

## Stage graph

```
compute_version
   │
   ├── cpp_build_win_x64 ──── js_build_win_x64 ──── js_test_win_x64 ─┐
   ├── cpp_build_win_arm64 ── js_build_win_arm64 (build-only) ───────┤
   ├── cpp_build_linux_x64 ── js_build_linux_x64 ── js_test_linux_x64┤
   ├── cpp_build_linux_arm64 ─ js_build_linux_arm64 ─ js_test_linux_arm64┤
   └── cpp_build_osx_arm64 ── js_build_osx_arm64 ── js_test_osx_arm64┘
                                                                    │
                                                  (all 5 builds) ──> js_pack ──> js-sdk
```

Every `js_build_<rid>` stage also depends on `cpp_build_win_x64` because
the `cpp-native-include` artifact (public + ms-gsl headers) is produced
only by that stage and consumed by every JS build.

## Artifacts

| Stage                     | Artifact name               | Contents                                                                                       |
|---------------------------|-----------------------------|------------------------------------------------------------------------------------------------|
| `js_build_win_x64`        | `js-prebuild-win-x64`       | `prebuilds/win32-x64/{foundry_local_node.node, foundry_local_preload.node, foundry_local.dll}` |
| `js_build_win_arm64`      | `js-prebuild-win-arm64`     | `prebuilds/win32-arm64/{…, foundry_local.dll}`                                                  |
| `js_build_linux_x64`      | `js-prebuild-linux-x64`     | `prebuilds/linux-x64/{…, libfoundry_local.so}`                                                  |
| `js_build_linux_arm64`    | `js-prebuild-linux-arm64`   | `prebuilds/linux-arm64/{…, libfoundry_local.so}`                                                |
| `js_build_osx_arm64`      | `js-prebuild-osx-arm64`     | `prebuilds/darwin-arm64/{…, libfoundry_local.dylib}`                                            |
| `js_pack`                 | `js-sdk`                    | `foundry-local-sdk-<version>.tgz`                                                              |

Naming convention: artifact names use ADO RIDs (`win-x64`, `osx-arm64`);
in-tarball directories use Node's `${process.platform}-${process.arch}`
(`win32-x64`, `darwin-arm64`) so the runtime addon loader finds them.
Note: `linux-arm64` uses the same key for both ADO RID and the npm tarball
directory (Node's `process.platform` is `"linux"`, `process.arch` is `"arm64"`).

## Build-time native dependencies

The Node-API addon links against `foundry_local` and `#include`s both
the public SDK headers and `<gsl/span>`, transitively included from
[`foundry_local_cpp.h`](../../sdk_v2/cpp/include/foundry_local/foundry_local_cpp.h).
The public C++ wrapper API targets C++17 (`gsl::span` instead of
C++20 `std::span`) for maximum consumer compatibility. The pure C ABI
(`foundry_local_c.h`) has no vcpkg dependency.

The `cpp_build_win_x64` stage bundles the ms-gsl headers from
`vcpkg_installed/x64-windows/include/gsl/` into the existing
`cpp-native-include` artifact alongside `foundry_local/`. ms-gsl is
header-only and platform-agnostic, so a single payload serves every JS
build stage. The bundling step lives in
[`steps-build-windows.yml`](templates/steps-build-windows.yml) and only
runs on the x64 path.

The JS build stage points node-gyp at the downloaded artifacts via
three env-var overrides:

- `FOUNDRY_LOCAL_LIB_DIR` — directory containing `foundry_local.{lib,dll,so,dylib}`,
  honored by
  [`print-import-lib-dir.mjs`](../../sdk_v2/js/script/gyp/print-import-lib-dir.mjs).
- `FOUNDRY_LOCAL_INCLUDE_DIR` — directory containing `gsl/`, honored by
  [`print-vcpkg-include.mjs`](../../sdk_v2/js/script/gyp/print-vcpkg-include.mjs).
  The public include dir (`../cpp/include`) is added separately by
  `binding.gyp`.
- `FOUNDRY_LOCAL_PREBUILD_DIR` — forces the `copy_addon_to_prebuilds`
  destination. Required on win-arm64 because `print-prebuild-dir.mjs`
  otherwise uses the host Node's `process.arch` (x64) and lands the
  cross-compiled addon in `prebuilds/win32-x64/`. Also set on linux-arm64
  for consistency (though native builds already resolve the right arch).

All three overrides are real DX wins for external consumers building
the addon themselves against a downloaded native artifact.

## Cross-compile: win-arm64

Build on the win-x64 agent, invoke `node-gyp rebuild --arch=arm64`.
`foundry_local.{dll,lib}` from `cpp-native-win-arm64` and the public +
gsl headers from `cpp-native-include` provide everything the linker
needs. `FOUNDRY_LOCAL_PREBUILD_DIR` points the addon-copy step at
`prebuilds/win32-arm64/`. No test stage — matches the C# / Python
matrix.

## Linux ARM64: native build + test

Build and test on the `onnxruntime-linux-ARM64-CPU-2019` pool
(`hostArchitecture: arm64`). Same `node-gyp rebuild` path as Linux x64.
`install-native.cjs` uses `Microsoft.ML.OnnxRuntime`, matching the C++ native pipeline.

## Test stage details

- `NodeTool@0` for Node 20.
- Drop `js-prebuild-<rid>` contents into `sdk_v2/js/prebuilds/`.
- Check out `test-data-shared` via `checkout-steps.yml`.
- On macOS, `brew install git-lfs` before checkout (same as Python).
- `npm ci --no-audit --no-fund` (no `--ignore-scripts` — install-native
  is allowed to run on the test side; it's a no-op when the prebuild is
  already in place).
- `npm run build:ts`, then `npx vitest run --reporter=verbose` with
  `FOUNDRY_TEST_DATA_DIR=$(testDataSharedDir)`.

## Pack stage

Runs on a Windows agent (`onnxruntime-Win-CPU-2022`) after all five
builds. Downloads each `js-prebuild-<rid>` artifact, merges them into
`sdk_v2/js/prebuilds/`, stamps the version via
`npm version <v> --no-git-tag-version --allow-same-version`, builds
TypeScript, then runs `npm pack`. Produces one
`foundry-local-sdk-<version>.tgz`.

`npm ci` in the pack stage uses `--ignore-scripts` (install-native
would otherwise re-run during pack-time install).

## Signing

The shared macOS `libfoundry_local.dylib` is signed in
`cpp_build_osx_arm64` before the native artifact is published, so every SDK v2
package consumes the same signed binary. JS-specific native addons are signed
in their `js_build_<rid>` stages on the agents that produced them. Doing this
work in `js_pack` would require routing back to platform-specific agents. The
`.tgz` itself is **not signed** — npm has no equivalent to NuGet package
signing.

| Stage                     | Files signed                                                                  | ESRP keyCode  | Tool                  |
|---------------------------|-------------------------------------------------------------------------------|---------------|-----------------------|
| `js_build_win_x64`        | `foundry_local_node.node`, `foundry_local_preload.node`, `foundry_local.dll`  | `CP-230012`   | SigntoolSign          |
| `js_build_win_arm64`      | same three Windows files                                                      | `CP-230012`   | SigntoolSign          |
| `cpp_build_osx_arm64`     | `libfoundry_local.dylib` (shared by every SDK v2 package)                       | `CP-401337-Apple` | `MacAppDeveloperSign` |
| `js_build_osx_arm64`      | both `.node` files                                                              | `CP-401337-Apple` | `MacAppDeveloperSign` |
| `js_build_linux_x64`      | none                                                                          | n/a           | Linux `.so` has no standard signing |
| `js_build_linux_arm64`    | none                                                                          | n/a           | Linux `.so` has no standard signing |
| `js_pack`                 | none                                                                          | n/a           | `.tgz` not signed by npm convention |

Windows signing block is a near-copy of the SDK DLL signing step in
[`steps-build-cs.yml`](templates/steps-build-cs.yml) — same
`ConnectedServiceName`, ESRP variable group
(`FoundryLocal-ESRP-Signing`), and `signConfigType: inlineSignParams`
shape.

## Files added / modified

**New:**
- [`.pipelines/v2/templates/stages-js.yml`](templates/stages-js.yml)
- [`.pipelines/v2/templates/steps-build-js.yml`](templates/steps-build-js.yml)
- [`.pipelines/v2/templates/steps-test-js.yml`](templates/steps-test-js.yml)
- [`.pipelines/v2/templates/steps-pack-js.yml`](templates/steps-pack-js.yml)

**Modified:**
- [`.pipelines/v2/templates/stages-sdk-v2.yml`](templates/stages-sdk-v2.yml)
  — appends `stages-js.yml`.
- [`.pipelines/v2/templates/steps-build-windows.yml`](templates/steps-build-windows.yml)
  (x64 path only) — stages `vcpkg_installed/x64-windows/include/gsl/`
  into the `cpp-native-include` artifact.
- [`sdk_v2/js/script/gyp/print-vcpkg-include.mjs`](../../sdk_v2/js/script/gyp/print-vcpkg-include.mjs)
  — honors `FOUNDRY_LOCAL_INCLUDE_DIR`.
- [`sdk_v2/js/script/gyp/print-import-lib-dir.mjs`](../../sdk_v2/js/script/gyp/print-import-lib-dir.mjs)
  — honors `FOUNDRY_LOCAL_LIB_DIR`.
- `sdk_v2/js/script/gyp/print-prebuild-dir.mjs` — honors
  `FOUNDRY_LOCAL_PREBUILD_DIR` (added for win-arm64 cross-compile).
- `.gitignore` — negation rule preserving `sdk_v2/js/package-lock.json`
  past the repo-wide lockfile ignore.
- `stages-js.yml` — added `js_build_linux_arm64` and `js_test_linux_arm64`
  stages; updated `js_pack` dependsOn and artifact inputs to include
  `js-prebuild-linux-arm64`.
- `steps-build-js.yml` — added `linux-arm64` to `rid` values; added
  `Set prebuild directory (linux-arm64)` bash step; expanded Linux/macOS
  build and staging bash conditions to cover the new RID.
- `steps-test-js.yml` — added `linux-arm64` to `rid` values; expanded
  all Linux/macOS bash conditions to cover the new RID.
- `steps-pack-js.yml` — added `linux-arm64` to the assembly sources
  array; updated header comment.
- `sdk_v2/js/script/install-native.cjs` — strengthened the missing-RID
  case from a silent `console.warn + return` to a thrown Error with a
  clear message directing users to `FOUNDRY_LOCAL_SKIP_INSTALL=1`.

**Unchanged (intentional):**
- `sdk_v2/js/script/copy-native.mjs` and `pack-prebuilds.mjs` — local-dev
  helpers. CI consumes `cpp-native-<rid>` artifacts directly.
- `sdk_v2/cpp/include/foundry_local/foundry_local_cpp.h` — `<gsl/span>`
  stays; wrapper API is C++17.

## Locked decisions

- **Pack pool:** Windows (`onnxruntime-Win-CPU-2022`), matching
  `cpp_pack_nuget`.
- **Install command:** `npm ci --no-audit --no-fund --ignore-scripts`
  for build + pack; plain `npm ci --no-audit --no-fund` for tests so the
  install-native postinstall runs (no-op when prebuild is present).
- **Version source:** `$(Pipeline.Workspace)/version-info/sdkVersion.txt`,
  stamped via `npm version` in `js_pack`.
- **Signing:** Authenticode for Windows, ESRP MacAppDeveloperSign for
  macOS, none for Linux, none for the `.tgz`.
- **Single combined tarball:** `js_pack` assembles all five prebuilds
  into one `foundry-local-sdk-<version>.tgz`.
- **JS scoped out of WinML.**
- **ORT package:** `Microsoft.ML.OnnxRuntime` on every RID.

## Open items

- **Vitest concurrency on shared pools.** Some tests load real models
  (gated by `FOUNDRY_TEST_DATA_DIR`). On macOS especially, AcesShared
  agents have limited RAM. If runs go OOM, switch to
  `vitest run --pool=forks --poolOptions.forks.singleFork=true`.
  Measure first.
- **macOS notarization.** Developer ID signing is configured with hardened
  runtime and verified before packaging. The release pipeline still needs an
  Apple notary submission for Gatekeeper malware verification.
