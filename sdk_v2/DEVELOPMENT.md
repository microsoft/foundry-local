# sdk_v2 — Developer Setup

A first-time contributor should be able to install the tools listed below,
clone the repo, then run the one-shot build/test script from this directory
and watch all four SDKs go green:

```powershell
pwsh ./build_and_test_all.ps1
```

If that passes, your machine is correctly configured.

## Prerequisites

All four SDKs (C++, C#, Python, JS/TS) build on **Windows**, **Linux**, and
**macOS**. WinML 2.x hardware acceleration is bundled automatically on Windows.

### All platforms

| Tool             | Minimum version | Notes                                                                                 |
| ---------------- | --------------- | ------------------------------------------------------------------------------------- |
| Git              | recent          | LFS is **not** required.                                                              |
| CMake            | 3.20            | Driven by `sdk_v2/cpp/build.py`; do not invoke `cmake --build` directly.              |
| vcpkg            | recent          | Set `VCPKG_ROOT`, or use the copy bundled with Visual Studio (auto-detected).         |
| Python           | 3.11–3.14, **64-bit** | Required by `build.py` and for the Python SDK. 32-bit Python will not work.   |
| .NET SDK         | 9.0             | The SDK targets `net8.0;net9.0;netstandard2.0`; the test project additionally targets `net462` on Windows (via the .NET Framework Targeting Pack from VS); samples target `net9.0`. The single package bundles WinML 2.x on Windows — its OS-version floor is enforced by the native runtime (`LoadLibraryW` + `RtlGetVersion` in `winml_ep_bootstrapper.cc`), not by a .NET TFM. |
| Node.js          | 20 LTS or newer | Brings `npm`. The JS SDK declares `"engines": { "node": ">=20" }`.                    |
| PowerShell       | 7+ (`pwsh`)     | The one-shot script and `samples/js/test-v2.ps1` are written for PowerShell 7.        |

### Windows-only

| Tool                        | Version                  | Notes                                                                 |
| --------------------------- | ------------------------ | --------------------------------------------------------------------- |
| Visual Studio 2026 (v18)    | Enterprise / Professional / Community | Install the **Desktop development with C++** and **.NET desktop development** workloads. Provides MSVC, Windows SDK, and vcpkg. |
| Windows SDK                 | 10.0 (VS workload)       | Required by the C++ build for any `windows.h` consumer. The exact patch number is not pinned — whatever the VS C++ workload installs (currently 10.0.26100) works. |
| .NET Framework 4.6.2 Targeting Pack | (VS workload)    | One C# test target framework is `net462`.                             |

Launch your dev shell with **x64** explicitly — `Enter-VsDevShell` defaults
to x86 and silently breaks the Python cffi extension build:

```powershell
pwsh -NoExit -Command "& { Import-Module 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'; Enter-VsDevShell <instance-id> -SkipAutomaticLocation -Arch amd64 -HostArch amd64 }"
```

Verify with `echo $env:VSCMD_ARG_TGT_ARCH` — must be `x64`. (The one-shot
script also forces `VSCMD_ARG_TGT_ARCH=x64` around the Python step as a
safety net, but the C++ step does not, so getting the shell right matters.)

### Linux

* GCC 11+ or Clang 14+ (C++20 with `<format>` support).
* `build-essential`, `ninja-build`, `pkg-config`, `curl`, `zip`, `unzip`, `tar`.
* vcpkg dependencies: `autoconf`, `automake`, `libtool`, `python3-pip`.

### macOS

* Xcode 15+ command-line tools (Clang with C++20).
* Homebrew packages: `cmake`, `ninja`, `pkg-config`, `python@3.11`, `node`, `dotnet-sdk`.

## What gets installed per-SDK

The one-shot script does not pre-install language toolchains, but it does
install per-SDK package dependencies on first run:

| SDK    | What runs                                                                              |
| ------ | -------------------------------------------------------------------------------------- |
| C++    | `python build.py [--config ...] [--skip_tests]` — configure + build + ctest. vcpkg restores native deps; ORT/GenAI come from NuGet via FetchContent (versions from `sdk_v2/deps_versions.json`). |
| C#     | `dotnet test Microsoft.AI.Foundry.Local.SDK.sln -c Release` — restores NuGet packages on demand. |
| Python | `python -m pip install -e .[dev]` (compiles the cffi extension; needs MSVC/Clang) → `python -m pytest test/`. |
| JS     | `npm install` (runs `node-gyp` against the C++ build output) → `npm run build` → `npm test` (vitest). |

## Common knobs

```powershell
# Full build + test, default config (RelWithDebInfo / Release).
pwsh ./build_and_test_all.ps1

# Just rebuild the native and the JS bindings, skip the slow C++ test pass.
pwsh ./build_and_test_all.ps1 -Only cpp,js -SkipCppTests

# Skip one SDK.
pwsh ./build_and_test_all.ps1 -Skip python

# Keep going past failures and print the summary at the end.
pwsh ./build_and_test_all.ps1 -ContinueOnError
```

See `pwsh ./build_and_test_all.ps1 -?` for the full parameter list.

## Model cache for tests

Model-dependent sdk_v2 tests require `FOUNDRY_TEST_DATA_DIR` to point to a
local model cache directory.

In CI, sdk_v2 pipelines pre-populate this directory from Azure Artifacts.
For local runs, developers should populate a local cache first, then point
`FOUNDRY_TEST_DATA_DIR` at it.

### Local developer flow (recommended)

Use the Foundry CLI/SDK locally to download the test models your SDK tests need.
This keeps local setup simple and avoids requiring blob storage credentials.

CLI Example:
```powershell
# 1) Inspect aliases available on your machine/region
foundry model list

# 2) Download the CPU models needed by sdk_v2 integration tests
foundry model download qwen2.5-0.5b-instruct-generic-cpu
foundry model download qwen3.5-0.8b-generic-cpu
foundry model download deepseek-r1-distill-qwen-14b-generic-cpu
foundry model download openai-whisper-tiny-generic-cpu
foundry model download qwen3-embedding-0.6b-generic-cpu
foundry model download nemotron-speech-streaming-en-0.6b-generic-cpu
```

Then set env var `FOUNDRY_TEST_DATA_DIR` to the cache path you want tests to use.

Example:

```powershell
$env:FOUNDRY_TEST_DATA_DIR = "$env:USERPROFILE\.foundry\cache\models"
pwsh ./build_and_test_all.ps1
```

The directory can have any name. What matters is that it already contains the
model files expected by the tests you are running.

### CI flow

CI does not run `foundry model download ...`. Instead, pipelines fetch a fixed
model set into `FOUNDRY_TEST_DATA_DIR` using:

- `.pipelines/templates/fetch-models-from-artifacts-feed.yml`

The legacy template name is retained to avoid changing its existing callers. The
template downloads pinned Universal Packages from the project-scoped
`AIFoundryLocal/AIFoundryLocal_PublicPackages` feed. The pipeline and feed are in
the same Azure DevOps organization, so downloads use the pipeline identity and
do not require an Azure service connection.

Each package contains one model's `v<model-version>` directory. The template
downloads it into the cache layout expected by the local model scanner:

```text
test-data-shared/
└── Microsoft/
    └── <model-alias>-<model-version>/
        └── v<model-version>/
            ├── genai_config.json
            ├── inference_model.json
            └── <model files>
```

### Updating the test models available in CI

Models are published as separate Universal Packages so one model can be added or
updated without republishing the entire test cache. Universal Package versions
are immutable.

To publish a model, first stage a complete cache alias directory. For example:

```text
C:\model-staging\qwen2.5-0.5b-instruct-generic-cpu-4\
└── v4\
    ├── genai_config.json
    ├── inference_model.json
    └── <model files>
```

The model leaf must contain `inference_model.json` with the versioned cache ID:

```json
{
  "Name": "qwen2.5-0.5b-instruct-generic-cpu:4"
}
```

Install the Azure DevOps CLI extension, authenticate with an identity that has
Feed Publisher permission, and publish the alias directory:

```powershell
az extension add --name azure-devops
az login

$packageName = '<lowercase-package-name>'
$packageVersion = '<major.minor.patch>'
$modelDirectory = 'C:\model-staging\<cache-alias>-<model-version>'

az artifacts universal publish `
  --organization "https://dev.azure.com/aiinfra" `
  --project "AIFoundryLocal" `
  --scope project `
  --feed "AIFoundryLocal_PublicPackages" `
  --name $packageName `
  --version $packageVersion `
  --description "Foundry Local generic CPU test model cache" `
  --path $modelDirectory
```

Publishing the alias directory uploads its contents, so `v4` is a top-level
directory within the package. Do not publish the contents of `v4` directly;
the version directory must remain present when `UniversalPackages@0` downloads
the package into the target cache alias directory. Do not ZIP the directory.

After publishing, add or update the corresponding `UniversalPackages@0` task in:

- `.pipelines/templates/fetch-models-from-artifacts-feed.yml`

Checklist:

1. Use a stable lowercase package name for a model variant and pin an exact
   semantic package version in the template; do not use `*`.
2. Set `downloadDirectory` to
   `${{ parameters.destinationPath }}/Microsoft/<cache-alias>-<model-version>`.
3. For a new model version, stage the new `v<model-version>` directory, update
   `inference_model.json`, publish a new package version, and update the cache
   alias, package version, and download path in the template.
4. To correct packaging without changing the model version, publish a new patch
   package version (for example, `4.0.1`) and update only
   `vstsPackageVersion`. The cache ID can remain at model version `4`.
5. Keep at least one model per test task used across SDKs (chat, embeddings,
  audio/ASR) so integration suites remain runnable.
6. Validate by running `pwsh ./build_and_test_all.ps1` with
  `FOUNDRY_TEST_DATA_DIR` set to a cache containing the same model set.
7. Verify the package with a CI download before removing an older package entry.

## Troubleshooting

* **`cl.exe ... HostX86\x86\cl.exe` during pip install** — your shell has
  `VSCMD_ARG_TGT_ARCH=x86`. Re-launch with `-Arch amd64 -HostArch amd64`
  (see above). The one-shot script also guards against this for the Python
  step.
* **Python build fails with `__stdcall`/`__cdecl` errors** — same root cause:
  cl.exe is targeting x86. Fix the shell.
* **C# tests load the wrong native** — never invoke `cmake --build` directly
  or pass `--build_dir` to `build.py`. The C# tests pin an absolute path to
  `sdk_v2/cpp/build/<Platform>/<Config>/`; bypassing `build.py` puts the
  binary somewhere else.
