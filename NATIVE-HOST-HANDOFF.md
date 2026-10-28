# Native shared host: completed work and remaining gates

## Code and scope

The implementation is in draft PR #1228, **Add a native shared host for Foundry Local SDKs**.

- Code branch: `baijumeswani/shared-serving`
- Squashed code commit: `00c6ee032065863a4dd65ee838e299c445d35777`
- Handoff branch: `baijumeswani/shared-serving-handoff`
- Original branch base: `69a5270babffdd6d92d23f2e613e0847a1121aa1`

The squash preserved the tested source tree exactly. It did not rebase onto the newer `main`.
Rebuild and repeat the relevant gates after a rebase or further code changes.
The handoff is a separate documentation commit and is not part of the one-commit code PR.

**This is an implementation handoff, not a release approval.** Windows focused tests and local package
checks passed. Feature work and release gates listed below are still open. No application consumer
repositories were changed or published as part of this update.

## Completed implementation

### Common native backend

- Embedded execution remains the default. Hosted execution is an explicit configuration choice.
- Existing manager, catalog, model, session and preflight entry points dispatch to explicit native
  embedded or hosted backends. Language bindings do not implement their own IPC/process managers.
- Backend-bound handles have tagged representations. Ordinary request, response and item storage
  remains local, with owned payloads transferred across the private protocol.
- Hosted execution reuses the native inference/session implementation. The host itself uses the
  embedded backend; it does not recursively start another shared host.
- IPC uses Windows named pipes and Linux/macOS Unix-domain sockets, with scoped object identities,
  request admission, cancellation, callbacks and streamed input/output queues.
- Discovery, metadata, downloads and readiness observations do not start a host or acquire ownership.
  Model/session operations acquire the shared host when needed.
- Runtime catalog registrations are reconciled after startup. Model load/unload checks use artifact
  identity, rather than assuming a path still refers to the same files.
- There is no silent embedded fallback, uncertain-operation replay or automatic session reconstruction
  after terminal host loss.

Broader modality, queue and cancellation parity still needs the qualification below. Wiring an entry
point is not evidence that every supported scenario has passed.

### Host startup, sharing and lifetime

- A dependency-light C++ executable ships with a runtime manifest. Bootstrap verifies the selected
  files and loads the native dependencies inside the child.
- A runtime/protocol/effective-profile identity selects the shared host. Installation path and language
  package version are not intended compatibility keys. Exact dependency checks currently constrain
  cross-package sharing; see the Python issue below.
- Startup coordination, owner-process observation and host-incarnation checks prevent adopting an
  incompatible host or treating stale object IDs as current.
- Hosted managers own persistent client connections. Local disposal releases only that client's
  ownership and dependent work, not the other clients' host.
- Final-owner departure drains admitted work, allows a fixed five-second reconnect grace, commits
  shutdown and performs normal native shutdown. Successor startup must observe actual predecessor
  process exit; EOF or removal of a readiness file is insufficient.
- Reference counting covers host lifetime, not per-model ownership. Explicit shared unload retains
  native in-use rejection.
- HTTP inference requires a sponsoring manager. An HTTP connection pool is not a host owner.
  Global HTTP shutdown is rejected for the shared host; shared web-service Stop is scoped/protected.
- Repeated or second-client hosted web-service Start reuses the existing listener. The original
  embedded-mode behavior is unchanged.

### All five bindings and ABI compatibility

| Binding | Hosted configuration |
|---|---|
| C++ | Configuration setter in the existing wrapper |
| C# | `UseSharedHost` |
| Python | `use_shared_host` |
| JavaScript/TypeScript | `useSharedHost` |
| Rust | `.use_shared_host(bool)` |

The additive configuration setter is appended to C API function-table version 3. Existing table
prefixes and API-v2 callers remain supported; payload versions remain 2. New wrappers negotiate the
table version before reading the extension, fall back safely for default/false, and report unsupported
hosted mode on an older runtime. Public C++ wrappers remain C++17-compatible.

Configuration, native marshalling and existing model/session APIs are wired in every binding.
Conflicting embedded/hosted singleton reuse is rejected, including when derived Rust handles retain
the native manager.

### Packaging and documentation

- The native executable and manifest are required runtime payloads for supported desktop RIDs.
- NuGet staging validates the manifest schema, names, build identity and final file hashes. Signing
  refreshes the manifest after the final native bytes are established.
- C# restore/publish, Python wheel assembly, npm installation, Rust runtime acquisition and native
  archives propagate the host alongside normal runtime assets.
- Unix extensionless executables, publish permissions and ZIP/TAR executable metadata are handled.
- Windows, Linux and macOS producer templates were updated. The Unix archive helper was executed
  locally and its executable metadata checked; this was not Linux/macOS execution qualification.
- Existing SDK README/configuration/reference pages describe opt-in, lazy startup, sharing and
  lifetime. Rust documentation labels the older standalone service as a preview pending migration.

## Windows validation completed

These are focused runs and local test packages, not final signed release artifacts.
Counts from different runs below must not be added together as a single suite result.

| Check | Result and boundary |
|---|---|
| Native build | Final RelWithDebInfo build passed through `build.py`. |
| Native host/backend tests | 160 tests across 17 suites passed. |
| Hosted HTTP regression | Seven HostServer tests passed, including repeated/second-client Start. Included again in the later focused native run. |
| C++ compatibility | C++17/API-v2 compatibility executable exited 0. |
| C# | All SDK target frameworks compiled without warnings/errors; four focused compatibility tests passed. |
| Python | 38 selected tests passed with no skips. Ruff retained 42 existing diagnostics and introduced none; this is not a clean repository-wide lint result. |
| JavaScript | Manager run: 23 passed, six existing skips. Later dispose run: 15 passed, six existing skips. Addon, TypeScript/type builds and scoped Biome passed. |
| Rust | 47 unit, 25 build-support and three HTTP-extraction tests passed: 75 total. Clippy for library/tests with warnings denied and formatting passed. A focused actual-native conflicting-mode test also passed. |
| Runtime packaging | 17 NuGet staging/manifest tests passed; six edited pipeline templates parsed. |
| Native formatting | Newly introduced native files formatted; whitespace checks passed. |

Actual package checks also passed:

1. A local native NuGet was restored and published into a separate C# consumer, with actual host
   startup and normal process exit.
2. An actual npm artifact was packed and installed through its ordinary dependency installer.
   Clean C# and JavaScript consumers shared one host PID/URL and one tiny coding model with independent
   histories. JavaScript continued inference after C# disposal. Final JavaScript disposal produced
   actual host exit code 0.
3. A fresh `cp311-abi3-win_amd64` wheel was built with fresh cffi/native payloads and installed in a
   clean environment using ordinary declared dependencies. With native/PYTHONPATH overrides removed,
   it generated tiny coding output, verified independent histories, served HTTP and released its
   packaged host normally. This was not a Windows ARM64 execution test.
4. An actual Rust crate was packed, unpacked as a separate consumer dependency and exercised through
   normal NuGet acquisition. An initial upstream GenAI feed returned 401; the successful restore used
   the original, already-restored package through an ordinary local NuGet feed. Tiny coding inference,
   independent sessions, HTTP and actual normal host exit passed.

The earlier expanded 487-test run was **not green**: 484 individual tests passed, suite setup failed
because `FOUNDRY_TEST_DATA_DIR` was missing, and three model-dependent chat tests skipped.
Do not report that run as a full-suite pass.

Local logs/package probes live under the ignored `sdk_v2/cpp/build/` tree and are not shipped with
this branch. Useful evidence names include `native-host-build65-tests.log`,
`native-host-mixed-package-execution64.log`, `native-host-clean-wheel-execution.log`,
`native-host-rust-acquisition-tests.log` and `native-host-nuget-pack-tests.log`.

## Pending feature work

### 1. Migrate consumers, then remove the older Rust host

- Replace application adapters' preview `SharedService` use with the ordinary hosted manager/model
  APIs. Pin the new SDK commit and regenerate the real dependency/vendor graph and consumer SDK.
- Keep the manager alive for the entire HTTP/session lifetime; releasing a session alone is not
  necessarily manager disposal.
- Preserve authoritative `/models/loaded?details=true` observations of the actual loaded artifact
  and effective context. Catalog context and preflight are not substitutes, and observing state must
  not load a model.
- Preserve existing provider discovery, permissions, activation and explicit application Start.
- Update lifetime/recovery tests for client-local disposal and automatic final-owner shutdown.
- Only after consumers/tests migrate, remove the preview Rust `SharedService`, its Rust executable
  and the old Rust-specific bundle publishing. Those remain in the squashed code today.
- Consumer repositories have their own prescribed build, vendor and generated-SDK workflows.
  Follow those on Linux rather than hand-editing generated dependencies or SDK copies.
  Their changes remain separate/local-only unless separately authorized for publication.

### 2. Resolve Python/NuGet host compatibility

The current profile includes exact resident native/ORT/GenAI hashes and image identity. Normal Python
installations resolve ORT through pip; C#/JavaScript resolve it through NuGet.

The two inspected Windows ORT DLLs both reported file version `1.31.0.20261008.6.402979a`, but differed:

| Source | Bytes | SHA-256 |
|---|---:|---|
| NuGet | 17,253,176 | `0C42DC4E278F0968FDF9EBF1B1E54A74C657A9942E70803E453C91993451CF4C` |
| pip | 19,288,928 | `417BED96648EF4C6B5B9DF0F7518350C7315BFBDA702BC29DCF6296061156C8C` |

The current key selects separate hosts for these installations. This is a conservative compatibility
policy, not proof that the binaries are ABI-incompatible or that Python cannot use IPC. The reason for
the binary difference has not been established.

Decide whether normal installations should use one common host execution bundle, or whether verified
client/host protocol compatibility can be separated from exact execution-file integrity. Do not simply
remove integrity verification. Python hosted operation passed; concurrent Python/NuGet sharing has
not been demonstrated.

### 3. Complete platform robustness follow-ups

- Review/fix the POSIX launcher's blocking `waitpid` against startup-deadline requirements.
- Explicitly handle inherited nonstandard descriptors in POSIX spawn.
- Address the macOS non-atomic socket/accept/pipe `fcntl(FD_CLOEXEC)` fork/exec window.
- Qualify hardened macOS host loading with separately signed/ad-hoc ORT, GenAI and provider libraries.
- Review bounded reconciliation for an interrupted launch with unknown child identity. The current
  conservative behavior can preserve uncertainty indefinitely; a successor must not kill/replace an
  unproven live predecessor.
- Audit optional provider identities beyond native/ORT/GenAI, and static-linked hosted usage.

## Pending validation and release gates

### Platform and package matrix

Execute on actual `win-x64`, `win-arm64`, `linux-x64`, `linux-arm64` and `osx-arm64` systems.
Windows x64 local evidence does not qualify the other RIDs; a cross-build is not execution evidence.

For each supported binding/RID combination, install the normal packages without development redirects
or host-specific copying. Check clean installation, relocation, bootstrap, signing, permissions,
manifest integrity, existing supported publish forms and old API-v2/default-embedded compatibility.
Repeat with exact final signed release artifacts, not just local `99.0.0-native-host-test.*` packages.

Prove cross-binding sharing with matching dependencies/effective profiles and safe separation for
incompatible ones. Record host PID, loaded artifact, independent histories, continued inference after
the first client exits and actual final process exit code.

### Lifetime, IPC and API parity

Qualify client disposal/crash, last-owner crash, starter death before attachment, reconnect/shutdown
races, an inherited connection surviving in another process, stale session/preflight IDs, uncertain
operations without replay, unfinished input queues, output backpressure and synchronous cancellation
from callbacks. Confirm admission closes/drains correctly without destroying active native resources.

Cover every already-supported modality and media/item/queue API, including streaming and cancellation,
under both embedded and hosted modes. Add focused regressions for newly reproduced defects.
Run the model-dependent suite with the real required cache and investigate unexpected skips.
Run the repository's existing Linux sanitizer/memory checks as applicable.

### Real coding-model and application gate

Use the real `qwen_3.8_27b_int4_int8kv_cpuembed_dflash2_int4` fixture, registered with task `coding`,
tool-calling support and reasoning support. Verify:

- Tool calling and failed-tool recovery with the tools unchanged, preserving the full prompt prefix.
- A genuine workspace edit followed by independently executed tests.
- The original long-turn/240-second timeout scenario, cancellation and repeated recovery.
- Compatible-client sharing, continued inference after one client leaves and normal final shutdown.
- Normal authenticated application onboarding/flow and the regenerated real consumer SDK.

Recovery tests carried forward from the earlier Rust service are not proof that this full real-model
flow passes on the new native host. The original full-prefix recovery and long-turn timeout remain
unresolved until reproduced and verified.

The CUDA teardown fix is tracked by `microsoft/onnxruntime#33291`, commit
`c64c9fd76404df85bbbac37485d5c0f2babcd194`. The FPA-enabled CUDA 12.8 candidate previously checked was
`1.32.0.20261022.1.c64c9fd`, SHA-256
`121fdc32e6de78ba0180a20c5de9d57408fb4f974b62577f915e57a171345cac`.
Registration and tiny-model actual-exit checks passed for that candidate; full Qwen qualification
did not. Record the exact provider actually loaded in the final real-model gate.

## Linux starting commands

Run from the repository root on Linux after installing the normal compiler, CMake/vcpkg and runtime
dependency prerequisites. Use normal feed access for the intended ORT/GenAI versions. The commands
below are a source-checked starting point, **not an already-executed Linux result**.

Keep logs, temporary files and test results inside the ignored build tree. Do not pass `--build_dir`
or bypass `build.py` with direct CMake build commands.

```bash
set -euo pipefail
repo_root="$(pwd)"
build="$repo_root/sdk_v2/cpp/build/Linux/RelWithDebInfo"
mkdir -p "$build/validation/tmp"
export TMPDIR="$build/validation/tmp"

python sdk_v2/cpp/build.py --configure --build \
  --config RelWithDebInfo --parallel 4 \
  --cmake_extra_defines FOUNDRY_LOCAL_BUILD_TESTS=ON FOUNDRY_LOCAL_BUILD_SERVICE=ON \
  2>&1 | tee "$build/validation/build.log"

(
  cd "$build/bin"
  ./foundry_local_tests \
    --gtest_filter='ApiBackendTest.*:BootstrapDescriptorTest.*:ChildHostRoleTest.*:Host*.*:IpcChannelTest.*:ItemCodecTest.*:ProcessWatchTest.*:RequestAdmissionTest.*:Sdk*.*' \
    --gtest_output="xml:$build/validation/native-host-tests.xml" \
    2>&1 | tee "$build/validation/native-host-tests.log"
  ./cpp-api-v2-compatibility
)

ctest --test-dir "$build" --build-config RelWithDebInfo \
  --tests-regex '^elf-' --no-tests=error --output-on-failure \
  2>&1 | tee "$build/validation/elf-tests.log"

python sdk_v2/cpp/nuget/test_pack.py \
  2>&1 | tee "$build/validation/pack-tests.log"
```

The focused filter matches the final Windows host/backend run; Linux may have different platform-only
tests/counts. Check that the intended suites actually run and investigate skips. The ELF checks are
existing Linux loader/export gates, not substitutes for hosted lifecycle/package execution.
For incremental native rebuilds, use `python sdk_v2/cpp/build.py --build --config RelWithDebInfo --parallel 4`.

For a Rust source-level check against that native build:

```bash
export CARGO_TARGET_DIR="$repo_root/sdk_v2/cpp/build/rust-host-validation"
export FOUNDRY_LOCAL_NATIVE_BIN_DIR="$build/bin"
cargo test --manifest-path sdk_v2/rust/Cargo.toml \
  --lib --test build_support_test --test http_extract_test
cargo clippy --manifest-path sdk_v2/rust/Cargo.toml --lib --tests -- -D warnings
cargo fmt --manifest-path sdk_v2/rust/Cargo.toml -- --check
```

That native-directory override is a development check, not a clean-package gate. For the latter,
clear development overrides and use the actual packaged crate with normal runtime NuGet acquisition
through `FOUNDRY_LOCAL_RUNTIME_VERSION` and the documented feed/configuration settings.
Build/install the actual NuGet, npm and wheel artifacts similarly; merely running bindings against
a build directory does not qualify package propagation or cross-binding sharing.

Before broad model-dependent runs, set `FOUNDRY_TEST_DATA_DIR` to the real provisioned model cache
using the existing test policy. Do not use an empty placeholder to make setup appear successful.
Preserve failing results. An HTTP response alone, a removed readiness file or forced process
termination does not establish recovery or normal shutdown.
