# Azure ML Registry model catalog

The Python SDK can discover and run models from an Azure ML registry using
Foundry Local's existing model, cache, download, and inference APIs. Registry
models are read-only; catalogs do not publish models or modify registry tags.

## Acquire a catalog

```python
from foundry_local_sdk import CatalogType

catalog = manager.get_catalog(
    CatalogType.AZURE_REGISTRY,
    {"name": "contoso-models", "credential": "azure_cli"},
)
model = catalog.get_model("my-model")
if model is None:
    raise LookupError("No compatible model")
model.download()
model.load()
```

`name` is required and identifies a 2–32 character registry name. Names are
normalized case-insensitively. `credential` is optional and accepts
`azure_cli` (the default) or `workload_identity`. Unknown options and invalid
values are errors. Equivalent options reuse the same manager-owned catalog;
different registries or credential modes have separate catalogs. Public and
local catalog acquisition is unchanged.

The C API exposes `Manager_GetCatalogWithOptions` in API v4. C++ and Python
negotiate supported API versions. Hosted/out-of-process catalog access is not
implemented in this feature.

## Shared catalog processing

```text
Public service / Azure ML REST client
    -> source adapter: ModelInfo records
    -> RemoteModelCatalog: compatibility and provider filtering
    -> BaseModelCatalog: grouping, indices, stable handles, selection
    -> existing download and inference pipeline
```

The public and registry sources share processing after metadata acquisition.
Compatibility and device/provider filtering therefore apply to both catalogs,
including listing and version history. A future device-capability or model-size
policy belongs in this shared processing path, not in an Azure-specific adapter.

Listings select the latest compatible numeric version per variant. Exact
`<variant-name>:<version>` lookup and version history can still access older
versions. Cache-first alias lookup can select a completed older package.
Named history queries fetch only that model's paginated versions; full discovery
fetches up to four models' version lists concurrently, using the same retry policy.
Retained aliases discover newly cached members without reacquisition and preserve
loaded or explicitly selected variants. Loaded entries remain visible if their alias
disappears remotely, until they are unloaded and the catalog refreshes.

## Registry metadata

Each variant is an Azure ML custom model with a positive integer version. The
package root contains `genai_config.json` and the referenced model files.
Required string tags are:

| Tag | Example |
|---|---|
| `alias` | `my-model` |
| `task` | `chat-completion` |
| `device` | `CPU` |
| `execution_provider` | `CPUExecutionProvider` |
| `os` | `windows` |
| `architecture` | `arm64` |

Optional `minFLVersion` uses the same compatibility check as public models.
Assets without Foundry hardware tags or the legacy `foundryLocal` marker are
ignored. Malformed opted-in records are skipped with a warning; an exact lookup
of a malformed record reports the error. Legacy `foundryLocal` packages with a
recognized backend/device suffix are supported.

## Authentication, cache, and downloads

Run `az login` to use the default Azure CLI credential. Federated environments
can select `workload_identity` and provide `AZURE_TENANT_ID`,
`AZURE_CLIENT_ID`, and `AZURE_FEDERATED_TOKEN_FILE`. Azure Identity handles
token acquisition and refresh; cached lookups do not construct credentials.
SAS URLs are not persisted.

Registry snapshots and packages are stored under a registry-specific directory
inside the model cache, separate from public publisher directories. Completed
packages can be reused after restart. Snapshot read/merge/write uses the
cross-process file lock to prevent concurrent catalog instances from losing
updates. Online listings discard snapshot entries for a different registry
resource ID. Artifact grants accept the original Blob host or an account-bound
regional Azure ML data-proxy host, always over HTTPS and for the original container.
A download is complete only after package validation, completion metadata is
written, and the in-progress marker is removed. All supported generative tasks
require a valid `genai_config.json`; non-generative tasks validate their component
package. Retained handles use these same completion rules after removal or cancellation.
The runtime identifies generative artifacts by model ID and normalized path, so
independent catalogs can load different packages with the same public model ID.

The simple registry examples for [Python](../sdk_v2/python/examples/azure_registry_catalog.py),
[C++](../sdk_v2/cpp/examples/azure_registry_catalog/main.cc),
[C#](../sdk_v2/cs/examples/AzureRegistryCatalog/Program.cs),
[JavaScript](../sdk_v2/js/examples/azure_registry_catalog.mjs), and
[Rust](../sdk_v2/rust/examples/azure_registry_catalog.rs) download and load
`qwen2.5-0.5b` from `azureml-dev`, send one prompt, then clean up.
Review the model license before running; change the registry/model names for your catalog.
Java's existing speech-recognition preview is unchanged and does not expose registry catalogs.

Mocked unit tests cover discovery, compatibility, version selection, caching,
and failures without Azure access or model downloads.
