# Validation results — onnxr4c75000005 (linux-pkg)

- Platform: `linux-x64` (linux/x64)
- CPU: x86_64
- GPUs: NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB
- CUDA: | NVIDIA-SMI 580.105.08             Driver Version: 580.105.08     CUDA Version: 13.0     |

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
| cpp | install-smoke | cpu | - | **pass** | yes | native lib present for linux-x64: /datadisks/disk4/bmeswani/Foundry-Local/valida |
| cpp | pkg-inspect | cpu | - | **pass** | yes | inspected microsoft.ai.foundry.local.runtime.2.0.0-rc1.nupkg |
| cs | install-smoke | cpu | - | **pass** | yes | restored+built Microsoft.AI.Foundry.Local 2.0.0-rc1 (linux-x64) |
| cs | pkg-inspect | cpu | - | **pass** | yes | inspected microsoft.ai.foundry.local.2.0.0-rc1.nupkg |
| js | install-smoke | cpu | - | **pass** | yes | installed version=2.0.0-rc1 |
| js | pkg-inspect | cpu | - | **fail** | yes | inspected foundry-local-sdk-2.0.0-rc1.tgz |
| python | install-smoke | cpu | - | **pass** | yes | installed version=2.0.0rc1 |
| python | pkg-inspect | cpu | - | **pass** | yes | inspected foundry_local_sdk-2.0.0rc1-cp311-abi3-linux_x86_64.whl |

## ❌ Blocking failures (go/no-go blockers)

- `js__pkg-inspect__linux-x64__cpu` — inspected foundry-local-sdk-2.0.0-rc1.tgz
