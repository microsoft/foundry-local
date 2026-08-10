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
| _e.g. 2026-08-11_ | _@you / linux-cuda-agent_ | _linux-x64_ | _x64_ | _cpu, cuda_ | _`results/host__id.json`_ | _–_ | _–_ | _–_ | _–_ |

## Open blockers / triage log

Track every `fail` in a GA-blocking cell and every proposed `waived` here until resolved.

| id | cell_id | severity | owner | status (open/fixed-in-rcN/waived) | issue link | notes |
|----|---------|----------|-------|-----------------------------------|-----------|-------|
| F1 | js__pkg-inspect__macos-arm64__cpu | major | _TBD_ | open | _–_ | `foundry-local-sdk@2.0.0-rc1` ships **no license**: package.json has no `license` field AND the published tgz contains no LICENSE file (only package.json, README, dist, prebuilds), even though `files` lists `LICENSE`. Legal/compliance gap for a public release. Fix: add `"license": "MIT"` (or correct SPDX) and ensure LICENSE is packed. |

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
