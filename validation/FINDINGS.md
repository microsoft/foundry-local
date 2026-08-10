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

## Open blockers / triage log

Track every `fail` in a GA-blocking cell and every proposed `waived` here until resolved.

| id | cell_id | severity | owner | status (open/fixed-in-rcN/waived) | issue link | notes |
|----|---------|----------|-------|-----------------------------------|-----------|-------|
| F1 | js__pkg-inspect__macos-arm64__cpu | major | _TBD_ | open | _–_ | `foundry-local-sdk@2.0.0-rc1` ships **no license**: package.json has no `license` field AND the published tgz contains no LICENSE file (only package.json, README, dist, prebuilds), even though `files` lists `LICENSE`. Legal/compliance gap for a public release. Fix: add `"license": "MIT"` (or correct SPDX) and ensure LICENSE is packed. |
| F2 | js__tool-calling__macos-arm64__cpu | major (sample) | _TBD_ | open | _–_ | **Stale sample, not an RC defect.** `samples/js/tool-calling-foundry-local` creates the manager with `serviceEndpoint: "http://localhost:5000"` and then calls `manager.startWebService()`; the RC correctly rejects that combination (`cannot start local web service when external_service_url is configured`). The JS web-server sample (uses `startWebService` without a serviceEndpoint) passes, confirming the runtime is fine. Fix the sample: drop `serviceEndpoint` (start a local service) or don't call `startWebService` (use the external endpoint). |
| F3 | cs__chat__macos-arm64__cpu | minor (env) | _TBD_ | open | _–_ | **Environment, not an RC defect.** The C# `native-chat-completions` sample **restores, builds, loads the native lib, and produces a correct chat completion** (verified by running the built binary directly — full "Chat completion response:" output on WebGPU). It targets `net9.0`; a machine with only the net10 shared runtime fails at launch until `DOTNET_ROLL_FORWARD=Major` (now set by the harness) or the net9.0 runtime is installed. Separately, the harness `dotnet run` path is intermittently very slow on this box (Azure-catalog region fetch), so the automated cs-chat cell is recorded as manually-verified rather than a harness JSON. Consider multi-targeting / documenting the runtime requirement for sample consumers. |
| F4 | cs__chat__macos-arm64__cpu | minor | _TBD_ | open | _–_ | On C# process shutdown (macos-arm64) the native lib logs `[*** LOG ERROR #0001 ***] [foundry_local] mutex lock failed: Invalid argument` after the model unloads. Looks like a shutdown-ordering issue (logging after a logger/mutex is torn down). Cosmetic at runtime but should be fixed to avoid alarming users and to keep clean shutdown. Not observed from Python/JS. |
| N1 | (informational) | minor | _TBD_ | open | _–_ | Python `foundry_local_sdk.__version__` reports `0.1.0` while the pip package metadata is `2.0.0rc1`. In-code `__version__` is not wired to the package version. Cosmetic but confusing for users who print it. |
| N2 | (informational) | minor | _TBD_ | open | _–_ | Native chat client is deprecated: Python `model.get_chat_client()` and C# `IModel.GetChatClientAsync` emit deprecation warnings ("use ChatSession"/"use new ChatSession(model)"), yet the shipped native-chat samples still use the deprecated API. Update samples to `ChatSession` before GA. |
| N3 | (informational) | minor | _TBD_ | open | _–_ | Stale model aliases in samples/manifests vs the RC catalog: vision `Qwen2.5-VL-7B-Instruct` → now `qwen3-vl-2b/4b/8b-instruct`; chat-large `deepseek-r1-distill-qwen-14b` → now `deepseek-r1-14b`/`deepseek-r1-7b`. Harness manifest updated to valid aliases; the vision **sample source** still hardcodes the old alias and should be refreshed. |
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
