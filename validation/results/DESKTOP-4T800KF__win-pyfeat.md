# Validation results — DESKTOP-4T800KF (win-pyfeat)

- Platform: `windows-x64` (windows/x64)
- CPU: Intel64 Family 6 Model 140 Stepping 1, GenuineIntel
- GPUs: NVIDIA RTX A2000 Laptop GPU
- CUDA: Cuda compilation tools, release 13.3, V13.3.33

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
