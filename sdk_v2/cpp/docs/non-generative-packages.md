# Multi-component non-generative packages

Foundry Local recognizes a package root when it contains `inference_model.json`,
the referenced `component_manifest.json`, `tokenizer.json`, and
`tokenizer_config.json`. Component directories are assets of that package and
are not catalogued independently.

`inference_model.json` extends the existing local artifact:

```json
{
  "Name": "clm-v0.1-8b-generic-cpu:1",
  "Alias": "clm",
  "Task": "text-ranking",
  "ComponentManifest": "component_manifest.json",
  "License": "Apache-2.0",
  "Provenance": {
    "source": "Contrastive-LM/CLM-v0.1-8B",
    "artifact_revision": "<immutable revision>",
    "base_model": "Qwen/Qwen3-8B",
    "base_revision": "<immutable revision>"
  },
  "Provider": {
    "execution_provider": "cpu",
    "variant": "fp32"
  },
  "Capabilities": ["structured-input", "candidate-ranking"]
}
```

`Name`, `Alias`, `Task`, `ComponentManifest`, `License`, every provenance
field, both provider fields, and at least one capability are required. `Task`
is `text-ranking` or `typed-decision`. Package-relative paths cannot be
absolute or contain `..`, and symlinks cannot resolve outside the canonical
package root. `ComponentManifest` is currently fixed to
`component_manifest.json`, matching the ORT GenAI package loader. Ranking
packages require `encoder`/`backbone` plus either `clm_heads` or the split
`state_head`, `action_head`, and `scorer`. Decision packages require `backbone`
plus `pointer_head`/`kev_head`. A `download.tmp` marker makes a package
incomplete.

Provider aliases are normalized and unknown values are rejected. CPU uses the
ORT default provider (an empty provider list); CUDA normalizes to `cuda`.
Supported aliases also include QNN, WebGPU, DML, OpenVINO, VitisAI, RyzenAI,
NvTensorRtRtx, and AMDGPU.

Provider libraries are prepared before a non-generative runtime is acquired.
Foundry Local packages the CUDA add-on and matching ORT provider libraries;
all ORT core/provider binaries must come from one ABI- and CUDA-compatible
bundle. WebGPU uses the normal Foundry EP bootstrapper and additionally
registers the downloaded provider in ORT GenAI's environment.

Optimized fixed-catalog CLM packages may also contain:

- `fused_state_ranking.onnx`
- `precomputed_action_projections.bin` and its JSON metadata
- `clm_cuda_graph_buckets.txt` and its JSON profile
- `safe_encoder/model.onnx`
- `clm_fallback_idle_ms.txt`

The runtime validates finite intermediate/output values. FP16 remains primary;
the BF16 safe encoder is acquired only after a non-finite FP16 encoder result.
Safe encoders are shared process-wide by package/provider identity. Each
ranking session releases its lease after the package-configured idle timeout,
and the encoder unloads when no session retains a lease. No optimization
environment variable is required.

Optional `Resources.estimated_resident_bytes` in `inference_model.json`
declares the package's qualified resident-memory estimate. When absent,
Foundry Local conservatively uses the total package file size. The service
reserves this amount before cold construction and releases it after failed
load or unload. Configure the process limit with the Foundry additional option
`NonGenerativeMemoryBudgetBytes`.

Cold initialization and readiness are bounded independently for each provider
group. The Foundry additional option
`NonGenerativeReadinessConcurrency` controls the per-provider limit and
defaults to one, preventing simultaneous large CUDA captures while allowing
CPU and CUDA packages to initialize independently.

Optional `component_runtime.json` contains generic component execution policy:

```json
{
  "schema_version": 1,
  "components": {
    "backbone": {"cuda_graph_max_signatures": 4}
  }
}
```

A zero limit disables capture. Positive limits enable bounded multi-signature
capture with persistent buffers; unseen signatures execute eagerly after the
budget is exhausted.

Register the root through the local catalog with a matching model ID and task.
`POST /v1/rank` and `POST /v1/systemone` accept a `model` containing either the
registered ID or alias. Runtimes are cached by resolved model ID, canonical
path, and provider. Active requests own a model session, so unload and
unregister fail until they drain. Unload evicts the runtime.

Explicit `FOUNDRY_LOCAL_RANK_MODEL_PATH`,
`FOUNDRY_LOCAL_SYSTEMONE_MODEL_PATH`, and
`FOUNDRY_LOCAL_NON_GENERATIVE_PACKAGE_ROOT` remain prototype-only fallbacks
when the endpoint default (`clm`/`clm-latest` or `kev`/`kev-latest`) is absent
from the selected catalog. Unknown explicit names return not found rather than
silently using an environment package. When fallback metadata is present it is
validated and the response reports its actual package model ID. No remote
catalog submission is performed.
