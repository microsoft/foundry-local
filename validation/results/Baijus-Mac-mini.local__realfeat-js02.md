# Validation results — Baijus-Mac-mini.local (realfeat-js02)

- Platform: `macos-arm64` (darwin/arm64)
- CPU: arm
- GPUs: Apple M4 Pro
- CUDA: n/a

## Totals

| result | count |
|--------|-------|
| pass | 2 |
| fail | 1 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## Cells

| sdk | feature | accel | model | result | blocking | notes |
|-----|---------|-------|-------|--------|----------|-------|
| js | embeddings | cpu | qwen3-embedding-0.6b | **pass** | yes | sample-backed run completed |
| js | tool-calling | cpu | qwen2.5-0.5b | **fail** | yes | sample-backed run completed |
| js | web-server | cpu | qwen2.5-0.5b | **pass** | yes | sample-backed run completed |

## ❌ Blocking failures (go/no-go blockers)

- `js__tool-calling__macos-arm64__cpu` — sample-backed run completed
