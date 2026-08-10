# Combined validation checklist (generated)

- Machines reporting: Baijus-Mac-mini.local, onnxr4c75000005
- Platforms covered: linux-x64, macos-arm64
- Cells (deduplicated): 54

## Provisional verdict: **NO-GO ❌**

> Provisional and mechanical: it only checks that GA-blocking cells are pass/waived. The release lead makes the final call per ACCEPTANCE_POLICY.md, including manifest/`n-a` justification and GA-artifact equivalence.

## Totals

| result | count |
|--------|-------|
| pass | 46 |
| fail | 8 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## GA-blocking cells needing attention

| cell_id | result | machine | notes |
|---------|--------|---------|-------|
| cs__chat__linux-x64__cuda | **fail** | onnxr4c75000005 | Built WITHOUT GPU-specialized packages (removed Microsoft.ML.OnnxRuntime.Gpu/Onn |
| cs__embeddings__linux-x64__cpu | **fail** | onnxr4c75000005 | Built WITHOUT GPU-specialized packages (removed Microsoft.ML.OnnxRuntime.Gpu/Onn |
| cs__model-mgmt__linux-x64__cpu | **fail** | onnxr4c75000005 | Built WITHOUT GPU-specialized packages (removed Microsoft.ML.OnnxRuntime.Gpu/Onn |
| cs__web-server__linux-x64__cpu | **fail** | onnxr4c75000005 | Built WITHOUT GPU-specialized packages (removed Microsoft.ML.OnnxRuntime.Gpu/Onn |
| js__pkg-inspect__macos-arm64__cpu | **fail** | Baijus-Mac-mini.local | inspected foundry-local-sdk-2.0.0-rc1.tgz |
| js__pkg-inspect__linux-x64__cpu | **fail** | onnxr4c75000005 | inspected foundry-local-sdk-2.0.0-rc1.tgz |
| js__tool-calling__macos-arm64__cpu | **fail** | Baijus-Mac-mini.local | sample-backed run completed |
| js__tool-calling__linux-x64__cpu | **fail** | onnxr4c75000005 | sample-backed run completed |

## All cells

| sdk | feature | accel | model | result | blocking | machine |
|-----|---------|-------|-------|--------|----------|---------|
| cpp | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cpp | install-smoke | cpu | - | **pass** | yes | onnxr4c75000005 |
| cpp | pkg-inspect | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cpp | pkg-inspect | cpu | - | **pass** | yes | onnxr4c75000005 |
| cs | chat | cuda | qwen2.5-0.5b | **fail** | yes | onnxr4c75000005 |
| cs | embeddings | cpu | qwen3-embedding-0.6b | **fail** | yes | onnxr4c75000005 |
| cs | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cs | install-smoke | cpu | - | **pass** | yes | onnxr4c75000005 |
| cs | model-mgmt | cpu | qwen2.5-0.5b | **fail** | yes | onnxr4c75000005 |
| cs | pkg-inspect | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| cs | pkg-inspect | cpu | - | **pass** | yes | onnxr4c75000005 |
| cs | web-server | cpu | qwen2.5-0.5b | **fail** | yes | onnxr4c75000005 |
| js | audio-file | cpu | whisper-tiny | **pass** | yes | onnxr4c75000005 |
| js | chat | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| js | chat | cpu | qwen2.5-0.5b | **pass** | yes | onnxr4c75000005 |
| js | embeddings | cpu | qwen3-embedding-0.6b | **pass** | yes | Baijus-Mac-mini.local |
| js | embeddings | cpu | qwen3-embedding-0.6b | **pass** | yes | onnxr4c75000005 |
| js | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| js | install-smoke | cpu | - | **pass** | yes | onnxr4c75000005 |
| js | integrations | cpu | qwen2.5-0.5b | **pass** | no | onnxr4c75000005 |
| js | pkg-inspect | cpu | - | **fail** | yes | Baijus-Mac-mini.local |
| js | pkg-inspect | cpu | - | **fail** | yes | onnxr4c75000005 |
| js | tool-calling | cpu | qwen2.5-0.5b | **fail** | yes | Baijus-Mac-mini.local |
| js | tool-calling | cpu | qwen2.5-0.5b | **fail** | yes | onnxr4c75000005 |
| js | vision | cpu | qwen3-vl-2b-instruct | **pass** | yes | onnxr4c75000005 |
| js | web-server | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| js | web-server | cpu | qwen2.5-0.5b | **pass** | yes | onnxr4c75000005 |
| python | audio-file | cpu | whisper-tiny | **pass** | yes | Baijus-Mac-mini.local |
| python | audio-file | cpu | whisper-tiny | **pass** | yes | onnxr4c75000005 |
| python | chat | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| python | chat | cpu | qwen2.5-0.5b | **pass** | yes | onnxr4c75000005 |
| python | chat | cuda | qwen2.5-0.5b | **pass** | yes | onnxr4c75000005 |
| python | chat | webgpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| python | compat-upgrade | cpu | qwen2.5-0.5b | **pass** | yes | onnxr4c75000005 |
| python | crosscutting | cpu | qwen2.5-0.5b | **pass** | yes | onnxr4c75000005 |
| python | embeddings | cpu | qwen3-embedding-0.6b | **pass** | yes | Baijus-Mac-mini.local |
| python | embeddings | cpu | qwen3-embedding-0.6b | **pass** | yes | onnxr4c75000005 |
| python | ep-bootstrap | cpu | - | **pass** | yes | onnxr4c75000005 |
| python | ep-bootstrap | cuda | - | **pass** | yes | onnxr4c75000005 |
| python | install-smoke | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| python | install-smoke | cpu | - | **pass** | yes | onnxr4c75000005 |
| python | integrations | cpu | qwen2.5-0.5b | **pass** | no | Baijus-Mac-mini.local |
| python | integrations | cpu | qwen2.5-0.5b | **pass** | no | onnxr4c75000005 |
| python | model-mgmt | cpu | qwen2.5-0.5b | **pass** | yes | onnxr4c75000005 |
| python | model-mgmt-fail | cpu | qwen2.5-0.5b | **pass** | no | onnxr4c75000005 |
| python | pkg-inspect | cpu | - | **pass** | yes | Baijus-Mac-mini.local |
| python | pkg-inspect | cpu | - | **pass** | yes | onnxr4c75000005 |
| python | soak-resource | cpu | qwen2.5-0.5b | **pass** | no | onnxr4c75000005 |
| python | tool-calling | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| python | tool-calling | cpu | qwen2.5-0.5b | **pass** | yes | onnxr4c75000005 |
| python | vision | cpu | qwen3-vl-2b-instruct | **pass** | yes | Baijus-Mac-mini.local |
| python | vision | cpu | qwen3-vl-2b-instruct | **pass** | yes | onnxr4c75000005 |
| python | web-server | cpu | qwen2.5-0.5b | **pass** | yes | Baijus-Mac-mini.local |
| python | web-server | cpu | qwen2.5-0.5b | **pass** | yes | onnxr4c75000005 |
