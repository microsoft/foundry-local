# Validation results — onnxr4c75000005 (linux-jsfeat)

- Platform: `linux-x64` (linux/x64)
- CPU: x86_64
- GPUs: NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB
- CUDA: | NVIDIA-SMI 580.105.08             Driver Version: 580.105.08     CUDA Version: 13.0     |

## Totals

| result | count |
|--------|-------|
| pass | 6 |
| fail | 1 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## Cells

| sdk | feature | accel | model | result | blocking | notes |
|-----|---------|-------|-------|--------|----------|-------|
| js | audio-file | cpu | whisper-tiny | **pass** | yes | sample-backed run completed |
| js | chat | cpu | qwen2.5-0.5b | **pass** | yes | sample-backed run completed |
| js | embeddings | cpu | qwen3-embedding-0.6b | **pass** | yes | sample-backed run completed |
| js | integrations | cpu | qwen2.5-0.5b | **pass** | no | sample-backed run completed |
| js | tool-calling | cpu | qwen2.5-0.5b | **fail** | yes | sample-backed run completed |
| js | vision | cpu | qwen3-vl-2b-instruct | **pass** | yes | sample-backed run completed |
| js | web-server | cpu | qwen2.5-0.5b | **pass** | yes | sample-backed run completed |

## ❌ Blocking failures (go/no-go blockers)

- `js__tool-calling__linux-x64__cpu` — sample-backed run completed
