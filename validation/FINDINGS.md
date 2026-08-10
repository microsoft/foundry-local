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

Do **not** hand-edit another agent's results file. Do **not** overwrite
`COMBINED_CHECKLIST.md` by hand — it is generated.

## Agent run log (append one row per machine/run)

| date | agent/owner | platform_id | arch | accelerators exercised | results file | pass | fail | blocked | notes |
|------|-------------|-------------|------|------------------------|--------------|------|------|---------|-------|
| _e.g. 2026-08-11_ | _@you / linux-cuda-agent_ | _linux-x64_ | _x64_ | _cpu, cuda_ | _`results/host__id.json`_ | _–_ | _–_ | _–_ | _–_ |

## Open blockers / triage log

Track every `fail` in a GA-blocking cell and every proposed `waived` here until resolved.

| id | cell_id | severity | owner | status (open/fixed-in-rcN/waived) | issue link | notes |
|----|---------|----------|-------|-----------------------------------|-----------|-------|
| _B1_ | _–_ | _blocker/major/minor_ | _–_ | _open_ | _–_ | _–_ |

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
