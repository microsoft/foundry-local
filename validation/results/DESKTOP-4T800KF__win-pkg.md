# Validation results — DESKTOP-4T800KF (win-pkg)

- Platform: `windows-x64` (windows/x64)
- CPU: Intel64 Family 6 Model 140 Stepping 1, GenuineIntel
- GPUs: NVIDIA RTX A2000 Laptop GPU
- CUDA: Cuda compilation tools, release 13.3, V13.3.33

## Totals

| result | count |
|--------|-------|
| pass | 2 |
| fail | 2 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## Cells

| sdk | feature | accel | model | result | blocking | notes |
|-----|---------|-------|-------|--------|----------|-------|
| cpp | pkg-inspect | cpu | - | **pass** | yes | inspected microsoft.ai.foundry.local.runtime.2.0.0-rc1.nupkg |
| cs | pkg-inspect | cpu | - | **pass** | yes | inspected microsoft.ai.foundry.local.2.0.0-rc1.nupkg |
| js | pkg-inspect | cpu | - | **fail** | yes | inspected foundry-local-sdk-2.0.0-rc1.tgz |
| python | pkg-inspect | cpu | - | **fail** | yes | inspected foundry_local_sdk-2.0.0rc1-cp311-abi3-win_amd64.whl |

## ❌ Blocking failures (go/no-go blockers)

- `js__pkg-inspect__windows-x64__cpu` — inspected foundry-local-sdk-2.0.0-rc1.tgz
- `python__pkg-inspect__windows-x64__cpu` — inspected foundry_local_sdk-2.0.0rc1-cp311-abi3-win_amd64.whl
