# Combined validation checklist (generated)

- Machines reporting: Baijus-Mac-mini.local
- Platforms covered: macos-arm64
- Cells (deduplicated): 8

## Provisional verdict: **NO-GO ❌**

> Provisional and mechanical: it only checks that GA-blocking cells are pass/waived. The release lead makes the final call per ACCEPTANCE_POLICY.md, including manifest/`n-a` justification and GA-artifact equivalence.

## Totals

| result | count |
|--------|-------|
| pass | 7 |
| fail | 1 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## GA-blocking cells needing attention

| cell_id | result | machine | notes |
|---------|--------|---------|-------|
| js__pkg-inspect__macos-arm64__cpu | **fail** | Baijus-Mac-mini.local | inspected foundry-local-sdk-2.0.0-rc1.tgz |

## All cells

| sdk | feature | accel | model | result | blocking | machine |
|-----|---------|-------|-------|--------|----------|---------|
| cpp | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cpp | pkg-inspect | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cs | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cs | pkg-inspect | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| js | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| js | pkg-inspect | cpu | - | **fail** | yes | Baijus-Mac-mini.local |
| python | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| python | pkg-inspect | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
