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
| 2026-08-10 | @baijumeswani / onnxr4c75000005 | linux-x64 | x64 | cpu, cuda | `results/onnxr4c75000005__linux-csfeat.json` | 0 | 4 | 0 | C# chat/embeddings/web-server/model-mgmt: sample build **fixed** (**F5**, CPM missing PackageVersion for linux-GPU pkgs) → then produce **correct** inference output (auto-selects CUDA A100 variant), but the process **SIGSEGVs (exit 139) at shutdown** in 1DS telemetry teardown (**F6**, new linux-x64 RC defect). Runtime functionally verified; exit-code assertion fails. |
| 2026-08-10 | @baijumeswani / onnxr4c75000005 | linux-x64 | x64 | cpu, cuda | `results/onnxr4c75000005__linux-direct.json` | 8 | 0 | 0 | Direct-API + CUDA: model-mgmt 10/10, crosscutting 8/8, compat-surface 4/4, model-mgmt-fail 7/7, soak (bounded/flat — see below), ep-bootstrap cpu+cuda, and **REAL CUDA chat on A100** ('Paris' 0.38s, load 1.2s). CUDA needs user-supplied CUDA 12 runtime (**F7**). WebGPU: no linux-x64 variant published (blocked/experimental). |

## Open blockers / triage log

Track every `fail` in a GA-blocking cell and every proposed `waived` here until resolved.

| id | cell_id | severity | owner | status (open/fixed-in-rcN/waived) | issue link | notes |
|----|---------|----------|-------|-----------------------------------|-----------|-------|
| F1 | js__pkg-inspect__macos-arm64__cpu | major | _TBD_ | **fixed (branch `fix/js-package-license`)** | _–_ | `foundry-local-sdk@2.0.0-rc1` ships **no license**: package.json has no `license` field AND the published tgz contains no LICENSE file (only package.json, README, dist, prebuilds), even though `files` lists `LICENSE`. Legal/compliance gap for a public release. Fix: add `"license": "MIT"` (or correct SPDX) and ensure LICENSE is packed. |
| F2 | js__tool-calling__macos-arm64__cpu | major (sample) | _TBD_ | **fixed (branch `fix/js-tool-calling-sample`)** | _–_ | **Stale sample, not an RC defect.** `samples/js/tool-calling-foundry-local` creates the manager with `serviceEndpoint: "http://localhost:5000"` and then calls `manager.startWebService()`; the RC correctly rejects that combination (`cannot start local web service when external_service_url is configured`). The JS web-server sample (uses `startWebService` without a serviceEndpoint) passes, confirming the runtime is fine. Fix the sample: drop `serviceEndpoint` (start a local service) or don't call `startWebService` (use the external endpoint). |
| F3 | cs__chat__macos-arm64__cpu | minor (env) | _TBD_ | open | _–_ | **Environment, not an RC defect.** The C# `native-chat-completions` sample **restores, builds, loads the native lib, and produces a correct chat completion** (verified by running the built binary directly — full "Chat completion response:" output on WebGPU). It targets `net9.0`; a machine with only the net10 shared runtime fails at launch until `DOTNET_ROLL_FORWARD=Major` (now set by the harness) or the net9.0 runtime is installed. Separately, the harness `dotnet run` path is intermittently very slow on this box (Azure-catalog region fetch), so the automated cs-chat cell is recorded as manually-verified rather than a harness JSON. Consider multi-targeting / documenting the runtime requirement for sample consumers. |
| F4 | cs__chat__macos-arm64__cpu | minor | _TBD_ | **fixed (branch `fix/cs-shutdown-mutex`)** | _–_ | On C# process shutdown (macos-arm64) the native lib logs `[*** LOG ERROR #0001 ***] [foundry_local] mutex lock failed: Invalid argument` after the model unloads. **Root cause:** when a caller never disposes `FoundryLocalManager`, the native `Manager` singleton (a namespace-scope `static unique_ptr`) is destroyed during C-runtime static destruction — after spdlog's shared console-sink / global-registry mutexes (lazily-constructed function-local statics, destroyed earlier) are already gone, so `~SpdlogLogger`'s `flush()`/`drop()` lock a destroyed mutex. The C# finalizer can't help (it skips native release once `Environment.HasShutdownStarted`). C#-only; Python/JS tear down deterministically. **Fix:** subscribe to `AppDomain.CurrentDomain.ProcessExit` in the manager ctor and Dispose there (idempotent, unsubscribes on Dispose), so `Manager::Destroy()` runs while the runtime is healthy. C# SDK builds clean. **On linux-x64 the same telemetry/log-manager teardown path escalates to a hard SIGSEGV — see F6.** |
| F5 | cs__chat__linux-x64__cpu | major (sample) | _TBD_ | fixed-in-branch | _–_ | **Linux-only sample defect (found + fixed in this validation branch).** The C# samples use Central Package Management; `samples/cs/Directory.Build.props` auto-sets `RuntimeIdentifier=linux-x64`, which activates a conditional `ItemGroup` referencing `Microsoft.ML.OnnxRuntime.Gpu` + `Microsoft.ML.OnnxRuntimeGenAI.Cuda` — but `samples/cs/Directory.Packages.props` had **no `PackageVersion`** entries for them → `NU1008`, build fails on every Linux box. Fix (applied): add `PackageVersion` for both (1.28.0 / 0.15.2). Ship this fix or the C# samples are unbuildable on Linux. |
| F6 | cs__chat__linux-x64__cpu | **major** | _TBD_ | open | _–_ | **New linux-x64 RC defect: C# process SIGSEGVs (exit 139) at shutdown.** After correct inference and clean model unload, the process crashes in a C++ atexit static destructor: `~Manager` → `fl::OneDsTelemetry::~OneDsTelemetry()` → `CleanupLogManager` → `Microsoft::Applications::Events::LogManagerImpl::FlushAndTeardown()` → `PlatformAbstraction::PlatformAbstractionLayer::shutdown()` → **call to `0x0`** (null fn-ptr). Deterministic across chat/embeddings/web-server/model-mgmt. Output is correct (markers + sanity pass) so functional inference works, but every C# process that loads the SDK returns non-zero and crashes on exit → fails CI/exit-code contracts. **Not** seen from Python/JS (both exit 0 on Linux). Same telemetry/log-manager subsystem as **F4** (macOS cosmetic mutex log), escalated to a hard crash on Linux. Recommend GA-blocker triage for the C# linux-x64 claim. |
| F7 | python__ep-bootstrap__linux-x64__cuda | **major** | _TBD_ | open | _–_ | **New linux-x64 defect: no graceful EP degradation.** On a box with an NVIDIA GPU but no CUDA 12 runtime, `download_and_register_eps()` **hard-raises** `FoundryLocalException: Some EPs failed to register` (root cause: the CUDA EP bundle links CUDA 12 — `libcudart.so.12`, `libcublasLt.so.12`, `libcublas.so.12`, `libcurand.so.10`, `libcufft.so.11` — which are absent when only the NVIDIA **driver** (CUDA 13) is installed). Because every sample calls `download_and_register_eps()` up front, this aborts **even CPU-only** workflows on any GPU box lacking the CUDA 12 runtime. Providing the CUDA 12 runtime (`nvidia-*-cu12` wheels on `LD_LIBRARY_PATH`) makes CUDA register **and run real A100 inference** ('Paris' 0.38s). Fix: skip/deregister an EP that fails to load and continue (degrade to CPU) instead of raising; and/or document the CUDA 12 runtime prerequisite. |
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
  in branch), **F6** (C# shutdown SIGSEGV), **F7** (`download_and_register_eps()` hard-fails on GPU
  boxes without the CUDA 12 runtime instead of degrading to CPU).
- **Reproduced on Linux (not divergent):** **F1** (JS package ships no license), **F2** (JS
  tool-calling stale sample), **N1** (`__version__`=0.1.0), **N2** (C# `GetChatClientAsync`
  deprecation). Python tool-calling **passes** on Linux (F2 is JS-only).


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
    on all four C# cells. Primary linux-x64 GA blocker.
  - **F1** — JS package ships **no license** (legal blocker; blocking `js__pkg-inspect` cell).
  - **F7** — `download_and_register_eps()` **hard-raises** on GPU boxes without the CUDA 12 runtime,
    aborting even CPU-only workflows. Major reliability blocker for GPU hosts.
- **Sample-level (not RC blockers, should still ship fixes):** **F5** (C# CPM missing GPU
  PackageVersion — samples unbuildable on Linux; **fixed in this branch**), **F2** (JS tool-calling
  stale sample).
- **Bottom line:** the core CPU/CUDA runtime works on linux-x64, but **F6 + F1 + F7** are un-waived
  blocking-cell failures → **NO-GO** until fixed or formally waived. If F6/F1/F7 are resolved (or
  the C# and JS claims are scoped out for this platform), the Python/CUDA surface would be a GO.
