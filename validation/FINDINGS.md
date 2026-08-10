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
