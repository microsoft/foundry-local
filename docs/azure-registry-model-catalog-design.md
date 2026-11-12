# Azure ML Registry model catalogs

Azure Registry catalogs provide read-only discovery and local execution of models
from an Azure ML registry. Public, local and registry catalogs remain separate;
there is no aggregation or fallback to another catalog.

## Acquisition

Catalog acquisition uses source-specific string options rather than a separate
registry API or manager-global credential configuration:

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
# Use the existing inference APIs.
model.unload()
```

```cpp
foundry_local::KeyValuePairs options{
    {"name", "contoso-models"},
    {"credential", "azure_cli"},
};
auto& catalog = manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_AZURE_REGISTRY, options);
```

| Option | Meaning |
|---|---|
| `name` | Required Azure ML registry name; normalized case-insensitively |
| `credential` | `azure_cli` (default) or `workload_identity` |

Unknown keys, invalid registry names and unsupported credentials are errors.
Options are copied at acquisition. Equivalent effective settings share a
manager-owned instance; different registries or credential modes do not.
Public/local acquisition is unchanged and rejects nonempty options.

The append-only C API entry is `Manager_GetCatalogWithOptions(manager, type,
options, out_catalog)`, using the existing `flKeyValuePairs`. It requires API v5.
Earlier table entries, including the v4 registry-name entry, remain available.
C++ and Python negotiate down through supported versions; existing public/local
operations still support v2/v3 runtimes.

Developers run `az login`; model operations do not invoke `az ml` or require its
extension. Federated CI explicitly selects `workload_identity` and supplies
`AZURE_TENANT_ID`, `AZURE_CLIENT_ID` and `AZURE_FEDERATED_TOKEN_FILE`. Azure Identity
owns token acquisition/refresh. Cached lookups do not construct credentials.

## Shared processing boundary

```text
Public service client / Azure ML REST client
    -> source adapter: normalized ModelInfo records
    -> RemoteModelCatalog: compatibility, model construction, provider eligibility
    -> BaseModelCatalog: grouping, stable storage, indices and selection
    -> existing downloader / local runtime / shared host
```

Public and Azure Registry catalogs are sibling implementations of
`RemoteModelCatalog`; the registry does not inherit the public-service catalog.
Sources supply metadata and cache locations, not independently constructed
models. Final discovery, lookup, exact-version and history methods all route
through `RemoteModelCatalog::CreateModels`, including cached registry aliases.

Compatibility policies belong in `RemoteModelCatalog::IsModelCompatible`, not in an AML
adapter or a public-service-only fetch method. A future device-memory or model-size
policy added there applies to both sources and all acquisition paths, including
retained alias selection. The current
shared policies cover minimum Foundry Local version and normalized OS/architecture;
provider snapshots and default selection are shared too. No speculative memory
threshold is implemented by this change.

Paging and authentication remain protocol-specific: the public source uses
continuation tokens; AML uses validated `nextLink` URLs. Registry artifact grants
feed the existing download/resume/progress/cancellation pipeline.

## Registry metadata

Each variant is an Azure ML custom model with a positive integer version. The
package root contains `genai_config.json` and its referenced model/tokenizer files.
Required string tags are:

| Tag | Example |
|---|---|
| `alias` | `my-model` |
| `task` | `chat-completion` |
| `device` | `CPU` |
| `execution_provider` | `CPUExecutionProvider` |
| `os` | `windows` |
| `architecture` | `arm64` |

The adapter reuses `CatalogLocalModel` parsing and conversion, preserving existing
capability/context metadata and normalized platform properties. Optional
`minFLVersion` participates in the same compatibility check as public models.
Assets without Foundry hardware tags or the legacy `foundryLocal` marker are
ignored. Incomplete opted-in metadata is an error.

Legacy `foundryLocal` packages with alias/task and a recognized backend/device
suffix remain supported. Their platform admission is left to package loading.
Generic GPU models retain the existing CUDA/WebGPU/DirectML behavior.

## Inventory, cache and execution

Listings select the latest compatible numeric version per variant; exact
`<variant-name>:<version>` lookup and version history retain access to older
versions. Cache-first aliases can select an older completed package independently
of live listings. Returned handles remain manager/catalog-owned.

Registry caches remain alongside publisher directories:
`<model-cache>\<lowercase-name>-<SHA256(azureml://registries/<name>)>\<variant>-<version>`.
Public/local scanning excludes these directories and the earlier
`_test_registries` layout. SAS URLs are never persisted.

Completed packages are usable after restart without contacting Azure. Explicit
listing queries the registry subject to the normal refresh interval; ordinary
alias misses reuse that inventory rather than repeating full discovery.
Snapshots preserve completed local versions removed remotely. Read/merge/write
transactions use the existing cross-process file lock in addition to the
per-instance mutex, preventing different catalog instances from losing updates.

Shared-host discovery observes host providers without registering them in the
client. Host model selection carries registry and credential mode, while runtime
identity remains model ID plus canonical artifact path.

## Validation and smoke test

Build through the existing driver (choose the machine's architecture):

```powershell
cd sdk_v2\cpp
python build.py --configure --build --skip_tests --skip_examples --arm64 --cmake_extra_defines FOUNDRY_LOCAL_BUILD_TESTS=ON
.\build\Windows\RelWithDebInfo\bin\RelWithDebInfo\foundry_local_tests.exe --gtest_filter="*AzureRegistryCatalog*:*AzureModelCatalog*:*BaseModelCatalog*:SdkSessionTest.*Catalog*"
```

With matching Python/native dependencies and `FOUNDRY_LOCAL_LIB_DIR` configured:

```powershell
python sdk_v2\python\src\foundry_local_sdk\_native\build_cffi.py
python -m pytest sdk_v2\python\test\unit\test_catalog_types.py -q
az login
python sdk_v2\python\examples\azure_registry_model_catalog.py --registry contoso-models --model-id my-model-generic-cpu:1
```

Mocked native tests require no Azure credentials or physical accelerators.
Live Azure/federated CI validation remains opt-in. Preview AML endpoint changes
remain a compatibility risk; errors preserve HTTP status and Azure request IDs
without exposing tokens or artifact grants.
