# Foundry Local SDK — JavaScript / TypeScript (v2)

Native bindings around the Foundry Local **C++ SDK**, surfaced as an ESM npm package
(`foundry-local-sdk`). This README is the orientation guide for a developer who has just
cloned the repo and wants to build, test, and debug the JS SDK.

> The architectural plan lives in [docs/PortJsToSdkV2.md](docs/PortJsToSdkV2.md). The
> implementation conventions live in
> [.github/instructions/js-sdk-v2.instructions.md](../../.github/instructions/js-sdk-v2.instructions.md)
> and [.github/instructions/js-sdk-v2-items.instructions.md](../../.github/instructions/js-sdk-v2-items.instructions.md).
> Read those before changing the addon or item types.

---

## 1. Architecture in 30 seconds

```
your TS/JS code
      │
      ▼
 public TS surface  ──►  src/             (Manager, Catalog, Model, Session, Item, ItemQueue, ...)
      │
      ▼
 Node-API C++ addon ──►  native/src/      (foundry_local_node.node, C++20, node-addon-api)
      │
      ▼
 C++ wrapper header ──►  ../cpp/include/foundry_local_cpp.h   (header-only, C++17)
      │
      ▼
 C ABI              ──►  foundry_local.{dll,so,dylib}         (built from ../cpp/src/)
      │
      ▼
 ONNX Runtime + ORT-GenAI
```

The addon talks to the **C++ wrapper**, never directly to the C ABI. If the wrapper is
missing something, fix the wrapper rather than reaching past it.

---

## 2. Prerequisites

| Tool             | Version           | Notes                                                          |
|------------------|-------------------|----------------------------------------------------------------|
| Node.js          | **20 LTS or newer** | ESM-only package, `engines.node >= 20`.                       |
| npm              | bundled with Node | (or pnpm/yarn — npm scripts are what's tested).                |
| Python           | 3.10+             | Required by `node-gyp` **and** by `sdk_v2/cpp/build.py`.       |
| CMake            | 3.28+             | Drives the C++ build.                                          |
| C++ toolchain    | C++20 capable     | MSVC 19.38+ (VS 2022 17.8), Clang 16+, or GCC 12+.             |
| vcpkg            | bootstrapped      | The C++ build uses a vendored vcpkg manifest; see `../cpp/`.   |

Platform-specific:

- **Windows:** Install "Desktop development with C++" workload in Visual Studio 2022 or
  2026, plus the Windows 10/11 SDK. `node-gyp` auto-discovers MSVC (see the VS 2026 caveat
  below if you're on that toolchain). PowerShell 7 recommended.

  > **Visual Studio 2026 (VS 18) requires node-gyp ≥ 12.1.0.** Older node-gyp versions
  > (including 11.5.0, which is what ships with current Node 23) only recognize
  > VS 2017–2022, and when they detect they're running inside a VS dev prompt they refuse
  > to fall back to another installation — the build fails with `unknown version
  > "undefined"` / `could not find a version of Visual Studio 2017 or newer to use`.
  > Fixes, in order of preference:
  >
  > 1. Bump the bundled node-gyp:
  >    ```pwsh
  >    npm install --save-dev node-gyp@^12.1.0
  >    ```
  > 2. Build from a plain PowerShell (not a VS Developer prompt) so node-gyp auto-discovers
  >    your VS 2022 install.
  > 3. Pin the toolset to VS 2022:
  >    ```pwsh
  >    npm config set msvs_version 2022
  >    ```
- **Linux:** `build-essential`, `libssl-dev`, and the Foundry Local ORT/GenAI runtime
  dependencies the C++ build pulls in via vcpkg.
- **macOS:** Xcode Command Line Tools; deployment target is `11.0`.

You do **not** need to install `node-gyp` globally — it ships as a dev-dependency.

---

## 3. Building

The JS package depends on the **C++ SDK** being built first. The native addon links against
`foundry_local.{dll,so,dylib}` and copies it (plus ORT runtime siblings) into
`prebuilds/<platform>-<arch>/`.

### 3.1 Build the C++ SDK first

From the repo root:

```pwsh
python sdk_v2/cpp/build.py --configure --build --config RelWithDebInfo
```

Output lands at:

```
sdk_v2/cpp/build/Windows/<Config>/bin/<Config>/ or
sdk_v2/cpp/build/<Linux|macOS>/<Config>/bin/
   ├── foundry_local.{dll,so,dylib}     ← the C ABI library
   ├── onnxruntime*                     ← ORT siblings
   └── onnxruntime-genai*               ← GenAI siblings
```

> **Important:** always invoke `build.py`. Do not call `cmake --build` directly and do not
> pass `--build_dir` — those skip the platform segment in the output path and the JS
> `copy-native` script (and the C# tests) won't find the binaries. See
> [.github/instructions/cpp-build.instructions.md](../../.github/instructions/cpp-build.instructions.md).

Common configs:

| Config             | When                                                                     |
|--------------------|--------------------------------------------------------------------------|
| `Debug`            | Stepping through native code, full PDBs, no optimization.                |
| `RelWithDebInfo`   | Default for dev — optimized but symbols intact. The npm scripts default here. |
| `Release`          | Ship config, no PDBs.                                                    |

The JS build defaults to `RelWithDebInfo`. To build the addon against a different config,
set `FOUNDRY_LOCAL_CPP_CONFIG` before running the npm scripts:

```pwsh
$env:FOUNDRY_LOCAL_CPP_CONFIG = "Debug"
npm run build
```

### 3.2 Build the JS package

From `sdk_v2/js/`:

```pwsh
npm install
npm run build
```

`npm run build` is the umbrella script. It runs, in order:

1. **`copy-native:dev`** — copies `foundry_local.{dll,so,dylib}` and its ORT/GenAI siblings
   from `../cpp/build/<Platform>/<Config>/bin/<Config>/` into
   `prebuilds/<process.platform>-<process.arch>/`.
2. **`build:native`** — `node-gyp rebuild` produces `foundry_local_node.node`
   (the addon) and copies it into the same `prebuilds/` directory.
3. **`build:ts`** — `tsc -p tsconfig.build.json` produces `dist/`.

If you only changed TypeScript: `npm run build:ts`.
If you only changed C++ in `native/src/`: `npm run build:native`.
If you only rebuilt the C++ SDK: `npm run copy-native:dev` followed by `npm run build:native`
(the addon needs to relink against any ABI changes).

### 3.3 Why C++20 for the addon but C++17 for the wrapper header?

The addon source files (`native/src/*.cc`) compile at **C++20** — this is an internal
choice that matches node-gyp's default and avoids the MSVC D9025 "overriding /std" warning.
The **C++ wrapper header** (`foundry_local_cpp.h`) stays C++17-consumable because external
C++ consumers need to include it from any toolchain. The addon happily includes a C++17
header from a C++20 TU.

---

## 4. Using the C++ SDK directly (without the JS layer)

The C++ SDK is a first-class consumer of the same library the addon links against. You can
use it from a standalone C++ project to reproduce, debug, or prototype behavior outside the
JS layer:

```cpp
// my_repro.cc — link against foundry_local + include the wrapper header
#include "foundry_local_cpp.h"

int main() {
    foundry_local::ManagerOptions opts;
    opts.app_name = "my-repro";
    foundry_local::Manager mgr(opts);
    auto catalog = mgr.GetCatalog();
    auto model = catalog.GetModel("qwen2.5-0.5b-instruct-generic-cpu");
    model->Load();
    foundry_local::ChatSession sess(*model);
    foundry_local::Request req;
    req.AddItem(foundry_local::Item::Text("Hello"));
    auto resp = sess.ProcessRequest(req);
    // ...
}
```

Build it against:

- Header: `sdk_v2/cpp/include/foundry_local_cpp.h`
- Import lib (Windows) / shared lib (POSIX): from
  `sdk_v2/cpp/build/<Platform>/<Config>/bin/<Config>/`
- C++ standard: **17 or newer** (the header is C++17-clean)

The integration tests under `sdk_v2/cpp/test/sdk_api/` are the canonical examples — read
[audio_transcriptions_test.cc](../cpp/test/sdk_api/audio_transcriptions_test.cc) or
[streaming_audio_test.cc](../cpp/test/sdk_api/streaming_audio_test.cc) for end-to-end usage
patterns. Their TS equivalents under `sdk_v2/js/test/` mirror them line-for-line where
practical, which is useful when chasing a JS-layer regression: reproduce in C++, confirm
the wrapper behavior, then trace the divergence into the addon.

The other SDKs (`sdk_v2/cs/`, `sdk_v2/python/`) consume the **same** `foundry_local`
binary. Their tests are an equally valid reference for behavior.

---

## 5. Testing

Test runner: **Vitest**. From `sdk_v2/js/`:

```pwsh
npm test           # one-shot run
npm run test:watch # watch mode
```

### 5.1 The two flavors of test

- **Cache-only / JS-layer tests** — run unconditionally as long as the addon is built.
  They use an in-memory fake catalog (`test/_fixtures/cacheOnlyManager.ts`) to exercise
  Manager, Catalog, Model, Request, ItemQueue, Item, and Session validation paths without
  loading real models.
- **Real-model integration tests** — gated by the `FOUNDRY_TEST_DATA_DIR` environment
  variable. They construct a real `Manager` pointed at the cache, load the model, and run
  inference. Used for `ChatSession`, `EmbeddingsSession`, `AudioSession`, streaming, etc.

### 5.2 Running the real-model tests

Point `FOUNDRY_TEST_DATA_DIR` at a directory that holds the cached model variants. The C++
SDK's `SharedTestEnv` and the C# / Python test suites use the same cache layout, so the
same directory works across SDKs:

```pwsh
$env:FOUNDRY_TEST_DATA_DIR = "D:\path\to\foundry-local-test-models"
npm test
```

When the variable is **unset**, those tests show up as **`skipped`** in the vitest summary
— not passed. That distinction matters: a clean run without the cache should look like
`70 passed | 20 skipped (90)`, never `90 passed (90)`. If the count of skipped tests is
zero and you didn't set the cache, the gating logic is broken — file a bug.

The fixture lives in [test/_fixtures/realModelManager.ts](test/_fixtures/realModelManager.ts).
In CI, if the requested model is **not already cached**, the fixture skips rather than
triggering a multi-gigabyte download. Local developers implicitly opt into downloads simply
by setting `FOUNDRY_TEST_DATA_DIR`.

### 5.3 Running a single test file or test

```pwsh
npx vitest run test/audio-session.test.ts
npx vitest run -t "transcribes Recording.mp3"
```

### 5.4 Lint / format

```pwsh
npm run lint       # biome check
npm run format     # biome format --write
```

---

## 6. Debugging

### 6.1 Debugging TypeScript / JS

Use VS Code's JavaScript debugger:

1. Open the test file you want to debug.
2. From the command palette: **"JavaScript Debug Terminal"**.
3. In that terminal: `npx vitest run path/to/file.test.ts`.

Breakpoints in TS source and the `dist/` output both bind correctly — the vitest config
turns on source maps. For a quick `console.log`-style print, vitest forwards stdout to the
terminal in real time.

### 6.2 Debugging the native addon (C++)

The addon is a `.node` file loaded into the Node process. Debug it like any native DLL/SO
loaded into Node:

**Windows (MSVC):**

1. Build the **C++ SDK in `Debug`** and the addon against it:
   ```pwsh
   python sdk_v2/cpp/build.py --build --config Debug
   $env:FOUNDRY_LOCAL_CPP_CONFIG = "Debug"
   cd sdk_v2/js
   npm run copy-native:dev
   npm run build:native
   ```
2. In VS Code, create a launch configuration of type `cppvsdbg` that launches `node.exe`
   with arguments `node_modules/vitest/vitest.mjs run test/your-file.test.ts` and `cwd`
   set to `sdk_v2/js`. Set breakpoints in `native/src/*.cc`.
3. Symbols for `foundry_local.dll` are in the same `bin/Debug/` directory as the DLL —
   the debugger picks them up automatically.

**Linux / macOS:**

Use `lldb` or `gdb`:

```bash
lldb -- node node_modules/vitest/vitest.mjs run test/your-file.test.ts
```

### 6.3 Debugging the C++ SDK itself

If the failure is inside the C++ wrapper or implementation (not the JS layer), reproduce
it against the C++ integration tests — they're faster to iterate on and have proper symbols:

```pwsh
python sdk_v2/cpp/build.py --build --config Debug
cd sdk_v2/cpp/build/Windows/Debug/bin/Debug
.\sdk_integration_tests.exe --gtest_filter="AudioSessionFixture.TranscribeFromUri"
```

`--gtest_filter` is essential — the full suite loads multiple models and takes ~10 minutes.

### 6.4 ORT / GenAI load failures

If the addon throws on first use with a `LoadLibrary` / `dlopen` error, the most likely
cause is that the ORT/GenAI sibling DLLs aren't next to `foundry_local_node.node`. Rerun
`npm run copy-native:dev`. The required sibling list is in
[script/copy-native.mjs](script/copy-native.mjs).

For the deeper contract, see
[.github/instructions/ort-loading-contract.instructions.md](../../.github/instructions/ort-loading-contract.instructions.md).

---

## 7. Repository layout (this package)

```
sdk_v2/js/
├── binding.gyp              # node-gyp build descriptor for the addon
├── package.json             # npm scripts, deps
├── tsconfig.json            # base TS config (editors)
├── tsconfig.build.json      # build TS config (emits dist/)
├── biome.json               # lint/format config
├── vitest.config.ts         # test runner config
│
├── src/                     # public TS surface (Manager, Catalog, Model, Session, ...)
├── native/src/              # node-addon-api C++ addon (C++20)
├── script/                  # build helper scripts (copy-native, pack-prebuilds, gyp helpers)
├── test/                    # vitest test files
│   └── _fixtures/           # shared test fixtures (cacheOnlyManager, realModelManager)
├── docs/                    # design docs
├── prebuilds/               # GENERATED — addon + native deps per <platform>-<arch>
└── dist/                    # GENERATED — emitted JS + .d.ts
```

`prebuilds/` and `dist/` are gitignored and regenerated by `npm run build`. Never edit them
by hand.

---

## 8. Where to file changes

| Change                                                  | Edit here                                        |
|---------------------------------------------------------|--------------------------------------------------|
| Public TS API surface                                   | `src/`                                           |
| Addon / native binding logic                            | `native/src/`                                    |
| A new test                                              | `test/` (mirror an existing file's structure)    |
| C ABI surface or C++ wrapper                            | `../cpp/` (escalate — affects all language SDKs) |
| ORT/GenAI loading rules                                 | See ort-loading-contract instructions           |
| Item discriminated union                                | See js-sdk-v2-items instructions                |

When in doubt, the C# and Python v2 SDKs (`sdk_v2/cs/`, `sdk_v2/python/`) have already
worked out the wrapper-call mapping for nearly every operation. Mirror them; do not invent
a new mapping.
