# Combined validation checklist (generated)

- Machines reporting: Baijus-Mac-mini.local
- Platforms covered: macos-arm64
- Cells (deduplicated): 20

## Provisional verdict: **NO-GO ❌**

> Provisional and mechanical: it only checks that GA-blocking cells are pass/waived. The release lead makes the final call per ACCEPTANCE_POLICY.md, including manifest/`n-a` justification and GA-artifact equivalence.

## Totals

| result | count |
|--------|-------|
| pass | 18 |
| fail | 2 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## GA-blocking cells needing attention

| cell_id | result | machine | notes |
|---------|--------|---------|-------|
| js__pkg-inspect__macos-arm64__cpu | **fail** | Baijus-Mac-mini.local | inspected foundry-local-sdk-2.0.0-rc1.tgz |
| js__tool-calling__macos-arm64__cpu | **fail** | Baijus-Mac-mini.local | sample-backed run completed |

## All cells

| sdk | feature | accel | model | result | blocking | machine |
|-----|---------|-------|-------|--------|----------|---------|
| cpp | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cpp | pkg-inspect | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cs | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cs | pkg-inspect | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| js | chat | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| js | embeddings | cpu | qwen3-embedding-0.6b | **pass** | yes | Baijus-Mac-mini.local |
| js | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| js | pkg-inspect | cpu | - | **fail** | yes | Baijus-Mac-mini.local |
| js | tool-calling | cpu | qwen2.5-0.5b | **fail** | yes | Baijus-Mac-mini.local |
| js | web-server | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| python | audio-file | cpu | whisper-tiny | **pass** | yes | Baijus-Mac-mini.local |
| python | chat | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| python | chat | webgpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| python | embeddings | cpu | qwen3-embedding-0.6b | **pass** | yes | Baijus-Mac-mini.local |
| python | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| python | integrations | cpu | qwen2.5-0.5b | **pass** | no | Baijus-Mac-mini.local |
| python | pkg-inspect | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| python | tool-calling | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| python | vision | cpu | qwen3-vl-2b-instruct | **pass** | yes | Baijus-Mac-mini.local |
| python | web-server | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
