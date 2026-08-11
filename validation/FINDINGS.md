# Foundry Local 2.0.0 — Findings & Reporting

This is the shared reporting surface for **multi-machine, multi-session** validation. Each
agent that runs the harness on a Windows / Linux / macOS box reports its findings here so we
build one cross-platform go/no-go picture.

Read [`VALIDATION.md`](VALIDATION.md) for intent and how to run, and
[`ACCEPTANCE_POLICY.md`](ACCEPTANCE_POLICY.md) for what the result classes mean.

## Reporting protocol (avoid merge conflicts)

1. **Machine-readable (preferred).** Run the harness; it writes
   `validation/results/<hostname>__<run-id>.json`. **Commit that file** (one file per
   machine/run — no shared-line edits, so agents never conflict).
2. **Human summary.** Add one row per machine to the **Agent run log** table below (append
   only). Keep it short; link to your results file.
3. **Aggregate.** Anyone can regenerate the combined checklist:
   `python3 validation/orchestrator/aggregate.py` → writes
   `validation/results/COMBINED_CHECKLIST.md` and prints the provisional go/no-go verdict.
4. **Planned tracking matrix.** [`STATUS_MATRIX.md`](STATUS_MATRIX.md) is the up-front
   go/no-go matrix for **every** planned cell across all target platforms (regenerate with
   `python3 validation/orchestrator/plan_matrix.py`). Cells show `NOT RUN` until a result
   record is committed, then the recorded outcome is overlaid — so it doubles as a live
   tracker of what has passed/failed/blocked and what is still outstanding.

Do **not** hand-edit another agent's results file. Do **not** overwrite
`COMBINED_CHECKLIST.md` by hand — it is generated.

## Agent run log (append one row per machine/run)

| date | agent/owner | platform_id | arch | accelerators exercised | results file | pass | fail | blocked | notes |
|------|-------------|-------------|------|------------------------|--------------|------|------|---------|-------|
| 2026-08-10 | @baijumeswani / mac-mini | macos-arm64 | arm64 | cpu | `results/Baijus-Mac-mini.local__*.json` | 7 | 1 | 0 | install-smoke: all 4 SDKs PASS. pkg-inspect: cpp/cs/python PASS, js FAIL (no license — F1). |
| 2026-08-10 | @baijumeswani / mac-mini | macos-arm64 | arm64 | cpu, webgpu | `results/Baijus-Mac-mini.local__realfeat-*.json` | 12 | 1 | 0 | Real feature E2E via in-process FFI (no service). Python: chat (cpu+**webgpu**), embeddings, tool-calling, vision, web-server, audio-file, integrations all PASS. JS: chat, embeddings, web-server PASS; tool-calling FAIL (F2, stale sample). C#: build+native-load PASS (run needs net9.0 runtime / roll-forward — F3). WebGPU inference independently verified (model returned exact expected token). |
| 2026-08-10 | @baijumeswani / mac-mini | macos-arm64 | arm64 | cpu, webgpu | (direct API validation) | 10 | 0 | 0 | **Model-management surface (Python) 10/10 PASS**: catalog.list_models, get_model, CPU+WebGPU variant enumeration, explicit select_variant, download + idempotent cache-reuse (2nd download ~0s), get_cached_models, load→is_loaded→unload lifecycle, get_latest_version. Covers the `model-mgmt` capability (only a C# per-SDK sample exists; validated directly against the RC API). |
| 2026-08-10 | @baijumeswani / mac-mini | macos-arm64 | arm64 | cpu | (direct API validation) | 8 | 0 | 0 | **Cross-cutting surface (Python) 8/8 PASS** (`scripts/crosscutting_check.py`): unknown alias → `None` (no crash); invalid `select_variant` raises typed `FoundryLocalException`; non-deprecated `ChatSession.process_request` returns text; two concurrent `ChatSession`s on one loaded model both respond; `Request.cancel()` before dispatch is safe; **unload correctly refused with a typed error while a session is live** then succeeds once the session is closed; reload + reuse after unload works. Confirms the dispose/lifecycle & error-handling contracts. |
| 2026-08-10 | @baijumeswani / mac-mini | macos-arm64 | arm64 | cpu | (direct API validation) | 8 | 0 | 0 | **Soak & resource gates (Python) 8/8 PASS** (`scripts/soak_resource_check.py`): 16 load/unload cycles all infer & unload cleanly; long streaming (124 chunks + final `Response`); mid-stream `Request.cancel()` winds the worker down without wedging (process stays healthy); 4 concurrent sessions all respond; sustained 20-inference RSS growth negligible (+20MB); model load 0.6s; first-token latency 0.08s. See **N4** for the load/unload RSS observation. |
| 2026-08-10 | @baijumeswani / mac-mini | macos-arm64 | arm64 | cpu | (direct API validation) | 7 | 0 | 0 | **Model-mgmt failure / hostile paths (Python) 7/7 PASS** (`scripts/model_mgmt_failure_check.py`, each scenario in an isolated subprocess/cache): unknown alias → `None`; invalid variant → typed error; corrupt/truncated weights → typed `FoundryLocalException` (no crash); missing weight file → `load` raises typed error (no crash, see **N5**); read-only cache dir → typed error on download (no crash); 3 concurrent downloads of the same model are safe (model then loads & infers); `remove_from_cache` lifecycle (`is_cached` True→False). Graceful-degradation contract holds — no segfault/hang in any scenario. |
| 2026-08-10 | @baijumeswani / mac-mini | macos-arm64 | arm64 | cpu | (direct API validation) | 4 | 0 | 0 | **1.x→2.0 source/API compat (Python) 4/4 PASS** (`scripts/compat_surface_check.py`, baseline = 1.2.4): 2.0 **preserves the entire 1.x core public surface** — all 1.x top-level symbols, every `FoundryLocalManager` method (`initialize/instance/discover_eps/download_and_register_eps/start_web_service/stop_web_service`), and every `IModel` method — no removals. `foundry_local_sdk.openai` remains an importable submodule. 2.0 is otherwise **additive** (54 new top-level symbols: the `ChatSession`/`Request`/`Item` model, +`close/shutdown`). Only behavioral change is the `get_*_client()` deprecation (source-compatible, warns). See **N6** for the caveats on the *runtime/service* upgrade slice (still fleet work). |
| _e.g. 2026-08-11_ | _@you / linux-cuda-agent_ | _linux-x64_ | _x64_ | _cpu, cuda_ | _`results/host__id.json`_ | _–_ | _–_ | _–_ | _–_ |
| 2026-08-10 | @baijumeswani / onnxr4c75000005 (A100×8) | linux-x64 | x64 | cpu | `results/onnxr4c75000005__linux-pkg.json` | 7 | 1 | 0 | install-smoke: all 4 SDKs PASS (cpp/cs need .NET **SDK 9**; box default is SDK 8 → NETSDK1045 until installed). pkg-inspect: cpp/cs/python PASS (linux-x64 `libfoundry_local.so` + LICENSE present), js FAIL (**F1** no license — reproduced on Linux). |
| 2026-08-10 | @baijumeswani / onnxr4c75000005 | linux-x64 | x64 | cpu | `results/onnxr4c75000005__linux-pyfeat.json` | 7 | 0 | 0 | Python feature E2E via in-process FFI (no service): chat, embeddings, tool-calling, audio-file, vision, web-server, integrations **all PASS** on CPU. (Note: Python tool-calling passes on Linux — the F2 failure is JS-only.) |
| 2026-08-10 | @baijumeswani / onnxr4c75000005 | linux-x64 | x64 | cpu | `results/onnxr4c75000005__linux-jsfeat.json` | 6 | 1 | 0 | JS feature E2E: chat, embeddings, audio-file, vision, web-server, integrations PASS; tool-calling FAIL (**F2**, same stale-sample `serviceEndpoint`+`startWebService` bug — reproduced on Linux, not an RC defect). |
| 2026-08-10 | @baijumeswani / onnxr4c75000005 | linux-x64 | x64 | cpu, cuda | `results/onnxr4c75000005__linux-csfeat.json` | 0 | 4 | 0 | C# chat (cuda) + embeddings/web-server/model-mgmt: sample build **fixed** (**F5** — corrected fix **removes** the wrong GPU-specialized packages; samples build with only default ORT + GenAI.Foundry) → then **run real CUDA models on the A100** (`qwen2.5-0.5b-instruct-cuda-gpu:4`, CUDA 12.8 runtime) and produce **correct** inference output, but the process **SIGSEGVs (exit 139) at shutdown** in 1DS telemetry teardown (**F6**, new linux-x64 RC defect). Runtime functionally verified; exit-code assertion fails. |
| 2026-08-10 | @baijumeswani / onnxr4c75000005 | linux-x64 | x64 | cpu, cuda | (direct-API validation) | 8 | 0 | 0 | Direct-API + CUDA: model-mgmt 10/10, crosscutting 8/8, compat-surface 4/4, model-mgmt-fail 7/7, soak (bounded/flat — see below), ep-bootstrap cpu+cuda, and **REAL CUDA chat on A100** ('Paris' 0.38s, load 1.2s). CUDA needs user-supplied CUDA 12 runtime (**F7**). WebGPU: no linux-x64 variant published (blocked/experimental). |
| 2026-08-10 | @baijumeswani / onnxr4c75000005 | linux-x64 | x64 | cpu, cuda | (F7 fix verify — branch `baijumeswani/f7-cuda-graceful-degrade`) | 2 | 0 | 0 | **F7 fixed + verified.** Rebuilt `libfoundry_local.so` (c_api.cc + manager.cc) into the RC Python venv. Without CUDA 12 runtime: `download_and_register_eps()` **no longer raises** → logs graceful-degrade Warning, CUDA `is_registered=false`, CPU inference returns 'Paris'. With CUDA 12.8 runtime: CUDA still registers (`is_registered=true`, no regression). Detector unit-test semantics unchanged. |
| 2026-08-10 | @baijumeswani / onnxr4c75000005 | linux-x64 | x64 | cpu | (F6 fix verify — branch `baijumeswani/f6-cs-shutdown-crash`) | 2 | 0 | 0 | **F6 fixed + verified (A/B).** C# `FoundryLocalManager` now subscribes to `AppDomain.ProcessExit` and Disposes there (mirrors Python `atexit`/JS `process.on("exit")`). No-`Dispose` repro built against source SDK: **pre-fix control = exit 139 (SIGSEGV) ×3; post-fix = exit 0 ×3** (no-Dispose *and* explicit-Dispose paths — idempotent, no double-free). Root cause: native `Manager::s_instance_` + 1DS `LogManager` destroyed during C-runtime static destruction after 1DS globals gone → null fn-ptr call. |
| 2026-08-11 | @baijumeswani / DESKTOP-4T800KF (RTX A2000) | windows-x64 | x64 | cpu | `results/DESKTOP-4T800KF__win-install.json`, `__win-pkg.json` | 6 | 2 | 0 | install-smoke: **all 4 SDKs PASS** (two-source: RC from ORT feed + deps from `packagefeedproxy.microsoft.io` mirrors — public pypi/npm CDNs blocked on this box). pkg-inspect: cpp/cs PASS (win-x64 `foundry_local.dll` + LICENSE), **js FAIL (F1** no license — reproduced), **python FAIL (F8** new — wheel ships a **111 MB `foundry_local.pdb`** embedding 561 build-agent paths). Needed 3 cross-platform harness fixes (npm→npm.cmd, UTF-8 child stdio, cross-platform RSS). |
| 2026-08-11 | @baijumeswani / DESKTOP-4T800KF | windows-x64 | x64 | cpu | `results/DESKTOP-4T800KF__win-pyfeat.json` | 7 | 0 | 0 | Python feature E2E via in-process FFI (no service): chat, tool-calling, embeddings, audio-file, vision, web-server, integrations **all PASS** on CPU. (Python tool-calling passes on Windows — F2 is JS-only, consistent with Linux. web-server needed `PYTHONIOENCODING=utf-8` — sample printed streamed Unicode to a cp1252 console; **sample bug, not RC** — harness now forces UTF-8 child stdio.) |
| 2026-08-11 | @baijumeswani / DESKTOP-4T800KF | windows-x64 | x64 | cpu | `results/DESKTOP-4T800KF__win-jsfeat.json` | 6 | 1 | 0 | JS feature E2E: chat, embeddings, audio-file, vision, web-server, integrations PASS; tool-calling FAIL (**F2**, same stale-sample `serviceEndpoint`+`startWebService` bug — reproduced on Windows, not an RC defect). |
| 2026-08-11 | @baijumeswani / DESKTOP-4T800KF (RTX A2000) | windows-x64 | x64 | cpu, cuda | `results/DESKTOP-4T800KF__win-csfeat.json` | 0 | 7 | 0 | C# E2E: 5 run cells (chat/embeddings/audio-file/web-server/model-mgmt) all **complete inference correctly** (markers + sanity pass; sample auto-selects the `cuda-gpu` variant on this GPU box) but **exit 3221226324 = 0xC0000374 STATUS_HEAP_CORRUPTION at teardown** → **F6 reproduced on windows-x64** (Linux exit-139 SIGSEGV ⇒ Windows heap-corruption; same 1DS teardown path). 2 build cells (tool-calling/vision) fail to **compile**: `ToolChoice` ambiguous between `Microsoft.AI.Foundry.Local` and the Betalgo OpenAI lib (**F10**, stale sample vs new RC type). |
| 2026-08-11 | @baijumeswani / DESKTOP-4T800KF (RTX A2000) | windows-x64 | x64 | cpu, cuda, webgpu | `results/DESKTOP-4T800KF__win-direct.json` | 10 | 1 | 0 | Direct-API + accelerators: model-mgmt 10/10, crosscutting 8/8, model-mgmt-fail 7/7, compat-surface 4/4, soak (plateaus 481MB — **best** of the 3 platforms; `mem_char.py` confirms sustained-inference WorkingSet+Private plateau after ~10 infers, **no leak**), ep-bootstrap cpu/cuda/webgpu, and **REAL CUDA + REAL WebGPU chat** (both → 'Paris'). **1 FAIL: ep-bootstrap winml-dml (F9)** — RC exposes **no DirectML/WinML EP** (only CUDA/NvTensorRTRTX/OpenVINO/WebGpu) though `platform_manifest` lists winml-dml as *required*; npu-winml blocked (no NPU hardware + no WinML NPU EP). |
| 2026-08-11 | @baijumeswani / DESKTOP-4T800KF (RTX A2000 + Intel Iris Xe) | windows-x64 | x64 | nvtensorrtrtx, openvino | (F9 deep-dive — corrects earlier winml claim) | 1 | 1 | 0 | **Ran the WinML EP models.** After `download_and_register_eps()` the catalog exposes **16 `*-trtrtx-gpu` + 18 `*-openvino-gpu`** variants (catalog is EP-registration-filtered — the earlier "no WinML variants" reading enumerated *before* registration). **NvTensorRTRTX: REAL inference PASS** — `qwen2.5-0.5b-instruct-trtrtx-gpu:2` → 'Paris' (load 9.0s/infer 1.58s), **but only with `onnxruntime-genai-cuda.dll` (cuda-ep bundle) on the DLL search path**; out-of-box the load fails `Error 126` (native-loader gap, **F9**). **OpenVINO: FAIL** — generator creation aborts on the Intel Iris Xe allocator (`VendorId:32902`). Also confirmed **F1 fixed on `main` (PR #971)** and **F6 fixed on `main` (PR #974)** — neither in the RC branch. |
| 2026-08-11 | @baijumeswani / DESKTOP-4T800KF (RTX A2000 + Intel Iris Xe) | windows-x64 | x64 | nvtensorrtrtx (load-order) | (F9 load-order probe) | 2 | 0 | 0 | **Answered: does installing CUDA EP alongside NvTensorRTRTX fix trtrtx without PATH changes?** **Scenario A** — register CUDA + NvTensorRTRTX via `download_and_register_eps()`, then load `trtrtx-gpu` directly → **STILL FAILS** (`onnxruntime-genai-cuda.dll missing`): EP *registration* alone does not load the DLL. **Scenario B** — register both, **load a `cuda-gpu` model first** (pulls `onnxruntime-genai-cuda.dll` into the process), then load `trtrtx-gpu` in the same process → **PASS → 'Paris', no PATH edits**. Conclusion: the defect is purely **DLL discovery/ordering** — the fix is to add the cuda-ep bundle dir to the DLL search path when the NvTensorRTRTX EP registers, so trtrtx loads out-of-box regardless of order. |

## Open blockers / triage log

Track every `fail` in a GA-blocking cell and every proposed `waived` here until resolved.

| id | cell_id | severity | owner | status (open/fixed-in-rcN/waived) | issue link | notes |
|----|---------|----------|-------|-----------------------------------|-----------|-------|
| F1 | js__pkg-inspect__macos-arm64__cpu | major | _TBD_ | **fixed on `main` — PR #971 "Add license to js sdk"** (not yet in RC) | _–_ | `foundry-local-sdk@2.0.0-rc1` ships **no license**: package.json has no `license` field AND the published tgz contains no LICENSE file (only package.json, README, dist, prebuilds), even though `files` lists `LICENSE`. Legal/compliance gap for a public release. Fix: add `"license": "MIT"` (or correct SPDX) and ensure LICENSE is packed. |
| F2 | js__tool-calling__macos-arm64__cpu | major (sample) | _TBD_ | **fixed (branch `fix/js-tool-calling-sample`)** | _–_ | **Stale sample, not an RC defect.** `samples/js/tool-calling-foundry-local` creates the manager with `serviceEndpoint: "http://localhost:5000"` and then calls `manager.startWebService()`; the RC correctly rejects that combination (`cannot start local web service when external_service_url is configured`). The JS web-server sample (uses `startWebService` without a serviceEndpoint) passes, confirming the runtime is fine. Fix the sample: drop `serviceEndpoint` (start a local service) or don't call `startWebService` (use the external endpoint). |
| F3 | cs__chat__macos-arm64__cpu | minor (env) | _TBD_ | open | _–_ | **Environment, not an RC defect.** The C# `native-chat-completions` sample **restores, builds, loads the native lib, and produces a correct chat completion** (verified by running the built binary directly — full "Chat completion response:" output on WebGPU). It targets `net9.0`; a machine with only the net10 shared runtime fails at launch until `DOTNET_ROLL_FORWARD=Major` (now set by the harness) or the net9.0 runtime is installed. Separately, the harness `dotnet run` path is intermittently very slow on this box (Azure-catalog region fetch), so the automated cs-chat cell is recorded as manually-verified rather than a harness JSON. Consider multi-targeting / documenting the runtime requirement for sample consumers. |
| F4 | cs__chat__macos-arm64__cpu | minor | _TBD_ | **fixed (branch `fix/cs-shutdown-mutex`)** | _–_ | On C# process shutdown (macos-arm64) the native lib logs `[*** LOG ERROR #0001 ***] [foundry_local] mutex lock failed: Invalid argument` after the model unloads. **Root cause:** when a caller never disposes `FoundryLocalManager`, the native `Manager` singleton (a namespace-scope `static unique_ptr`) is destroyed during C-runtime static destruction — after spdlog's shared console-sink / global-registry mutexes (lazily-constructed function-local statics, destroyed earlier) are already gone, so `~SpdlogLogger`'s `flush()`/`drop()` lock a destroyed mutex. The C# finalizer can't help (it skips native release once `Environment.HasShutdownStarted`). C#-only; Python/JS tear down deterministically. **Fix:** subscribe to `AppDomain.CurrentDomain.ProcessExit` in the manager ctor and Dispose there (idempotent, unsubscribes on Dispose), so `Manager::Destroy()` runs while the runtime is healthy. C# SDK builds clean. **On linux-x64 the same telemetry/log-manager teardown path escalates to a hard SIGSEGV — see F6.** |
| F5 | cs__chat__linux-x64__cpu | major (sample) | _TBD_ | fixed-in-branch | _–_ | **Linux-only sample defect (found + fixed in this validation branch).** All 13 C# samples carried a conditional `ItemGroup Condition="'$(RuntimeIdentifier)' == 'linux-x64'"` adding `PackageReference`s to `Microsoft.ML.OnnxRuntime.Gpu` + `Microsoft.ML.OnnxRuntimeGenAI.Cuda`. Under Central Package Management those need `PackageVersion` entries in `samples/cs/Directory.Packages.props`, which were absent → `NU1008`, build fails on every Linux box (`Directory.Build.props` auto-sets the linux-x64 RID, activating the group). **Correct fix (applied, per SDK owner):** those GPU-specialized packages are **not needed** — the `Microsoft.AI.Foundry.Local.Runtime` RC already depends on the **default** `Microsoft.ML.OnnxRuntime` 1.28.0 + `Microsoft.ML.OnnxRuntimeGenAI.Foundry` 0.15.2, and the GenAI.Foundry build loads the CUDA provider at runtime from CUDA libs on `LD_LIBRARY_PATH`. So the fix **removes the conditional GPU `ItemGroup` from all 13 csproj files** (and reverts the earlier `Directory.Packages.props` additions). Verified: samples restore/build with only the default packages and **run real CUDA models on the A100** (`qwen2.5-0.5b-instruct-cuda-gpu:4`) with the CUDA 12.8 runtime present. |
| F6 | cs__chat__linux-x64__cpu | **major** | _TBD_ | **fixed (branch `baijumeswani/f6-cs-shutdown-crash`)** | _–_ | **New linux-x64 RC defect: C# process SIGSEGVs (exit 139) at shutdown.** After correct inference and clean model unload, the process crashes in a C++ atexit static destructor: `~Manager` → `fl::OneDsTelemetry::~OneDsTelemetry()` → `CleanupLogManager` → `Microsoft::Applications::Events::LogManagerImpl::FlushAndTeardown()` → `PlatformAbstraction::PlatformAbstractionLayer::shutdown()` → **call to `0x0`** (null fn-ptr). Deterministic across chat/embeddings/web-server/model-mgmt. Output is correct (markers + sanity pass) so functional inference works, but every C# process that loads the SDK returns non-zero and crashes on exit → fails CI/exit-code contracts. **Not** seen from Python/JS (both exit 0 on Linux). Same telemetry/log-manager subsystem as **F4** (macOS cosmetic mutex log), escalated to a hard crash on Linux. **Root cause (confirmed):** the native `Manager` singleton (`fl::Manager::s_instance_`, a namespace-scope `static unique_ptr`) and its embedded 1DS `LogManager` are destroyed during C-runtime static destruction — *after* the 1DS SDK's own platform-abstraction globals are gone — so `FlushAndTeardown → PlatformAbstractionLayer::shutdown` calls a null fn-ptr. **C#-specific because** Python (`atexit.register(self.close)`) and JS (`process.on("exit"/"beforeExit")`) both register a process-exit hook that releases the native Manager while the runtime is healthy; C# `FoundryLocalManager` was `IDisposable` but had **no process-exit hook**, so callers that never `Dispose()` deferred teardown to static destruction. **Fix (applied):** subscribe to `AppDomain.CurrentDomain.ProcessExit` in the ctor and `Dispose()` there (runs native `Manager::Shutdown()/Destroy()` → `s_instance_.reset()` while healthy); handler unsubscribes at the start of `Dispose(bool)`, and `Dispose()` is idempotent (Interlocked-guarded) so explicit-Dispose + fallback can't double-free. **Verified linux-x64 A/B** with a no-`Dispose` repro against the source SDK (native unchanged): pre-fix control = exit **139** (SIGSEGV) ×3; post-fix = exit **0** ×3 (no-Dispose path *and* explicit-Dispose path). |
| F7 | python__ep-bootstrap__linux-x64__cuda | **major** | _TBD_ | **fixed (branch `baijumeswani/f7-cuda-graceful-degrade`)** | _–_ | **New linux-x64 defect: no graceful EP degradation.** On a box with an NVIDIA GPU but no CUDA 12 runtime, `download_and_register_eps()` **hard-raises** `FoundryLocalException: Some EPs failed to register` (root cause: the CUDA EP bundle links CUDA 12 — `libcudart.so.12`, `libcublasLt.so.12`, `libcublas.so.12`, `libcurand.so.10`, `libcufft.so.11` — which are absent when only the NVIDIA **driver** (CUDA 13) is installed). Because every sample calls `download_and_register_eps()` up front, this aborts **even CPU-only** workflows on any GPU box lacking the CUDA 12 runtime. Providing a CUDA 12 runtime on `LD_LIBRARY_PATH` — validated with **both** the `nvidia-*-cu12` pip wheels **and** a system CUDA 12.8 toolkit (`/datadisks/.../cuda12.8/lib64`, `libcudart.so.12`/`libcublasLt.so.12` etc.) — makes CUDA register **and run real A100 inference** (Python 'Paris' 0.41s; C# `qwen2.5-0.5b-instruct-cuda-gpu:4`). **Root cause (confirmed):** the EP detector intentionally reports `success=false` + `failed_eps=[CUDA]` when a bootstrapper can't load (unit-tested), but `Manager_DownloadAndRegisterEpsImpl` in `c_api.cc` turned any `!result.success` into `FOUNDRY_LOCAL_ERROR_INTERNAL`, which the C++ wrapper's `Check()` threw in **every** binding. CPU is always available (not a downloadable bootstrapper), so it was never a fallback. **Fix (applied):** at the C API boundary, only a user *cancellation* is an error; a failed optional-EP registration returns success and degrades to CPU. `manager.cc` logs a Warning naming the failed EPs; they stay observable via `GetDiscoverableEps` (`is_registered=false`). Detector semantics unchanged (unit tests untouched). **Verified linux-x64** (rebuilt `.so` in the RC Python venv): without CUDA 12 runtime, `download_and_register_eps()` no longer raises → logs graceful-degrade warning, CUDA `is_registered=false`, CPU inference returns 'Paris'; with CUDA 12.8 present, CUDA still registers (`is_registered=true`, no regression). |
| F8 | python__pkg-inspect__windows-x64__cpu | **major** | _TBD_ | open | _–_ | **New windows-x64 packaging defect: Python wheel ships a 111 MB debug PDB.** `foundry_local_sdk-2.0.0rc1-cp311-abi3-win_amd64.whl` bundles `foundry_local_sdk/_native/win-x64/foundry_local.pdb` (**111 MB — ~10× the 10.6 MB `foundry_local.dll`**), and the PDB embeds **561 unique absolute build-agent paths** (`D:\a\_work\1\s\...`). Two problems: (1) **packaging bloat** — the wheel is an order of magnitude larger than it needs to be for end users; (2) **information disclosure** — internal build-tree layout is shipped to every consumer. The cpp NuGet package does **not** ship a PDB (passes). `pkg-inspect` is a blocking cell → blocking FAIL on windows-x64. **Fix:** strip the PDB from the wheel (or ship it separately as a symbols package), matching the NuGet packaging. |
| F9 | python__ep-bootstrap__windows-x64__winml-dml | **major** | _TBD_ | open | _–_ | **windows-x64 WinML-EP path: catalog variants exist and NvTensorRTRTX runs, but neither WinML EP loads out-of-box.** *(Corrected after deeper investigation — supersedes the original "no WinML EP" claim, which was wrong.)* The RC **does** expose the WinML EPs: `discover_eps()` returns **OpenVINOExecutionProvider** (Intel) + **NvTensorRTRTXExecutionProvider** (NVIDIA) alongside CUDA + WebGpu; both register successfully. There is **no literal DirectML EP** — WinML delivers acceleration through these hardware-specific EPs, so `winml-dml` in the manifest maps to this WinML-EP infrastructure. The **catalog is EP-registration-filtered**: after `download_and_register_eps()` it publishes **16 `*-trtrtx-gpu` + 18 `*-openvino-gpu`** variants (invisible before registration — which is why an earlier enumeration wrongly reported none). Load/run reality on this box (RTX A2000 + Intel Iris Xe): **① NvTensorRTRTX runs REAL inference** — `qwen2.5-0.5b-instruct-trtrtx-gpu:2` → **'Paris'** (load 9.0 s / infer 1.58 s) — **but only when `onnxruntime-genai-cuda.dll` (shipped in the cuda-ep bundle) is on the DLL search path**; out-of-box the trtrtx load fails with `Cuda interface not available: onnxruntime-genai-cuda.dll missing (Error 126)`. This is a **native-loader discovery gap** (the TRT-RTX GenAI path needs the CUDA GenAI interop DLL but its registration doesn't add the cuda-ep bundle dir to the search path — cf. `ort-loading-contract`), **not** a missing artifact. **② OpenVINO fails** at generator creation on the Intel iGPU: `allocator != nullptr was false — Failed to find allocator for device VendorId:32902 (Intel)`. **③ Load-order nuance (verified):** merely *registering* the CUDA EP alongside NvTensorRTRTX is **not** sufficient — trtrtx still fails (`onnxruntime-genai-cuda.dll missing`) because EP registration doesn't load that DLL into the process. But **loading any CUDA model first** (e.g. `cuda-gpu`, which pulls `onnxruntime-genai-cuda.dll` into the process address space) makes the subsequent trtrtx load succeed → **'Paris'**, with **no PATH edits**. So the defect is purely **DLL discovery/ordering**: the TRT-RTX GenAI path needs `onnxruntime-genai-cuda.dll` already-loaded or on the search path. **What works out-of-box: CPU, CUDA, WebGPU** (all real-inference verified → 'Paris'). **Action:** (a) fix the TRT-RTX loader path so `onnxruntime-genai-cuda.dll` (from the cuda-ep bundle) is added to the DLL search directory when the NvTensorRTRTX EP registers — then trtrtx works out-of-box regardless of load order (cf. `ort-loading-contract`); (b) triage the OpenVINO Intel-iGPU allocator failure; (c) reconcile `platform_manifest` — CUDA + WebGPU are the working GPU paths, TRT-RTX works with the loader fix, OpenVINO is currently broken on Intel here. Recorded **fail** because neither WinML variant loads out-of-box. |
| F10 | cs__tool-calling__windows-x64__cpu | major (sample) | _TBD_ | open | _–_ | **Stale C# sample won't compile against the RC (also hits vision).** `samples/cs/tool-calling-foundry-local-sdk/Program.cs` imports both `using Microsoft.AI.Foundry.Local;` and `using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;` and references `ToolChoice.Required`/`.Auto` **unqualified** → `error CS0104: 'ToolChoice' is an ambiguous reference` (the RC added its own `Microsoft.AI.Foundry.Local.ToolChoice`, which now collides with the OpenAI lib's `ToolChoice`). Build fails → cell fails before any runtime. **Not an RC runtime defect** — an API-surface *addition* that broke a stale sample. **Fix the sample:** fully-qualify the intended `ToolChoice` or add a `using ToolChoice = ...;` alias. The C# vision cell fails the same build class. |
| F6 (win) | cs__chat__windows-x64__cpu | **major** | _TBD_ | **fixed on `main` — PR #974 "C#: Automatically dispose the manager on process exit"** (confirmed merged to `main`, **not** in RC) | _–_ | **F6 reproduced on windows-x64.** All five C# run cells (chat/embeddings/audio-file/web-server/model-mgmt) complete inference correctly (every functional marker + sanity assertion passes; the sample auto-selects the `cuda-gpu` variant on this GPU box and unloads cleanly) but the process then **exits `3221226324` = `0xC0000374` = STATUS_HEAP_CORRUPTION** at teardown — the Windows manifestation of the Linux exit-139 SIGSEGV in the same 1DS telemetry / `Manager` static-destruction path. Same root cause and same fix as F6 (subscribe to `AppDomain.ProcessExit` → Dispose while runtime healthy); the fix branch predates this run so the shipped RC still crashes here. Blocking on all C# cells (exit-code contract). |
| N1 | (informational) | minor | _TBD_ | **fixed (branch `fix/python-version-metadata`)** | _–_ | Python `foundry_local_sdk.__version__` reported `0.1.0` (hardcoded) while pip metadata was `2.0.0rc1`. Fixed: `version.py` now derives `__version__` from `importlib.metadata.version("foundry-local-sdk")` with a `2.0.0.dev0` source-tree fallback. Validated both paths. (Also reproduced on linux-x64.) |
| N2 | (informational) | minor | _TBD_ | **parked (redesign, post-release)** | _–_ | Native chat/audio/embedding clients (`get_chat_client`/`get_audio_client`/`get_embedding_client` and C# equivalents) are `@deprecated` in favor of `ChatSession`/`AudioSession`/`EmbeddingsSession` (removal end-of-2026), yet ~25 Python + C# samples still use them. Scope is far larger than a rename — the Session API is a different programming model (`Request().add_item(MessageItem.user(...))` + `session.process_request(...)`). **Decision (owner):** the top-level samples were written for SDK v1 and should be **redesigned** to showcase the v2 recommended flow, not patch-fixed; parked for a dedicated redesign after the release. Deprecated path still works for 2.0.0. |
| N3 | (informational) | minor | _TBD_ | **fixed (branch `fix/sample-cleanups`)** | _–_ | Stale model aliases in sample sources vs the RC catalog. Fixed the vision example alias `Qwen2.5-VL-7B-Instruct-generic-cpu` → `qwen3-vl-2b-instruct-generic-cpu` in the cs/js/python/rust web-server responses-vision samples (validated alias; `qwen3-vl-2b-instruct` passed on macOS). The chat-large `deepseek-r1-distill-qwen-14b` alias does not appear in any sample source (harness manifest already refreshed). |
| N7 | (sample, C++) | major (sample) | _TBD_ | **fixed (branch `fix/sample-cleanups`)** | _–_ | **Stale C++ sample would not compile against the RC.** `samples/cpp/live-audio-transcription/main.cpp` called `DownloadAndRegisterEps(nullptr, cancelLambda)` with `nullptr` for the `const std::vector<std::string>&` names and a zero-arg `bool()` cancellation lambda (signature needs `std::function<bool(std::string_view, float)>`), and called `Model::Download(progress, cancel)` with **two** args though `Download` takes a single `std::function<int(float)>` — with the progress lambda returning `true` (=1=cancel under the int convention). Rewrote both callbacks to the current signatures, honoring Ctrl+C via `g_running` (EP: return true to continue; model download: return 0 to continue). |
| N8 | (SDK API, informational) | minor | _TBD_ | open | _–_ | **Inverted return-value semantics between the two C++ progress callbacks** (surfaced while fixing N7). `Manager::DownloadAndRegisterEps` progress is `std::function<bool(ep_name, percent)>` where **true = continue / false = cancel**, but `Model::Download` progress is `std::function<int(float)>` where **0 = continue / non-zero = cancel** — opposite conventions in the same SDK layer. This directly caused the N7 sample bug (author wrote `return true` in the model-download callback expecting "continue"). Consider harmonizing the C++ high-level API (e.g. make model download progress `bool(float)` with true=continue) in a follow-up — this is an API decision for the release owners, deliberately **not** changed inside a sample PR. |
| N4 | (soak, macos-arm64/cpu) | minor | _TBD_ | open | _–_ | **Repeated load/unload does not return model pages to the OS and shows a small steady-state RSS drift.** Over 16–20 `load→infer→unload` cycles of `qwen2.5-0.5b` (837MB), each `unload()` frees only ~90MB of the ~1.2GB resident; RSS climbs from ~1.2GB and asymptotes near ~1.75–1.87GB, with early growth ~130MB/cycle decaying to ~10–15MB/cycle steady-state. This is **bounded/plateauing, not an unbounded per-model leak** (a true leak would add ~model-size every cycle → tens of GB), and sustained inference on a loaded model is flat (+20MB over 20 calls). Still worth a native-side look before GA: `unload()` should ideally release/munmap weights, and the residual steady drift should be characterized. Gate passes (steady slope < 25MB/cycle). |
| N5 | (informational) | minor | _TBD_ | open | _–_ | `IModel.is_cached` is a **presence check, not an integrity check**: after deleting a required weight file (`model.onnx.data`) from a cached model, `is_cached` still returns `True`; the corruption is only surfaced (correctly, as a typed `FoundryLocalException`) at `load()`. Consider validating manifest/file completeness in `is_cached`, or documenting that callers must be prepared for a typed load error even when `is_cached` is true. |
| N6 | (compat, informational) | minor | _TBD_ | open | _–_ | 1.x→2.0 **source/API compat is strong** (N.B. `scripts/compat_surface_check.py` passes 4/4 against 1.2.4), but two upgrade caveats remain fleet/doc work: (1) `foundry_local_sdk.openai` is still importable but is no longer eager-exposed on the top-level namespace, so code relying on bare `foundry_local_sdk.openai.X` without an explicit `import foundry_local_sdk.openai` must add the import; (2) full **runtime** upgrade validation (rebuild an existing 1.x app end-to-end, model-cache/config reuse & migration, uninstall/reinstall, side-by-side) was **not** exercised here — 1.x's service-based runtime differs from 2.0's in-process runtime and needs the legacy Foundry service on target platforms. API-diff slice done; runtime-upgrade slice deferred to the provisioned agents. |

### Feed-access note (validated 2026-08-10, macos-arm64)

The ORT-Nightly ADO feed is anonymous for its **own** packages, but its **upstream proxy
requires authentication to save an upstream package on first fetch** — so anonymous installs
that pull ANY transitive dep (cffi, node-addon-api, Microsoft.ML.OnnxRuntime, Betalgo.Ranul.OpenAI,
Microsoft.Extensions.Logging) fail with 401/E401. The harness therefore uses a **two-source
model**: RC package from the ORT feed + all transitive deps from a separate deps source
(public registry, or a mirror on locked-down machines). With that, **all four RC packages
install & import/build cleanly on macos-arm64**. Earlier `adm-zip@^0.6.0` "missing" was a stale
upstream cache (adm-zip 0.6.0 exists on public npmjs) — NOT an RC bug.

### linux-x64 divergences from the macOS run (validated 2026-08-10, onnxr4c75000005, A100×8)

Every macOS finding was treated as a hypothesis and independently re-checked on Linux. Deltas:

- **Accelerator set:** the linux-x64 box publishes **CPU + CUDA** variants for `qwen2.5-0.5b`, not
  the CPU + WebGPU pair seen on macOS. **No WebGPU variant is published for linux-x64** (WebGPU is
  blocked/experimental on Linux) — so `python__chat__linux-x64__webgpu` is off-surface here, and the
  original `model_mgmt_check.py` WebGPU-only assertion was made platform-portable (accepts any GPU EP).
- **F4 → F6 escalation:** the macOS cosmetic C# shutdown mutex-log (**F4**) becomes a **hard SIGSEGV
  (exit 139)** on linux-x64 (**F6**) in the same 1DS telemetry / log-manager teardown path.
- **Soak (refutes concern in N4 for Linux):** load/unload RSS on Linux **plateaus lower/faster** than
  macOS — ~619MB steady-state vs macOS's ~1.75GB drift; sustained inference flat (+29MB/50 calls in a
  clean isolated process). No leak; Linux is **better** than the macOS observation.
- **New linux-only findings:** **F5** (C# samples unbuildable — CPM missing GPU PackageVersion; fixed
  in branch), **F6** (C# shutdown SIGSEGV — root-caused + fixed in branch
  `baijumeswani/f6-cs-shutdown-crash`), **F7** (`download_and_register_eps()` hard-fails on GPU
  boxes without the CUDA 12 runtime instead of degrading to CPU — root-caused + fixed in branch
  `baijumeswani/f7-cuda-graceful-degrade`).
- **Reproduced on Linux (not divergent):** **F1** (JS package ships no license), **F2** (JS
  tool-calling stale sample), **N1** (`__version__`=0.1.0), **N2** (C# `GetChatClientAsync`
  deprecation). Python tool-calling **passes** on Linux (F2 is JS-only).

### windows-x64 divergences from the macOS / linux-x64 runs (validated 2026-08-11, DESKTOP-4T800KF, RTX A2000)

Every prior finding was treated as a hypothesis and independently re-checked on this windows-x64 box
(Win11, Python 3.11.9, .NET SDK 9.0.316 → net9.0 runtime present, Node 24, NVIDIA RTX A2000 Laptop
4 GB + Intel Iris Xe, CUDA 12.8 EP bundle). Deltas:

- **Accelerator set:** the RC exposes **CPU + CUDA + WebGPU** (all real-inference verified → 'Paris')
  **plus the WinML EPs** OpenVINO (Intel) + NvTensorRTRTX (NVIDIA) — there is **no literal DirectML
  EP**; WinML delivers acceleration through these hardware EPs. The catalog is **EP-registration-
  filtered**: `download_and_register_eps()` reveals **16 `*-trtrtx-gpu` + 18 `*-openvino-gpu`**
  variants. **NvTensorRTRTX runs real inference** (`qwen2.5-0.5b-instruct-trtrtx-gpu:2` → 'Paris')
  **but only with `onnxruntime-genai-cuda.dll` on the DLL search path** — out-of-box its load fails
  (loader gap); **OpenVINO fails** on the Intel iGPU allocator (**F9**, corrected). So windows-x64 is
  the only platform verifying CUDA **and** WebGPU (and, with a loader fix, TensorRT-RTX) inference;
  macOS was CPU+WebGPU, linux-x64 CPU+CUDA.
- **F6 escalation (Linux SIGSEGV → Windows heap-corruption):** the C# teardown crash reproduces on
  windows-x64 as **exit `0xC0000374` (STATUS_HEAP_CORRUPTION)** after correct inference — same 1DS
  telemetry / `Manager` static-destruction root cause; fix branch `baijumeswani/f6-cs-shutdown-crash`
  applies but predates the shipped RC, so it still crashes here (**F6 (win)**).
- **Soak (best of the three platforms):** load/unload plateaus at **~481 MB** (0.2 MB/cycle) vs
  macOS ~1.75 GB and linux ~619 MB. The one alarming raw number — sustained-inference RSS +770 MB —
  was characterized with `mem_char.py`: both WorkingSet and Private bytes **plateau after ~10 infers**
  (slope 0.01–0.02 MB/infer), i.e. one-time arena/KV warm-up, **no unbounded leak**. Refutes any
  windows leak concern.
- **New windows-only findings:** **F8** (Python wheel ships a **111 MB `foundry_local.pdb`** with 561
  embedded build-agent paths — packaging bloat + info-disclosure; NuGet ships no PDB), **F9**
  (WinML-EP path — OpenVINO + NvTensorRTRTX EPs and their 18+16 catalog variants exist; NvTensorRTRTX
  runs real inference but only with the genai-cuda DLL on-path, OpenVINO fails on the Intel iGPU),
  **F10** (C# tool-calling/vision sample won't compile — `ToolChoice` ambiguous between the RC's new
  type and the Betalgo OpenAI lib).
- **Upstream-fix confirmations (checked against `origin/main`, 2026-08-11):** **F1** is fixed by
  **PR #971 "Add license to js sdk"** — `main:sdk_v2/js/package.json` now has `"license": "MIT"` and a
  tracked `sdk_v2/js/LICENSE` (both absent on the RC branch). **F6** is fixed by **PR #974 "C#:
  Automatically dispose the manager on process exit"** — `main:sdk_v2/cs/src/FoundryLocalManager.cs`
  adds the `AppDomain.CurrentDomain.ProcessExit` hook (subscribe in ctor, unsubscribe in `Dispose`),
  exactly the root-cause fix. **Neither PR is in `releases/rel-2.0.0`/the RC**, so the shipped RC
  still fails these cells on windows-x64 — they must be cherry-picked into the release for GA.
- **Reproduced on Windows (not divergent):** **F1** (JS package ships no license), **F2** (JS
  tool-calling stale sample; Python tool-calling **passes**, consistent with Linux), **F6** (C#
  shutdown crash, see above). A **web-server sample** print crashed on the cp1252 Windows console
  (`UnicodeEncodeError` on a streamed `Φ`) — **sample bug, not RC**: the RC web server loaded the
  model and streamed correctly; forcing `PYTHONIOENCODING=utf-8` (now done by the harness for all
  child samples) makes it PASS.
- **Harness portability fixes required on Windows (committed):** map `npm`→`npm.cmd` via
  `shutil.which`; force UTF-8 child stdio (`PYTHONIOENCODING`/`PYTHONUTF8`) so streamed Unicode
  doesn't crash on legacy code pages; cross-platform `rss_mb()` (ctypes `GetProcessMemoryInfo` on
  Windows); cross-platform read-only-cache scenario (`icacls` ACL instead of `chmod`).


## Waivers (approved known issues)

| cell_id | issue link | owner | user-facing note / workaround |
|---------|-----------|-------|-------------------------------|
| _–_ | _–_ | _–_ | _–_ |

## Final go/no-go

Filled in by the release lead once required coverage is complete (see
[`ACCEPTANCE_POLICY.md`](ACCEPTANCE_POLICY.md) → "Go / No-Go decision").

- Verdict: **TBD**
- Date / lead: **TBD**
- Rationale / evidence: link to `COMBINED_CHECKLIST.md` + committed results files.

### Independent linux-x64 verdict (agent, 2026-08-10)

**Verdict for linux-x64: NO-GO** (per [`ACCEPTANCE_POLICY.md`](ACCEPTANCE_POLICY.md); un-waived
GA-blocking cells remain non-pass).

- **What passes:** install-smoke for all 4 SDKs; pkg-inspect for cpp/cs/python (linux-x64
  `libfoundry_local.so` + LICENSE packaged, native loads via `$ORIGIN` RPATH with ORT→GenAI
  preload); **all 7 Python CPU features**; **6/7 JS CPU features**; every direct-API script
  (model-mgmt 10/10, crosscutting 8/8, compat-surface 4/4, model-mgmt-fail 7/7, soak); and **real
  CUDA A100 inference** from Python (and from C# at runtime). CPU + CUDA inference paths are
  functionally sound.
- **What blocks GA (un-waived, blocking cells):**
  - **F6** — C# process **SIGSEGVs (exit 139) on every shutdown** (1DS telemetry teardown). Output
    is correct, but no C# app that loads the SDK can exit cleanly → fails exit-code/CI contracts
    on all four C# cells. Primary linux-x64 GA blocker. **Root-caused + fixed in branch
    `baijumeswani/f6-cs-shutdown-crash`** (ProcessExit → Dispose; A/B verified exit 139 → exit 0).
  - **F1** — JS package ships **no license** (legal blocker; blocking `js__pkg-inspect` cell).
  - **F7** — `download_and_register_eps()` **hard-raises** on GPU boxes without the CUDA 12 runtime,
    aborting even CPU-only workflows. Major reliability blocker for GPU hosts. **Root-caused +
    fixed in branch `baijumeswani/f7-cuda-graceful-degrade`** (C-API degrades to CPU; verified).
- **Sample-level (not RC blockers, should still ship fixes):** **F5** (C# CPM missing GPU
  PackageVersion — samples unbuildable on Linux; **fixed in this branch**), **F2** (JS tool-calling
  stale sample).
- **Bottom line:** the core CPU/CUDA runtime works on linux-x64. As-shipped, the RC is **NO-GO**
  (**F6 + F1 + F7** are un-waived blocking-cell failures). **F6 and F7 now have verified fixes in
  dedicated branches** (`baijumeswani/f6-cs-shutdown-crash`, `baijumeswani/f7-cuda-graceful-degrade`);
  once those land plus **F1** (JS license) is resolved (or the C#/JS claims are scoped out for this
  platform), the linux-x64 surface would be a **GO**.

### Independent windows-x64 verdict (agent, 2026-08-11)

**Verdict for windows-x64: NO-GO** (per [`ACCEPTANCE_POLICY.md`](ACCEPTANCE_POLICY.md); un-waived
GA-blocking cells remain non-pass). Box: DESKTOP-4T800KF, NVIDIA RTX A2000 Laptop + Intel Iris Xe,
Win11, .NET SDK 9 (net9.0 runtime present — no roll-forward needed).

- **What passes:** install-smoke for all 4 SDKs (two-source install via ORT feed + corporate
  package mirrors); pkg-inspect for **cpp/cs** (win-x64 `foundry_local.dll` + LICENSE packaged, native
  loads via `DllLoader` with ORT→GenAI preload); **all 7 Python CPU features**; **6/7 JS CPU
  features**; every direct-API script (model-mgmt 10/10, crosscutting 8/8, model-mgmt-fail 7/7,
  compat-surface 4/4, soak — plateaus at ~481 MB, the **best** of the three platforms, `mem_char.py`
  confirms **no leak**); and **REAL CUDA *and* REAL WebGPU inference** (both → 'Paris'). windows-x64
  is the only platform where both CUDA and WebGPU inference are independently verified. The core
  **CPU + CUDA + WebGPU** runtime is functionally sound.
- **What blocks GA (un-waived, blocking cells):**
  - **F8** — the **Python wheel ships a 111 MB `foundry_local.pdb`** (≈10× the DLL) embedding 561
    build-agent paths → blocking `python__pkg-inspect` FAIL (packaging bloat + info-disclosure).
    Windows-specific; NuGet ships no PDB. Strip the PDB or ship it as a separate symbols package.
  - **F6 (win)** — every **C# process crashes at teardown with `0xC0000374` (heap corruption)** after
    correct inference → all C# cells fail the exit-code contract. Windows manifestation of the Linux
    F6 SIGSEGV. **Fixed upstream** by **PR #974** (`AppDomain.ProcessExit`→Dispose) on `main` — but
    **not in the RC/release branch**, so the shipped RC still crashes.
  - **F1** — JS package ships **no license** (legal blocker; blocking `js__pkg-inspect` cell) —
    reproduced on Windows. **Fixed upstream** by **PR #971** on `main` (adds `"license":"MIT"` +
    LICENSE) — **not in the RC**.
  - **F9** — the **WinML EP path doesn't load out-of-box**: OpenVINO + NvTensorRTRTX EPs and their
    18 + 16 catalog variants exist and register; **NvTensorRTRTX runs real inference** ('Paris') **but
    only** with `onnxruntime-genai-cuda.dll` on the DLL search path (out-of-box load fails — a
    native-loader gap), and **OpenVINO fails** on the Intel iGPU allocator. GPU acceleration is **not**
    blocked overall — **CPU + CUDA + WebGPU all run out-of-box** — but the WinML/TensorRT-RTX/OpenVINO
    accelerator claims need the loader fix + OpenVINO triage.
- **Sample-level (not RC runtime blockers, should still ship fixes):** **F10** (C# tool-calling/vision
  won't compile — `ToolChoice` ambiguous vs the Betalgo OpenAI lib), **F2** (JS tool-calling stale
  sample), and the Python **web-server** cp1252 print crash (sample bug; RC web server works — harness
  now forces UTF-8 child stdio).
- **Bottom line:** the core **CPU / CUDA / WebGPU** runtime is **functionally sound** on windows-x64
  (real inference verified on all three paths, direct-API suites green, memory bounded); the WinML
  **TensorRT-RTX** path also runs real inference once its genai-cuda DLL is discoverable. As-shipped
  the RC is **NO-GO** due to un-waived blocking-cell failures — **F8** (Python PDB packaging), **F6
  (win)** (C# heap-corruption at exit), **F1** (JS license), and **F9** (WinML EPs don't load
  out-of-box). **F1 and F6 already have merged upstream fixes on `main` (PR #971, PR #974) that are
  not yet in the release branch — cherry-pick them into `releases/rel-2.0.0` for GA.** F8 is a
  packaging fix; F9 needs the TRT-RTX loader-path fix + OpenVINO Intel-iGPU triage. Once those land
  (or the affected SDK/accelerator claims are scoped out), the windows-x64 surface would be a **GO**.
