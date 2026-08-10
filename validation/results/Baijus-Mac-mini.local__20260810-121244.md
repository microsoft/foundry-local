# Validation results — Baijus-Mac-mini.local (20260810-121244)

- Platform: `macos-arm64` (darwin/arm64)
- CPU: arm
- GPUs: Apple M4 Pro
- CUDA: n/a

## Totals

| result | count |
|--------|-------|
| pass | 7 |
| fail | 1 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## Cells

| sdk | feature | accel | model | result | blocking | notes |
|-----|---------|-------|-------|--------|----------|-------|
| cpp | install-smoke | cpu | - | **pass** | yes | native lib present for osx-arm64: /Users/baijumeswani/workspace/Foundry-Local/va |
| cpp | pkg-inspect | cpu | - | **pass** | yes | inspected microsoft.ai.foundry.local.runtime.2.0.0-rc1.nupkg |
| cs | install-smoke | cpu | - | **pass** | yes | restored+built Microsoft.AI.Foundry.Local 2.0.0-rc1 (osx-arm64) |
| cs | pkg-inspect | cpu | - | **pass** | yes | inspected microsoft.ai.foundry.local.2.0.0-rc1.nupkg |
| js | install-smoke | cpu | - | **pass** | yes | installed version=2.0.0-rc1 |
| js | pkg-inspect | cpu | - | **fail** | yes | inspected foundry-local-sdk-2.0.0-rc1.tgz |
| python | install-smoke | cpu | - | **pass** | yes | installed version=2.0.0rc1 |
| python | pkg-inspect | cpu | - | **pass** | yes | inspected foundry_local_sdk-2.0.0rc1-cp311-abi3-macosx_11_0_arm64.whl |

## ❌ Blocking failures (go/no-go blockers)

- `js__pkg-inspect__macos-arm64__cpu` — inspected foundry-local-sdk-2.0.0-rc1.tgz
