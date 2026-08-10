# Validation results — onnxr4c75000005 (linux-pyfeat)

- Platform: `linux-x64` (linux/x64)
- CPU: x86_64
- GPUs: NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB
- CUDA: | NVIDIA-SMI 580.105.08             Driver Version: 580.105.08     CUDA Version: 13.0     |

## Totals

| result | count |
|--------|-------|
| pass | 7 |
| fail | 0 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## Cells

| sdk | feature | accel | model | result | blocking | notes |
|-----|---------|-------|-------|--------|----------|-------|
| python | audio-file | cpu | whisper-tiny | **pass** | yes | sample-backed run completed |
| python | chat | cpu | qwen2.5-0.5b | **pass** | yes | sample-backed run completed |
| python | embeddings | cpu | qwen3-embedding-0.6b | **pass** | yes | sample-backed run completed |
| python | integrations | cpu | qwen2.5-0.5b | **pass** | no | sample-backed run completed |
| python | tool-calling | cpu | qwen2.5-0.5b | **pass** | yes | sample-backed run completed |
| python | vision | cpu | qwen3-vl-2b-instruct | **pass** | yes | sample-backed run completed |
| python | web-server | cpu | qwen2.5-0.5b | **pass** | yes | sample-backed run completed |
