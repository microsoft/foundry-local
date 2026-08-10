# Validation results — onnxr4c75000005 (linux-direct)

- Platform: `linux-x64` (linux/x64)
- CPU: x86_64
- GPUs: NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB, NVIDIA A100-SXM4-80GB
- CUDA: | NVIDIA-SMI 580.105.08             Driver Version: 580.105.08     CUDA Version: 13.0     |

## Totals

| result | count |
|--------|-------|
| pass | 8 |
| fail | 0 |
| blocked | 0 |
| waived | 0 |
| n-a | 0 |
| skipped | 0 |

## Cells

| sdk | feature | accel | model | result | blocking | notes |
|-----|---------|-------|-------|--------|----------|-------|
| python | chat | cuda | qwen2.5-0.5b | **pass** | yes | REAL CUDA inference on A100: qwen2.5-0.5b cuda-gpu variant loaded (1.2s), 'Paris |
| python | compat-upgrade | cpu | qwen2.5-0.5b | **pass** | yes | Direct-API scripts/compat_surface_check.py 4/4 on linux-x64 vs 1.2.4 baseline: f |
| python | crosscutting | cpu | qwen2.5-0.5b | **pass** | yes | Direct-API scripts/crosscutting_check.py 8/8 on linux-x64 (typed errors, concurr |
| python | ep-bootstrap | cpu | - | **pass** | yes | download_and_register_eps() registers CPU EP; real CPU inference verified across |
| python | ep-bootstrap | cuda | - | **pass** | yes | CUDA EP registers and runs REAL inference on A100 once CUDA 12 runtime libs are  |
| python | model-mgmt | cpu | qwen2.5-0.5b | **pass** | yes | Direct-API scripts/model_mgmt_check.py 10/10 on linux-x64 (CPU+CUDA variants enu |
| python | model-mgmt-fail | cpu | qwen2.5-0.5b | **pass** | no | Direct-API scripts/model_mgmt_failure_check.py 7/7 on linux-x64: graceful typed  |
| python | soak-resource | cpu | qwen2.5-0.5b | **pass** | no | Direct-API scripts/soak_resource_check.py: 7/8 coarse gates; independent clean-p |
