# Validation results — DESKTOP-4T800KF (win-jsfeat)

- Platform: `windows-x64` (windows/x64)
- CPU: Intel64 Family 6 Model 140 Stepping 1, GenuineIntel
- GPUs: NVIDIA RTX A2000 Laptop GPU
- CUDA: Cuda compilation tools, release 13.3, V13.3.33

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

- `js__tool-calling__windows-x64__cpu` — sample-backed run completed
