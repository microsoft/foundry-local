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
