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

The C API exposes `Manager_GetCatalogWithOptions` in API v5. Earlier API table
entries, including the v4 registry-name entry, remain available. C++ and Python
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
resource ID. A download is complete only when files exist and, for
chat-completion packages (or packages containing `genai_config.json`), the
configuration is valid.

Mocked unit tests cover discovery, compatibility, version selection, caching,
and failures without Azure access or model downloads.
