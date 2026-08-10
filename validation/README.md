# Foundry Local 2.0.0 — Release Validation Suite

A portable, cross-platform harness to validate the **`2.0.0-rc1`** release-candidate
packages (from the ORT-Nightly Azure DevOps feed) end-to-end, on Windows / Linux / macOS,
and produce a defensible **go/no-go** decision for the public release.

Clone `releases/rel-2.0.0` on any agent, run one command, and commit the results file —
findings from every machine merge into one checklist.

## Start here

- **[VALIDATION.md](VALIDATION.md)** — intent, matrix, and how to run.
- **[ACCEPTANCE_POLICY.md](ACCEPTANCE_POLICY.md)** — result classes and go/no-go rules.
- **[FINDINGS.md](FINDINGS.md)** — where each agent reports results (multi-machine).

## Quick start

```bash
# 1. Point at the ORT-Nightly feed (secrets via env vars, never committed)
export FOUNDRY_VALIDATION_NUGET_FEED=...    # C# + C++
export FOUNDRY_VALIDATION_NPM_REGISTRY=...  # JS
export FOUNDRY_VALIDATION_PIP_INDEX=...     # Python
export FOUNDRY_VALIDATION_FEED_TOKEN=...    # if private

# 2. See what applies to this machine
./run.sh --list                 # Linux/macOS
pwsh run.ps1 -List              # Windows

# 3. Run
./run.sh                        # all applicable cells
./run.sh --sdk python --feature install-smoke,chat   # a subset
python3 orchestrator/run_validation.py --simulate    # framework dry run, no installs

# 4. Merge results from all machines
python3 orchestrator/aggregate.py
```

## Layout

```
validation/
  README.md                     This file
  VALIDATION.md                 Intent + how-to-run (shared with other sessions)
  ACCEPTANCE_POLICY.md          Go/no-go policy
  FINDINGS.md                   Multi-machine reporting surface
  run.sh / run.ps1              One-command wrappers (Linux/macOS, Windows)
  manifests/
    platform_manifest.json      Frozen supported platforms/accelerators
    model_ep_availability.json  Expected model variants per platform (n-a justification)
    coverage_cells.json         Feature/cell catalog + blocking flags
  schema/
    result_record.schema.json   Shared per-cell result schema
  orchestrator/                 Stdlib-only Python harness
    run_validation.py           Fingerprint -> expand cells -> run -> emit records
    envinfo.py                  Environment fingerprinting
    feeds.py                    Isolated feed/config setup (env-driven)
    runners.py                  Per-SDK cell runners (install-smoke automated)
    results.py                  Record build + schema validation + summaries
    aggregate.py                Merge per-machine results -> combined checklist
  results/                      Per-machine result files land here (commit these)
```

No third-party packages are required to run the orchestrator (Python 3.10+ only). Real
package installs need the feed env vars above and the relevant SDK toolchain
(dotnet / node+npm / python) on the agent.
