# Validation results — DESKTOP-4T800KF (win-csfeat)

- Platform: `windows-x64` (windows/x64)
- CPU: Intel64 Family 6 Model 140 Stepping 1, GenuineIntel
- GPUs: NVIDIA RTX A2000 Laptop GPU
- CUDA: Cuda compilation tools, release 13.3, V13.3.33

## Totals

| result | count |
|--------|-------|
| pass | 0 |
| fail | 7 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## Cells

| sdk | feature | accel | model | result | blocking | notes |
|-----|---------|-------|-------|--------|----------|-------|
| cs | audio-file | cpu | whisper-tiny | **fail** | yes | sample-backed run completed |
| cs | chat | cpu | qwen2.5-0.5b | **fail** | yes | sample-backed run completed |
| cs | embeddings | cpu | qwen3-embedding-0.6b | **fail** | yes | sample-backed run completed |
| cs | model-mgmt | cpu | qwen2.5-0.5b | **fail** | yes | sample-backed run completed |
| cs | tool-calling | cpu | qwen2.5-0.5b | **fail** | yes | sample install/build failed |
| cs | vision | cpu | qwen3-vl-2b-instruct | **fail** | yes | sample install/build failed |
| cs | web-server | cpu | qwen2.5-0.5b | **fail** | yes | sample-backed run completed |

## ❌ Blocking failures (go/no-go blockers)

- `cs__chat__windows-x64__cpu` — sample-backed run completed
- `cs__tool-calling__windows-x64__cpu` — sample install/build failed
- `cs__embeddings__windows-x64__cpu` — sample-backed run completed
- `cs__audio-file__windows-x64__cpu` — sample-backed run completed
- `cs__vision__windows-x64__cpu` — sample install/build failed
- `cs__web-server__windows-x64__cpu` — sample-backed run completed
- `cs__model-mgmt__windows-x64__cpu` — sample-backed run completed
