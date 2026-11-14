# Multi-component non-generative packages

Foundry Local's runtime package contract requires
`component_manifest.json`, `genai_config.json`, `tokenizer.json`, and
`tokenizer_config.json`. Component directories are assets of that package and
are not catalogued independently. `inference_model.json` is optional when the
caller supplies the model ID and task during local registration.

When present, `inference_model.json` may provide descriptive discovery
metadata such as:

```json
{
  "Name": "clm-v0.1-8b-generic-cpu:1",
  "Alias": "clm",
  "Task": "ranking",
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

Foundry does not require the descriptive license, provenance, capabilities,
provider variant, or component-role layout shown above. If `Task` is present,
it must match the task supplied during registration. Package-relative paths
cannot escape the canonical package root, including through symlinks. A
`download.tmp` marker makes a package incomplete. ORT GenAI owns
model-specific graph topology validation.

Provider aliases are normalized and unknown values are rejected. CPU uses the
ORT default provider (an empty provider list); CUDA normalizes to `cuda`.
Runtime provider selection and options come from `genai_config.json`; Foundry
metadata records CPU or CUDA for catalog identity and admission grouping.
Foundry Local packages the CUDA add-on and matching ORT provider libraries;
all ORT core/provider binaries must come from one ABI- and CUDA-compatible
bundle.

ORT GenAI 0.19 supports an optional `fused_state_ranking` component when the
package declares it. Foundry does not implement a separate BF16 recovery
encoder, shared fallback pool, idle lease, projection cache, or graph-profile
framework.

Foundry estimates admission bytes from the total package file size.
`Resources.estimated_resident_bytes` is not currently used. File size is an
admission estimate, not a guaranteed conservative resident-memory bound. The
service reserves the estimate before cold construction and releases it after
failed load or unload. Configure the process limit with
`NonGenerativeMemoryBudgetBytes`.

Cold initialization and readiness are bounded independently for each provider
group. The Foundry additional option
`NonGenerativeReadinessConcurrency` controls the per-provider limit and
defaults to one, preventing simultaneous large CUDA captures while allowing
CPU and CUDA packages to initialize independently.

Register the root through the local catalog with a matching model ID and task.
`POST /catalogs/local/v1/rank` and
`POST /catalogs/local/v1/systemone` accept a `model` containing either the
registered ID or alias. Runtimes are cached by resolved model ID, canonical
path, provider, and task. Active requests own a model session, so unload and
unregister fail until they drain. Unload evicts the runtime.

HTTP routes require a registered local-catalog model ID or alias. The direct
C++ `RankingRuntime` and `DecisionRuntime` helpers may still be used with an
explicit package path by examples and internal integrations. Unknown explicit names return not found rather than
silently using an environment package. When fallback metadata is present it is
validated and the response reports its actual package model ID. No remote
catalog submission is performed.

Ranking results include `id` and zero-based `index` separately from candidate
text. Probabilities are relative to the submitted candidate set; ties preserve
input order. KEV `choice` criteria preserve caller-provided IDs and object
order. KEV `score` criteria are an ordered non-empty array. The returned score is the
probability-weighted average of the zero-based levels and may be fractional.
Foundry forwards that value unchanged. Decision temperature is fixed at
`1.0`.
