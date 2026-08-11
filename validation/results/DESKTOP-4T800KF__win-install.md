# Validation results — DESKTOP-4T800KF (win-install)

- Platform: `windows-x64` (windows/x64)
- CPU: Intel64 Family 6 Model 140 Stepping 1, GenuineIntel
- GPUs: NVIDIA RTX A2000 Laptop GPU
- CUDA: Cuda compilation tools, release 13.3, V13.3.33

## Totals

| result | count |
|--------|-------|
| pass | 4 |
| fail | 0 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## Cells

| sdk | feature | accel | model | result | blocking | notes |
|-----|---------|-------|-------|--------|----------|-------|
| cpp | install-smoke | cpu | - | **pass** | yes | native lib present for win-x64: C:\Code\foundry-local\validation\results\work-DE |
| cs | install-smoke | cpu | - | **pass** | yes | restored+built Microsoft.AI.Foundry.Local 2.0.0-rc1 (win-x64) |
| js | install-smoke | cpu | - | **pass** | yes | installed version=2.0.0-rc1 |
| python | install-smoke | cpu | - | **pass** | yes | installed version=2.0.0rc1 |
