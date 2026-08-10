# Validation results — onnxr4c75000005 (linux-csfeat)

- Platform: `linux-x64` (linux/x64)
- CPU: x86_64
- GPUs: NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB
- CUDA: | NVIDIA-SMI 580.105.08             Driver Version: 580.105.08     CUDA Version: 13.0     |

## Totals

| result | count |
|--------|-------|
| pass | 0 |
| fail | 4 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## Cells

| sdk | feature | accel | model | result | blocking | notes |
|-----|---------|-------|-------|--------|----------|-------|
| cs | chat | cpu | qwen2.5-0.5b | **fail** | yes | sample-backed run completed |
| cs | embeddings | cpu | qwen3-embedding-0.6b | **fail** | yes | sample-backed run completed |
| cs | model-mgmt | cpu | qwen2.5-0.5b | **fail** | yes | sample-backed run completed |
| cs | web-server | cpu | qwen2.5-0.5b | **fail** | yes | sample-backed run completed |

## ❌ Blocking failures (go/no-go blockers)

- `cs__chat__linux-x64__cpu` — sample-backed run completed
- `cs__embeddings__linux-x64__cpu` — sample-backed run completed
- `cs__web-server__linux-x64__cpu` — sample-backed run completed
- `cs__model-mgmt__linux-x64__cpu` — sample-backed run completed
