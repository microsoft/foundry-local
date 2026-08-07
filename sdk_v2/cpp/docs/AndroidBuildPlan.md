# Android Build & Test Infrastructure

## Overview

The C++ SDK supports cross-compilation for Android (arm64-v8a, x86_64) with emulator-based test
execution. The approach follows patterns from
[onnxruntime-genai](https://github.com/microsoft/onnxruntime-genai/pull/227) adapted for a pure C/C++
project (no Java/Gradle layer). Cross-compiling is supported from Windows, Linux, and macOS hosts.

### Current Status

- **597 / 597** tests pass on the Android x86_64 emulator (API 29)
- 1 test skipped (`CApiTest.RemoveFromCacheNotCachedModelFails`) — included in the pass count

---

## Quick Start

### Prerequisites

- Android NDK (tested with r29: `D:\Android\ndk\29.0.14206865`)
- Android SDK with emulator, platform-tools, and a system image
  (e.g. `system-images;android-29;default;x86_64`)
- Java 17+ (required by `sdkmanager` / `avdmanager` — specify with `--java_home` if system Java is older)

### Build & Test

```bash
# Full build + emulator test
python build.py --android --android_abi x86_64 \
    --android_ndk_path /path/to/ndk \
    --build --test --android_run_emulator \
    --android_emulator_api 29 \
    --config Debug \
    --java_home /path/to/jdk-17

# Build only (no tests), ARM64 for device
python build.py --android --android_abi arm64-v8a \
    --android_ndk_path /path/to/ndk \
    --build --config Release
```

---

## vcpkg Triplets & Dependencies

### Triplet Files

- `triplets/arm64-android.cmake` — ARM64 devices
- `triplets/x64-android.cmake` — x86_64 emulator

Both triplets use:
- **API level 28** (Android 9.0 Pie) — NDK provides iconv from this level, avoiding autotools
  cross-compilation issues with libiconv
- **Static library linkage** — all vcpkg deps are statically linked into `libfoundry_local.so`
- **NDK toolchain chaining** via `VCPKG_CHAINLOAD_TOOLCHAIN_FILE` (reads `ANDROID_NDK_HOME` env var,
  set by `build.py`)

### Dependency Status

| Dependency                | Android Status       | Notes                                          |
| ------------------------- | -------------------- | ---------------------------------------------- |
| `nlohmann-json`           | Works (header-only)  | No changes needed                              |
| `spdlog`                  | Builds for Android   | Uses `android_sink_mt` for logcat output       |
| `gtest`                   | Builds for Android   | No changes needed                              |
| `azure-storage-blobs-cpp` | Works via vcpkg      | Pulls in libcurl + OpenSSL (statically linked) |
| `oatpp`                   | Excluded on Android  | Web service disabled via CMake `if(ANDROID)`   |
| OpenSSL                   | vcpkg builds 3.x     | Unversioned `.so` works natively on Android    |

---

## CMake Configuration

`CMakeLists.txt` has `if(ANDROID)` guards for:

- **Service build disabled** — oatpp web service is not applicable on device
- **Examples disabled** — interactive examples don't run on device
- **`log` library linked** — Android's `liblog.so` for spdlog's `android_sink`
- **Windows-specific logic skipped** — delay-load, `.def` exports, `<stacktrace>`

The `ANDROID` variable is detected early from the vcpkg triplet name (before `project()` sets it):
```cmake
if(VCPKG_TARGET_TRIPLET MATCHES "android")
    set(FOUNDRY_LOCAL_BUILD_SERVICE OFF)
endif()
```

### Test Data Path

`test/CMakeLists.txt` sets `FOUNDRY_LOCAL_TEST_DATA_DIR` conditionally:
- **Android:** `"testdata"` (relative — test binary runs from the directory containing `testdata/`)
- **Desktop:** `"$<TARGET_FILE_DIR:foundry_local_tests>/testdata"` (absolute path to build output)

---

## build.py — Android Arguments

```
--android                Build for Android (requires --android_ndk_path or ANDROID_NDK_HOME)
--android_abi ABI        Target ABI: arm64-v8a (default), x86_64
--android_api LEVEL      Minimum API level (default: 28)
--android_home PATH      Android SDK root (default: $ANDROID_HOME, or inferred from NDK path)
--android_ndk_path PATH  Android NDK path (default: $ANDROID_NDK_HOME)
--android_run_emulator   Start emulator and run tests (requires x86_64 ABI)
--android_emulator_api N API level for emulator system image (default: same as --android_api)
--java_home PATH         Java 17+ installation for SDK tools (default: $JAVA_HOME)
```

### Configure Phase

When `--android` is specified, `build.py`:

1. Sets `ANDROID_NDK_HOME` env var for vcpkg triplet toolchain chaining
2. Sets `VCPKG_TARGET_TRIPLET` to `arm64-android` or `x64-android` based on ABI
3. Passes `-DANDROID_ABI`, `-DANDROID_PLATFORM` to CMake
4. Forces Ninja generator (VS generators don't support Android NDK)
5. Forces `FOUNDRY_LOCAL_BUILD_SERVICE=OFF`

### Validation

- `--android` requires `--android_ndk_path` or `ANDROID_NDK_HOME`
- `--android_run_emulator` requires `--android_abi x86_64`
- `--java_home` is validated to be Java 17+ (Android SDK tools require it)
- Falls back to manual AVD creation if `avdmanager` fails (common with Java version mismatches)

---

## Emulator Helpers (`tools/android.py`)

A standalone Python module providing emulator lifecycle and test execution:

| Function                       | Purpose                                                                |
| ------------------------------ | ---------------------------------------------------------------------- |
| `validate_java_home(path)`     | Validates Java 17+ at the given path                                   |
| `get_sdk_tool_paths(sdk_root)` | Resolves `emulator`, `adb`, `sdkmanager`, `avdmanager` with platform extensions |
| `create_virtual_device(...)`   | Installs system image + creates AVD (avdmanager with manual fallback)  |
| `start_emulator(...)`          | Launches emulator with CI defaults, waits for boot                     |
| `stop_emulator(...)`           | Graceful shutdown (`CTRL_BREAK_EVENT`/`SIGTERM`) with kill fallback    |
| `run_tests_on_device(...)`     | Pushes binary + libs + test data + models, builds CA bundle, captures logcat, runs tests |

### AVD Creation Fallback

`avdmanager` frequently fails due to Java version or SDK XML format incompatibilities. The module
falls back to **manual AVD creation** — writing `config.ini` and `.ini` files directly to
`~/.android/avd/`. This is more robust across environments.

### Emulator Defaults

- **Memory**: 4096 MB
- **GPU**: `swiftshader_indirect` (software rendering, no host GPU required)
- **Flags**: `-no-snapstorage -no-audio -no-boot-anim -delay-adb`
- **Linux**: adds `-no-window` (headless for CI)
- **Boot timeout**: 20 minutes
- **Boot detection**: `adb wait-for-device` → poll `getprop sys.boot_completed`

### Cross-Platform Process Management

| Concern              | Windows                    | Linux/macOS      |
| -------------------- | -------------------------- | ---------------- |
| Tool extensions      | `.exe` / `.bat`            | No extension     |
| Stop signal          | `CTRL_BREAK_EVENT`         | `SIGTERM`        |
| Process creation     | `CREATE_NEW_PROCESS_GROUP` | Default          |

---

## Test Execution Flow

When `--android_run_emulator --test` is specified, `build.py`:

1. **Create AVD**: `foundry_local_android` with `system-images;android-{api};default;x86_64`
2. **Start emulator**
3. **Push artifacts** to `/data/local/tmp/foundry_tests/`:
   - Test binary (`foundry_local_tests`)
   - Shared libraries (`libfoundry_local.so`, `libc++_shared.so`)
   - `testdata/` directory (audio files, JSON fixtures)
   - CPU test models from `FOUNDRY_TEST_DATA_DIR` (if available — GPU/embedding models are skipped)
4. **Build CA certificate bundle** from device system certs (see [SSL Handling](#sslcertificate-handling))
5. **Run tests**:
   ```
   cd /data/local/tmp/foundry_tests && \
     LD_LIBRARY_PATH=. \
     FOUNDRY_TEST_DATA_DIR=./test-model-cache \
     SSL_CERT_FILE=./cacert.pem \
     ./foundry_local_tests --gtest_color=yes
   ```
6. **Capture logcat** — `adb logcat -d -s foundry_local:* DEBUG:* libc:*` for SDK logs and crash diagnostics
7. **Stop emulator**

---

## SSL/Certificate Handling

### Problem

Statically-linked OpenSSL/libcurl (built by vcpkg) **ignores both the `SSL_CERT_DIR` and the
`SSL_CERT_FILE` environment variables** at request time. The default CA location (`--openssldir`) is
baked at build time to a path that does not exist on Android, and this libcurl build was not
compiled with the fallback that would otherwise consult `SSL_CERT_FILE`. As a result, setting either
environment variable alone has **no effect** — curl uses its (non-existent) compiled-in default and
verification fails with a misleading "self-signed certificate in certificate chain".

> This was confirmed on-device: passing the device's own trust store via `SSL_CERT_FILE` still
> failed, while the *exact same* bundle passed explicitly as a CA file (`openssl s_client -CAfile`,
> equivalent to `CURLOPT_CAINFO`) verified `ai.azure.com` with `Verify return code: 0 (ok)`. Only the
> delivery mechanism differed.

### Solution

The Core reads the `SSL_CERT_FILE` environment variable and forwards it explicitly to libcurl via
`CurlTransportOptions.CAInfo` (`CURLOPT_CAINFO`), which libcurl **always** honors. See
`src/http/http_client.cc` and `src/http/http_download.cc`. Callers therefore still set
`SSL_CERT_FILE`, but its effect now comes from the Core forwarding it to `CAInfo` — not from OpenSSL
auto-reading the environment.

### Building the bundle for testing

`tools/android.py` builds a CA bundle at test time from the device's system certificates:

1. Probes for cert directories:
   - `/apex/com.android.conscrypt/cacerts` (API 34+)
   - `/system/etc/security/cacerts` (API 28–33)
2. Concatenates all PEM files into a single bundle:
   ```
   cat /system/etc/security/cacerts/*.0 > /data/local/tmp/foundry_tests/cacert.pem
   ```
3. Sets `SSL_CERT_FILE` pointing to the bundle (the Core forwards it to `CAInfo`)

This approach uses the device's actual trust store, so it works with any API level and includes
all root CAs that the device trusts (including DigiCert Global Root G2 needed for Azure endpoints).

### Production (with Java Bindings — Future)

The Java/Android host app will export system certificates (including user-installed CAs and
enterprise roots) to a PEM file and set `SSL_CERT_FILE` before loading the native library. This is
the same pattern FL Core used.

### Key Insight

Neither `SSL_CERT_DIR` nor `SSL_CERT_FILE` is auto-consulted by this statically-linked
OpenSSL/libcurl on Android. The CA bundle **must** be passed explicitly via `CURLOPT_CAINFO`; the
Core does this by forwarding `SSL_CERT_FILE`. The error message when the store is missing is
misleading: "self-signed certificate in certificate chain" — it actually means OpenSSL cannot find
**any** CA store.

---

## Android Logging

`src/spdlog_logger.cc` uses spdlog's `android_sink_mt` (guarded by `#ifdef __ANDROID__`) to route
logs to Android logcat via `__android_log_write`. The tag is `"foundry_local"`.

CMakeLists.txt links against `log` (Android's `liblog.so`) when building for Android.

---

## Design Decisions

### Why not a custom toolchain wrapper?

The NDK's `android.toolchain.cmake` handles compiler selection, sysroot, STL, and API level. A
wrapper would add maintenance burden for no benefit.

### Why Ninja as default generator?

VS generators don't support Android NDK cross-compilation. Ninja works on all host platforms.

### Why `adb push` + `adb shell` instead of Gradle?

The project is pure C/C++. GTest binaries are pushed directly and executed via `adb shell`. When
Java bindings are added, Gradle `connectedDebugAndroidTest` will be added as a parallel path.

### Why x86_64 for emulator testing?

x86_64 emulator images run natively on x86_64 CI hosts with hardware acceleration. arm64-v8a
emulation on x86_64 is significantly slower. arm64-v8a builds are validated by compilation;
runtime testing happens on x86_64 emulator.

### Why static linking for vcpkg dependencies?

Static linking bundles everything into `libfoundry_local.so`. Only one `.so` needs deployment.
Avoids shared library path issues on Android.

### Why API 28 minimum?

The NDK provides `iconv` starting at API 28, which avoids autotools cross-compilation issues
with `libiconv` (a transitive dependency of several vcpkg ports).

### Java/Gradle — Deferred

Java bindings are planned but cleanly deferred. The infrastructure is entirely additive:

| Component            | Now (C++)                 | Later (+Java)                           | Rework? |
| -------------------- | ------------------------- | --------------------------------------- | ------- |
| CMake toolchain      | Set up                    | Unchanged                               | No      |
| build.py `--android` | Cross-compile C++         | Add `--build_java` flag                 | No      |
| Emulator helpers     | Generic                   | Unchanged                               | No      |
| Test runner          | `adb push`/`adb shell`   | Add Gradle `connectedDebugAndroidTest`  | No      |
| Shared lib naming    | `libfoundry_local.so`     | `System.loadLibrary("foundry_local")`   | No      |

---

## Remaining Work

- [x] ~~**CApiTest failures**~~ — Fixed. `Configuration::Validate()` now uses lazy `GetHomeDir()` — only
  called when `{home}` placeholder appears or `app_data_dir` is unset. Default `logs_dir` and
  `model_cache_dir` derive from the resolved `app_data_dir`, not from `GetHomeDir()`. Tests use
  `CreateTestConfig()` (in `c_api_test_helpers.h`) which sets `app_data_dir` on Android so `$HOME` is
  never needed. Regression tests in `configuration_test.cc` verify this contract.
- [x] ~~**Logcat capture**~~ — `android.py` clears logcat before tests and dumps SDK/crash logs after.
- [x] ~~**Certificate checker utility**~~ — `ssl_cert_checker.h/.cc` validates `SSL_CERT_FILE` at SDK
  init time (Android debug builds only) using OpenSSL's X509 APIs.
- [ ] **CI pipeline** — ADO YAML pipeline for automated Android builds and emulator tests.

---

## File Summary

| File                            | Status       | Description                                    |
| ------------------------------- | ------------ | ---------------------------------------------- |
| `build.py`                      | Modified     | `--android*`, `--java_home` args, configure + test runner |
| `CMakeLists.txt`                | Modified     | `if(ANDROID)` guards, `log` link, service/examples disabled |
| `test/CMakeLists.txt`           | Modified     | Conditional `FOUNDRY_LOCAL_TEST_DATA_DIR` (relative on Android) |
| `src/spdlog_logger.cc`          | Modified     | `android_sink_mt` for logcat on `__ANDROID__`  |
| `src/configuration.cc`          | Modified     | Lazy `GetHomeDir()` — only called when `{home}` placeholder used or `app_data_dir` unset |
| `src/platform/ssl_cert_checker.h/.cc` | **New** | Android-only SSL certificate validation diagnostic |
| `test/internal_api/c_api_test_helpers.h` | **New** | Shared test helpers: `ASSERT_FL_OK`, `CreateTestConfig`, `IsOk`, `StatusGuard`, `GetApi` |
| `test/internal_api/c_api_test.cc` | Modified   | Uses shared helpers; `CreateTestConfig` sets `app_data_dir` on Android |
| `test/internal_api/configuration_test.cc` | Modified | Regression tests for explicit `app_data_dir` without `{home}` |
| `tools/android.py`              | **New**      | Emulator management, AVD creation, test execution, CA bundle, logcat capture |
| `triplets/arm64-android.cmake`  | **New**      | vcpkg ARM64 triplet (API 28, static libs)      |
| `triplets/x64-android.cmake`    | **New**      | vcpkg x86_64 triplet (API 28, static libs)     |
