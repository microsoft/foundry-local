# Python Bindings Design — `sdk_v2/python`

**Status:** Draft, pending review
**Owner:** SDK team
**Target:** New Python SDK at `sdk_v2/python/` that binds directly to the Foundry Local C ABI (`foundry_local_c.h`).

---

## 1. Goals and non-goals

### Goals

- Replace the legacy Python SDK (`sdk/python`) with a new package under `sdk_v2/python/` that:
  - **Preserves the existing public API of `sdk/python` without breaking changes.** Existing user code that imports `Configuration`, `FoundryLocalManager`, `IModel`, `Catalog`, `EpInfo`, `EpDownloadResult`, `LogLevel`, `FoundryLocalException`, etc. must keep working as-is. Constructors, classmethods, properties, return types, and exception types are all part of the contract.
  - Binds directly to the C ABI declared in [foundry_local_c.h](../include/foundry_local/foundry_local_c.h). No reuse of the legacy `core_interop.py` JSON-command FFI.
  - Adds the new `Item` / `Session` / `Request` / `Response` programming model that the C++ and C# SDKs already expose, alongside the legacy surface — not in place of it.
  - Where the new model overlaps with legacy types (e.g. `Model` vs `IModel`), the legacy name is kept and the new shape is layered on additively.
  - Mirrors the design and naming of the C# SDK at `sdk_v2/cs/` for the *new* surface area only. Legacy names win where they collide.
  - Supports streaming inference at competitive performance — per-token overhead should not be the bottleneck.

### Parity rule

If a name, signature, return type, or exception type exists in [sdk/python/src](../../../sdk/python/src), the new SDK matches it exactly. New surface area is purely additive. When the C# SDK and the legacy Python SDK disagree, legacy Python wins for Python users.

### Non-goals

- Reimplementing the OpenAI Python client. The optional convenience layer is a thin wrapper over the upstream `openai` package, not a fork.
- `async`/`await` first-class API in v1. A synchronous public API matches Python idiom for inference workloads (Jupyter, scripts, web apps with blocking handlers). An `aio` submodule may be added later if user demand materialises.
- Wrapping the C++ classes directly (pybind11 / nanobind). The C ABI is the binding boundary by design — coupling the Python wheel to the C++ source tree would defeat the ABI stability work.
- Maintaining `sdk/python` going forward. It stays in the tree as a migration reference but receives no new features.

---

## 2. References

- **C ABI (binding target):** [foundry_local_c.h](../include/foundry_local/foundry_local_c.h)
- **C++ public surface (design rationale):** [foundry_local_cpp.h](../include/foundry_local/foundry_local_cpp.h)
- **C# bindings (design template):** [sdk_v2/cs/src](../../cs/src)
  - `Detail/FoundryLocalApi.cs`, `Detail/NativeMethods.cs` — interop layer pattern
  - `Session.cs`, `ChatSession.cs`, `Request.cs`, `Response.cs`, `Items/*.cs` — public API shape
- **Legacy Python SDK (parity reference):** [sdk/python](../../../sdk/python)
  - `foundry_local_manager.py`, `catalog.py`, `imodel.py`, `openai/chat_client.py`

---

## 3. Architecture

### 3.1 Layered structure

```
+------------------------------------------------------+
|  Public API:  foundry_local.*                        |
|    Manager, Catalog, Model, Configuration            |
|    Session, ChatSession, AudioSession, ...           |
|    Request, Response, Item hierarchy                 |
|    OpenAI compat layer (optional install extra)      |
+------------------------------------------------------+
|  Native interop:  foundry_local._native              |
|    cffi-generated extension module (_cffi.*.pyd/.so) |
|    Api singleton (loads vtables, status conversion)  |
|    Callback trampolines, lib loader                  |
+------------------------------------------------------+
|  Native library:  foundry_local.dll / .so / .dylib   |
|    (built from sdk_v2/cpp; bundled into wheel)       |
+------------------------------------------------------+
```

The native interop layer is a private package (`_native`). Public consumers import from `foundry_local`, never from `foundry_local._native`.

### 3.2 FFI technology choice — cffi (API mode)

We use **cffi in out-of-line API mode**, not ctypes.

#### Rationale

This SDK has a streaming hot path: per-token decoding fires a native callback that pushes one item per call into a queue, and Python pops and yields each one. The number of calls per second is in the thousands. Vtable indirection means every call also pays the cost of a function-pointer load through a Python-defined struct.

| Concern | ctypes | cffi (API mode) |
|---|---|---|
| Per-call overhead | ~1.5–3 µs (interpreted marshalling) | ~150–400 ns (compiled trampolines) |
| Vtable struct definitions (~80 fn ptrs) | Hand-mirrored Python classes; field order and type must match the C struct byte-for-byte. Drift is silent UB. | `cdef` consumes a (lightly preprocessed) copy of the actual header. Layout verified at extension build time against the real declarations. |
| UTF-8 strings (frequent) | `c_char_p` boxing on every call | `ffi.string()` on returned pointers; direct char* args |
| Streaming callback | `CFUNCTYPE` wrapper marshals args interpretively on the native thread | `@ffi.callback(...)` is a compiled trampoline |
| Header sync | Manual; no compile-time check | Paste the new struct snippet, rebuild |
| Stdlib | Yes | No (adds `cffi` runtime dep) |

The runtime dependency on `cffi` is acceptable: it is in the dependency closure of the bulk of the data/AI Python ecosystem (`cryptography`, `paramiko`, `bcrypt`, etc.) and ships prebuilt wheels for every platform we target. The wheel build needs `cffi` and a C compiler at build time, and we already require a C++ toolchain to build `foundry_local.dll`, so the marginal cost is zero.

#### Why API mode specifically

cffi has two modes:
- **ABI mode** (in-line `ffi.dlopen`): roughly the same per-call cost as ctypes. Picked for cdef syntax, not speed.
- **API mode** (out-of-line, `ffi.set_source(...)` + build-time compile): generates a CPython extension that calls native functions directly with no per-call interpreter overhead.

Only API mode pays for itself on a streaming inference path.

#### Why not pybind11 / nanobind

We could skip the C ABI and bind directly to C++ classes. Faster still, but:

- Couples the Python wheel to the C++ source tree — every C++ change forces a Python wheel rebuild.
- Defeats the C ABI stability work the C# bindings already rely on.
- Diverges from C# bindings — two binding paths, double the maintenance.

The C ABI was designed to be the binding boundary. We keep it.

### 3.3 Synchronous public API

Python users expect blocking calls for inference. The legacy `sdk/python` is sync. The OpenAI Python client is sync by default. We follow suit.

Streaming returns a regular iterator:

```python
for item in session.process_streaming_request(req):
    print(item.text, end="", flush=True)
```

Not an async generator. Users who need async can wrap with `asyncio.to_thread`. If demand justifies it later, an `aio` submodule lands as a thin wrapper around the same primitives — no separate code path on the hot path.

### 3.4 Resource lifetime

Every wrapper that owns a native handle:

- Implements `close()` and `__enter__` / `__exit__`.
- Uses `weakref.finalize(self, _release_fn, ptr)` as a safety net — the native handle is released even if the user drops the wrapper without calling `close()`.

This mirrors C# `IDisposable` semantics without forcing users to write `with` blocks for every short-lived object.

### 3.5 Threading and the GIL

- All C ABI calls release the GIL via cffi's `release_gil=True` annotation on long-running functions (`Session_ProcessRequest`, `Model_Download`, `Manager_DownloadAndRegisterEps`).
- Streaming and progress callbacks fire on native worker threads. The cffi-generated trampoline reacquires the GIL. The Python callback should be quick — push to a queue, return — and any heavy lifting happens on the consumer thread.
- The Python streaming iterator uses `queue.Queue` to bridge native callback thread → consumer thread. The native callback returns 1 (cancel) when the consumer signals cancellation via `Request.cancel()`.

---

## 4. Public API surface

The package is `foundry_local` (PyPI distribution: `foundry-local`). Naming follows Python conventions (snake_case methods, lowercase modules) while preserving class names from C# / legacy Python where feasible.

### 4.1 Module layout

```
sdk_v2/python/
  pyproject.toml
  build_backend.py                       # custom backend; runs C++ build, builds cffi ext, bundles binaries
  src/foundry_local/
    __init__.py                          # re-exports legacy + new public API + __version__
    py.typed                             # PEP 561 marker
    version.py                           # __version__ (legacy module name preserved)
    exception.py                         # FoundryLocalException (legacy module name preserved)
    logging_helper.py                    # LogLevel + set_default_logger_severity (legacy)
    configuration.py                     # Configuration (legacy ctor signature preserved)
    foundry_local_manager.py             # FoundryLocalManager singleton (legacy)
    catalog.py                           # Catalog (legacy)
    imodel.py                            # IModel ABC (legacy); also exports Model = IModel alias
    ep_types.py                          # EpInfo, EpDownloadResult (legacy)
    constants.py                         # FOUNDRY_LOCAL_PARAM_*, FOUNDRY_LOCAL_MODEL_PROP_* (new)
    enums.py                             # ItemType, MessageRole, FinishReason, DeviceType,
                                         #   TensorDataType, TextItemType (new)
    request.py                           # Request (new)
    response.py                          # Response, TokenUsage (new)
    items/                               # new Item hierarchy
      __init__.py
      item.py                            # Item base + dispatch on flItemType
      text_item.py                       # TextItem
      message_item.py                    # MessageItem (system/user/assistant/developer factories)
      bytes_item.py                      # BytesItem
      image_item.py                      # ImageItem
      audio_item.py                      # AudioItem
      tool_call_item.py                  # ToolCallItem
      tool_result_item.py                # ToolResultItem
      tensor_item.py                     # TensorItem (NumPy interop)
    session/                             # new Session hierarchy
      __init__.py
      session.py                         # Session base
      chat_session.py                    # ChatSession (validates task, add_tool_definition,
                                         #   turn_count, undo_turns)
      audio_session.py                   # AudioSession
      embeddings_session.py              # EmbeddingsSession
    openai/                              # legacy module path preserved
      __init__.py
      chat_client.py                     # IModel.get_chat_client() returns openai.OpenAI
      audio_client.py
      embedding_client.py
    _native/                             # private cffi layer (new)
      __init__.py
      build_cffi.py                      # ffi.cdef + ffi.set_source — runs at wheel build time
      _cffi.*.pyd / .so                  # generated extension (built into wheel)
      api.py                             # Api singleton: lib.FoundryLocalGetApi(1), vtables, CheckStatus
      callbacks.py                       # @ffi.callback wrappers for streaming/progress/EP callbacks
      lib_loader.py                      # locate foundry_local.dll/.so/.dylib
  test/                                  # unit tests (mock cffi)
  integration_test/                      # end-to-end with real models (mirrors sdk_v2/cs/test)
  examples/
    chat_session.py
    multi_turn_with_undo.py
    tool_calling.py
    audio_transcription.py
    streaming.py
```

### 4.2 Configuration

**Legacy parity, exact.** Constructor signature, attribute names, nested `WebService` class, `validate()`, and `as_dictionary()` are all preserved verbatim from [sdk/python/src/configuration.py](../../../sdk/python/src/configuration.py).

```python
class Configuration:
    class WebService:
        def __init__(
            self,
            urls: Optional[str] = None,
            external_url: Optional[str] = None,
        ): ...

    def __init__(
        self,
        app_name: str,
        foundry_local_core_path: Optional[str] = None,
        app_data_dir: Optional[str] = None,
        model_cache_dir: Optional[str] = None,
        logs_dir: Optional[str] = None,
        log_level: Optional[LogLevel] = LogLevel.WARNING,
        web: Optional["Configuration.WebService"] = None,
        additional_settings: Optional[Dict[str, str]] = None,
    ): ...

    def validate(self) -> None: ...
    def as_dictionary(self) -> Dict[str, str]: ...
```

Underneath, the constructor still flows to the new `flConfigurationApi`: the implementation translates the legacy fields into the appropriate vtable calls (`Configuration_SetLogLevel`, `Configuration_SetAppDataDir`, etc.) and stashes the resulting handle. `additional_settings` becomes repeated `Configuration_SetAdditionalOption(key, value)` calls. The `WebService.urls` value flows through `Configuration_AddWebServiceEndpoint`. All legacy fields remain readable as plain Python attributes (the constructor assigns them, just like today), so user code that reads `cfg.app_name` keeps working.

New surface area added on top of the legacy class (additive, optional):

```python
    # New helpers exposed for users who want direct access to the new
    # configuration knobs without going through the legacy field set.
    def add_catalog_url(self, url: str, filter_override: Optional[str] = None) -> "Configuration": ...
    def set_catalog_region(self, region: str) -> "Configuration": ...
```

These return `self` so they chain — they do *not* replace the constructor as the primary entry point.

### 4.3 FoundryLocalManager

**Legacy parity, exact.** The singleton API matches [sdk/python/src/foundry_local_manager.py](../../../sdk/python/src/foundry_local_manager.py): both `FoundryLocalManager(config)` and `FoundryLocalManager.initialize(config)` work, `FoundryLocalManager.instance` is a class attribute (not a method), and `start_web_service()` returns `None` while populating `self.urls`.

```python
class FoundryLocalManager:
    instance: "FoundryLocalManager" = None  # class attribute, set by __init__

    @staticmethod
    def initialize(config: Configuration) -> None:
        """Legacy entry point. Equivalent to FoundryLocalManager(config)."""

    def __init__(self, config: Configuration):
        """Enforces singleton under a class-level lock. Raises
        FoundryLocalException if already initialized."""

    # Legacy attributes (preserved, not changed)
    config: Configuration
    catalog: "Catalog"
    urls: Optional[list[str]]

    def discover_eps(self) -> list[EpInfo]: ...
    def download_and_register_eps(
        self,
        names: Optional[list[str]] = None,
        progress_callback: Optional[Callable[[str, float], None]] = None,
    ) -> EpDownloadResult: ...                # NOTE: returns EpDownloadResult, not None

    def start_web_service(self) -> None: ...   # populates self.urls
    def stop_web_service(self) -> None: ...
```

**No breaking changes from legacy.** In particular:
- The class attribute `FoundryLocalManager.instance` (not a method call) is part of the contract.
- The `__init__` and `initialize` paths both raise `FoundryLocalException` (not `RuntimeError`) on second creation.
- `download_and_register_eps` returns `EpDownloadResult`, including the case where no progress callback is supplied.
- `start_web_service` returns `None`; the bound URLs are read from `self.urls`.

Underneath, the `_core_interop` field is removed entirely. All vtable calls go through `foundry_local._native.api`. There is no shim for the old `_core_interop` attribute — it was a private implementation detail of the legacy SDK and not part of the public contract.

### 4.4 Catalog and IModel

**Legacy parity, exact.** [sdk/python/src/catalog.py](../../../sdk/python/src/catalog.py) and [sdk/python/src/imodel.py](../../../sdk/python/src/imodel.py) define the contract. We keep the `IModel` name and abstract base, and `Catalog` returns `IModel`.

```python
class Catalog:
    name: str  # populated in __init__ from get_catalog_name vtable call

    def list_models(self) -> list[IModel]: ...
    def get_model(self, model_alias: str) -> Optional[IModel]: ...
    def get_model_variant(self, model_id: str) -> Optional[IModel]: ...
    def get_latest_version(self, model_or_model_variant: IModel) -> IModel: ...
    def get_cached_models(self) -> list[IModel]: ...
    def get_loaded_models(self) -> list[IModel]: ...

class IModel(ABC):
    # All legacy properties and methods preserved verbatim:
    id: str
    alias: str
    info: ModelInfo
    is_cached: bool
    is_loaded: bool
    context_length: Optional[int]
    input_modalities: Optional[str]
    output_modalities: Optional[str]
    capabilities: Optional[str]
    supports_tool_calling: Optional[bool]
    variants: list["IModel"]

    def download(self, progress_callback: Callable[[float], None] = None) -> None: ...
    def get_path(self) -> str: ...
    def load(self) -> None: ...
    def unload(self) -> None: ...
    def remove_from_cache(self) -> None: ...
    def select_variant(self, variant: "IModel") -> None: ...

    # OpenAI clients are part of the legacy IModel contract — required, not optional.
    def get_chat_client(self) -> "openai.OpenAI": ...
    def get_audio_client(self) -> "openai.OpenAI": ...
    def get_embedding_client(self) -> "openai.OpenAI": ...
```

`Model` is provided as an alias of `IModel` for users who prefer the un-`I`-prefixed name on the new surface — but `IModel` is the canonical name and the type returned by `Catalog`.

**OpenAI client methods are part of the base contract.** They are not gated behind an install extra; if `openai` is not installed, calling `get_chat_client()` raises a clear `FoundryLocalException` instructing the user to `pip install openai`. The methods themselves always exist.

`ModelInfo` retains its current shape from [sdk/python/src/detail/model_data_types.py](../../../sdk/python/src/detail/model_data_types.py). New fields exposed by the C ABI are added as additional optional attributes; nothing is removed or renamed. The pydantic dependency may be replaced internally with `dataclasses` (see open questions) provided the public attribute names and types stay identical.

```python
@dataclass(frozen=True)
class ModelInfo:
    id: str
    name: str
    version: int
    alias: str
    uri: str | None
    device_type: DeviceType
    execution_provider: str | None
    task: str
    display_name: str | None
    publisher: str | None
    license: str | None
    filesize_mb: int | None
    supports_tool_calling: bool | None
    supports_reasoning: bool | None
    max_output_tokens: int | None

    def get_string_property(self, key: str) -> str | None: ...
    def get_int_property(self, key: str, default: int = 0) -> int: ...
```

`Model` is a concrete class, not an interface. Legacy `IModel` becomes `Model` — Python doesn't prefix interfaces with `I`, and the variants concept doesn't need an abstract base.

### 4.5 Items

Class hierarchy mirroring `sdk_v2/cs/src/Items/`:

```python
class Item:
    """Base class. Polymorphic via flItemType. Owns the native handle."""

    @property
    def item_type(self) -> ItemType: ...
    def close(self) -> None: ...
    # Internal: from_native(ptr, owns_handle) -> Item subclass

class TextItem(Item):
    def __init__(self, text: str, type: TextItemType = TextItemType.DEFAULT): ...
    @property
    def text(self) -> str: ...
    @property
    def type(self) -> TextItemType: ...

class MessageItem(Item):
    @classmethod
    def system(cls, content: str, name: str | None = None) -> "MessageItem": ...
    @classmethod
    def user(cls, content: str, name: str | None = None) -> "MessageItem": ...
    @classmethod
    def assistant(cls, content: str, name: str | None = None) -> "MessageItem": ...
    @classmethod
    def developer(cls, content: str, name: str | None = None) -> "MessageItem": ...

    def __init__(self, role: MessageRole, content: str | Iterable[Item], name: str | None = None): ...

    @property
    def role(self) -> MessageRole: ...
    @property
    def name(self) -> str | None: ...
    @property
    def parts(self) -> tuple[Item, ...]: ...

class BytesItem(Item):
    def __init__(self, data: bytes | memoryview, item_type: ItemType): ...
    @property
    def data(self) -> memoryview: ...

class ImageItem(Item):
    @classmethod
    def from_bytes(cls, data: bytes | memoryview, format: str) -> "ImageItem": ...
    @classmethod
    def from_uri(cls, uri: str, format: str | None = None) -> "ImageItem": ...

class AudioItem(Item):
    @classmethod
    def from_bytes(cls, data: bytes | memoryview, format: str,
                   sample_rate: int = 0, channels: int = 0) -> "AudioItem": ...
    @classmethod
    def from_uri(cls, uri: str, format: str | None = None,
                 sample_rate: int = 0, channels: int = 0) -> "AudioItem": ...

class ToolCallItem(Item):
    def __init__(self, call_id: str, name: str, arguments: str): ...
    @property
    def call_id(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def arguments(self) -> str: ...

class ToolResultItem(Item):
    def __init__(self, call_id: str, result: str): ...
    @property
    def call_id(self) -> str: ...
    @property
    def result(self) -> str: ...

class TensorItem(Item):
    @classmethod
    def from_numpy(cls, arr: "np.ndarray") -> "TensorItem":
        """Borrow the array's buffer; the deleter holds the array alive."""

    def to_numpy(self) -> "np.ndarray":
        """Zero-copy view into the tensor's buffer. Valid until the item is released."""

    @property
    def data_type(self) -> TensorDataType: ...
    @property
    def shape(self) -> tuple[int, ...]: ...
```

NumPy is an optional dependency (extra: `foundry-local[numpy]`). Tensor methods raise if NumPy is not installed.

### 4.6 Request and Response

```python
class Request:
    def __init__(self): ...
    def add_item(self, item: Item) -> "Request":
        """Transfers ownership of item to the request."""
    @property
    def item_count(self) -> int: ...
    def get_item(self, index: int) -> Item: ...
    def set_options(self, options: RequestOptions) -> "Request": ...
    def cancel(self) -> None: ...
    def close(self) -> None: ...

class Response:
    @property
    def items(self) -> list[Item]: ...
    @property
    def finish_reason(self) -> FinishReason: ...
    @property
    def usage(self) -> TokenUsage: ...
    def __iter__(self) -> Iterator[Item]: ...
    def close(self) -> None: ...

@dataclass(frozen=True)
class TokenUsage:
    prompt_tokens: int
    completion_tokens: int
    total_tokens: int
```

### 4.7 Sessions

```python
class Session:
    """Base class. Subclasses validate model task in __init__."""

    def __init__(self, model: Model): ...
    def set_options(self, options: RequestOptions) -> "Session": ...
    def process_request(self, request: Request) -> Response: ...
    def process_streaming_request(self, request: Request) -> Iterator[Item]:
        """Yields items as they are emitted by the native callback. Calls
        request.cancel() automatically if the iterator is closed early."""

    def close(self) -> None: ...

class ChatSession(Session):
    def __init__(self, model: Model):
        super().__init__(model)
        if model.info.task not in ("chat-completion", "vision-language-chat"):
            raise ValueError(...)

    def add_tool_definition(self, name: str, description: str,
                            json_schema: str) -> "ChatSession": ...
    @property
    def turn_count(self) -> int: ...
    def undo_turns(self, count: int) -> None: ...

class AudioSession(Session): ...
class EmbeddingsSession(Session): ...
```

Streaming iterator semantics:
1. The session installs a native callback that pushes items into a `queue.Queue`.
2. `process_streaming_request` calls `Session_ProcessRequest` on a worker thread (so the iterator can yield in real time on the caller's thread).
3. The iterator pops from the queue. When the worker thread finishes, the queue receives a sentinel and the iterator stops.
4. If the iterator is closed early (`break`, exception, `iterator.close()`), `request.cancel()` is invoked and the worker thread is joined.

### 4.8 OpenAI compatibility layer

`IModel.get_chat_client()`, `get_audio_client()`, and `get_embedding_client()` exist in the legacy SDK as part of the base interface. We preserve that. Implementation:

- The methods themselves always exist on `IModel`.
- They lazily import the `openai` package. If `openai` is not installed they raise `FoundryLocalException("openai package not installed; pip install openai")`.
- They boot the web service via `FoundryLocalManager.instance.start_web_service()` if not already running, and instantiate an `openai.OpenAI` with the bound URL plus the appropriate `model=` default.
- We do not vendor or fork the OpenAI client.

Distribution: `openai` is **not** in the default `dependencies` (it's a heavy transitive closure). It is in `[project.optional-dependencies]` as the `openai` extra, so `pip install foundry-local[openai]` pulls it in. But the methods on `IModel` always exist — only the underlying call fails if the dep isn't installed. This matches the legacy behaviour where the import would also fail at call time if `openai` was missing.

---

## 5. Native interop layer (`foundry_local._native`)

### 5.1 cffi cdef

`_native/build_cffi.py` declares the C API to cffi. Roughly:

```python
ffi = cffi.FFI()

ffi.cdef("""
    /* Pasted from foundry_local_c.h with macros expanded
       and SAL annotations stripped. */

    typedef enum flErrorCode { ... } flErrorCode;
    typedef enum flItemType  { ... } flItemType;
    /* ... all enums ... */

    typedef struct flUsage      { uint32_t version; int64_t prompt_tokens; ... } flUsage;
    typedef struct flTextData   { uint32_t version; const char* text; ... } flTextData;
    typedef struct flMessageData { ... } flMessageData;
    /* ... all data structs ... */

    typedef struct flApi flApi;
    typedef struct flItemApi flItemApi;
    /* ... opaque vtable structs declared as forward types ... */

    /* Each vtable struct is declared in full so its layout is known: */
    struct flApi {
        flStatus* (*Status_Create)(flErrorCode, const char*);
        void      (*Status_Release)(flStatus*);
        /* ... ~20 entries ... */
    };
    struct flItemApi      { /* ... */ };
    struct flInferenceApi { /* ... */ };
    struct flConfigurationApi { /* ... */ };
    struct flCatalogApi   { /* ... */ };
    struct flModelApi     { /* ... */ };

    /* Two exported symbols */
    const flApi* FoundryLocalGetApi(uint32_t version);
    const char*  FoundryLocalGetVersionString(void);
""")

ffi.set_source(
    "foundry_local._native._cffi",
    """#include "foundry_local/foundry_local_c.h" """,
    include_dirs=[<sdk_v2/cpp/include>],
    libraries=["foundry_local"],
    library_dirs=[<wheel platform dir>],
)
```

Maintenance: when V2 fields are added to a struct, paste the new declaration into the cdef and rebuild. The compile step verifies the layout against the actual header. We add a CI check that diffs the cdef against the header to catch drift.

### 5.2 API singleton

```python
# foundry_local/_native/api.py
from . import _cffi  # the generated extension
ffi = _cffi.ffi
lib = _cffi.lib

class _Api:
    def __init__(self):
        self._root = lib.FoundryLocalGetApi(1)
        if self._root == ffi.NULL:
            raise RuntimeError("FoundryLocalGetApi(1) returned NULL")
        self.root = self._root
        self.item      = self._root.GetItemApi()
        self.inference = self._root.GetInferenceApi()
        self.config    = self._root.GetConfigurationApi()
        self.catalog   = self._root.GetCatalogApi()
        self.model     = self._root.GetModelApi()

    def check_status(self, status):
        if status == ffi.NULL:
            return
        try:
            code = self.root.Status_GetErrorCode(status)
            msg_ptr = self.root.Status_GetErrorMessage(status)
            msg = ffi.string(msg_ptr).decode("utf-8") if msg_ptr != ffi.NULL else "Unknown error"
        finally:
            self.root.Status_Release(status)
        if code == lib.FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED:
            raise asyncio.CancelledError(msg)
        raise FoundryLocalException(msg, error_code=code)

api = _Api()
```

The `api` singleton is what every public class calls into. Exactly one place to look up vtable pointers; exactly one place to convert status into an exception.

### 5.3 Library loading

`_native/lib_loader.py` mirrors C# `DllLoader`. Search order:

1. `FOUNDRY_LOCAL_LIB_DIR` env var (override).
2. Platform-specific subdirectory inside the wheel:
   `foundry_local/_native/{win-x64,linux-x64,osx-arm64}/foundry_local.{dll,so,dylib}`
3. System library path (last resort).

The wheel ships ORT runtime DLLs co-located in the platform subdirectory. `prepare_native_dependencies()` in `_native/lib_loader.py` preloads `onnxruntime` then `onnxruntime-genai` by absolute path from that directory before `foundry_local` is loaded, so the native lib's `DT_NEEDED` / import-table entries resolve to the wheel-shipped copies on every platform. See [`cpp/docs/OrtRuntimeLoading.md`](../../cpp/docs/OrtRuntimeLoading.md) for the cross-binding contract.

### 5.4 Callback trampolines

```python
# foundry_local/_native/callbacks.py
@ffi.callback("int(float, void*)")
def _progress_trampoline(value, user_data):
    handle = ffi.from_handle(user_data)
    try:
        return 0 if handle.callback(value) is None else 1
    except Exception as ex:
        handle.exception = ex
        return 1

@ffi.callback("int(flStreamingCallbackData, void*)")
def _streaming_trampoline(event, user_data):
    handle = ffi.from_handle(user_data)
    try:
        # Drain queue and push to Python queue
        ...
        return 1 if handle.cancelled else 0
    except Exception as ex:
        handle.exception = ex
        return 1
```

The Python callable, queue, and exception slot live in a `CallbackHandle` object whose pointer is passed as `user_data`. cffi's `ffi.new_handle(obj)` returns a `void*` and keeps `obj` alive as long as the handle is live.

---

## 6. Packaging

### 6.1 pyproject.toml

```toml
[project]
name = "foundry-local"
dynamic = ["version"]
requires-python = ">=3.11,<3.15"
dependencies = ["cffi>=1.16"]

[project.optional-dependencies]
numpy = ["numpy>=1.23"]
openai = ["openai>=1.0"]

[build-system]
requires = ["setuptools>=68", "cffi>=1.16", "wheel"]
build-backend = "build_backend"
backend-path = ["."]
```

### 6.2 Build backend

The existing `sdk_v2/python/build_backend.py` is extended to:

1. Run the C++ build (or pull the prebuilt `foundry_local.dll` and ORT DLLs) into a staging directory.
2. Run `cffi`'s out-of-line build to emit `_cffi.*.pyd` / `.so` from `_native/build_cffi.py`.
3. Bundle binaries into the wheel under `foundry_local/_native/<platform>/`.

Wheel tags: `win_amd64`, `manylinux_2_28_x86_64`, `manylinux_2_34_aarch64`, `macosx_11_0_arm64`. No pure-Python wheel — the package is platform-specific.

### 6.3 Type stubs

The package ships type hints inline (`py.typed` marker). All public classes have full annotations. No separate `.pyi` files in v1.

---

## 7. Implementation phases

Each phase ends with running tests and a green build. No phase is "done" until the previous phase is.

| Phase | Scope | Validation |
|------:|-------|-----------|
| **1** | `_native/` cffi plumbing only. `build_cffi.py`, `lib_loader.py`, `api.py`, status conversion. No public API yet. | `FoundryLocalGetVersionString()` round-trips through Python. Vtable structs introspect cleanly. |
| **2** | `Configuration` + `FoundryLocalManager` + `Catalog` + `IModel` + `ModelInfo` + `EpInfo` + `EpDownloadResult` + `LogLevel` + `FoundryLocalException`. EP discovery. **All legacy public API at parity.** | **The legacy `sdk/python` test suite, copied into `sdk_v2/python/test/`, passes unmodified** (other than the import path). Integration test: create config, init manager, list models, look up by alias, read `ModelInfo` properties. |
| **3** | Item hierarchy: `Item`, `TextItem`, `MessageItem`, `BytesItem`, `ImageItem`, `AudioItem`, `ToolCallItem`, `ToolResultItem`, `TensorItem`. | Round-trip: create → set → get → release. Memory pressure test confirms no leaks. |
| **4** | `Request`, `Response`, `Session`, `ChatSession`. Synchronous `process_request`. Tool definitions, `turn_count`, `undo_turns`. | Multi-turn chat integration test passes against a CPU model (mirrors `ChatMultiTurnSession` from C++). |
| **5** | Streaming. Native callback trampoline → `queue.Queue` → iterator. Cancellation. | Streaming integration test produces a token-by-token transcript. Cancel test confirms cooperative shutdown. |
| **6** | `AudioSession`, `EmbeddingsSession`, OpenAI compat layer. | Audio transcription example works. `model.get_chat_client()` returns a working `openai.OpenAI` instance. |
| **7** | Packaging finalisation, wheel CI for all platforms, examples, docs. | Wheels install cleanly on a fresh venv; integration suite passes on Windows + Linux + macOS. |

Phase 1 is independent of all others and can begin immediately. Subsequent phases depend on the previous one.

---

## 8. Open questions and decisions to ratify

The parity rule (§1) settles several questions that were previously open. The remaining points are now mostly decided; this section is the record.

1. **Drop `pydantic` runtime dependency.** Decided: **yes, drop it.** Replace `BaseModel` types with frozen `@dataclass` types of the same name and the same public attributes.

   `pydantic` was used in the legacy SDK to validate JSON coming back from the old JSON-command FFI \u2014 it was load-bearing for *parsing*, not part of the user contract. With the new C ABI we get typed structs directly; there is nothing left to validate at runtime. Dropping pydantic removes a heavy transitive dep and its v1\u2192v2 churn from every consumer of `foundry-local`.

   Contract we keep:
   - All public attribute names and types on `EpInfo`, `EpDownloadResult`, `ModelInfo`, `PromptTemplate`, `Runtime`, `Parameter`, `ModelSettings`.\n   - `Optional[...]` semantics and field defaults.\n   - Frozen / immutable behaviour (legacy used `ConfigDict(frozen=True)` in places).\n\n   What goes away:\n   - `pydantic`-specific methods (`.model_dump`, `.model_validate`, `.model_fields`, `.model_json_schema`).\n   - Construction-time coercion of mistyped inputs.\n\n   Migration shim: each data class gets `to_dict()` (delegating to `dataclasses.asdict`) and a `from_dict(d)` classmethod so the rare user who called `.model_dump()` has a one-line fix. Called out in migration notes.

2. **Async wrappers in v1?** Decided: **no.** Legacy is sync; new surface is sync. Add `aio` submodule later if requested. Not a parity concern.

3. **Streaming idiom — sync iterator vs async generator?** Decided: **sync iterator.** This is the idiomatic Python pattern for streaming APIs — the OpenAI Python SDK itself uses sync iterators by default (with a separate `AsyncOpenAI` client for async). It composes with regular `for` loops, `itertools`, and `asyncio.to_thread` for users who need async.

4. **NumPy dependency.** Decided: **optional extra** (`pip install foundry-local[numpy]`). NumPy is a large transitive closure and is not core to chat / audio / embeddings users — it's only relevant for `TensorItem`, which is new surface. `TensorItem.from_numpy()` / `to_numpy()` raise `FoundryLocalException("numpy not installed; pip install foundry-local[numpy]")` if NumPy is missing.

5. **CPython only, or PyPy too?** Decided: **CPython only** in v1. Legacy is implicitly CPython-only too. Add PyPy if a real user asks.

6. **Supported Python versions.** Decided: **align with ONNX Runtime and ONNX Runtime GenAI's supported Python range.** Both a floor *and* a ceiling — we don't claim support for a version those packages won't install on. ORT and ORT GenAI both support Python 3.11–3.14, so v1 ships as `requires-python = ">=3.11,<3.15"`. Revisit when ORT/ORT GenAI bump their range.

7. **Internal interop replacement strategy.** Decided: **(B) reimplement on `_native/`.** Remove `_core_interop` and all of `detail/` from the legacy SDK entirely. Private modules were private; no shims, no transitional layer.

8. **`__init__.py` re-export list.** Decided: **clean public `__all__`, no internal classes leaking.** The legacy exports (`Configuration`, `FoundryLocalManager`, `__version__`) stay. New exports add the documented public surface only:

   ```python
   __all__ = [
       # Legacy
       "Configuration", "FoundryLocalManager", "__version__",
       "Catalog", "IModel", "Model",  # Model is an alias of IModel
       "ModelInfo", "PromptTemplate", "Runtime", "Parameter", "ModelSettings",
       "EpInfo", "EpDownloadResult",
       "LogLevel", "FoundryLocalException",
       # New surface
       "Session", "ChatSession", "AudioSession", "EmbeddingsSession",
       "Request", "Response", "TokenUsage",
       "Item", "TextItem", "MessageItem", "BytesItem", "ImageItem",
       "AudioItem", "ToolCallItem", "ToolResultItem", "TensorItem",
       "ItemType", "MessageRole", "FinishReason", "DeviceType",
       "TensorDataType", "TextItemType",
   ]
   ```

   `_native`, `detail`-style helpers, callback trampolines, the API singleton, and anything else internal stay private (leading underscore module / not in `__all__`).

Resolved by the parity rule (no longer open):
- ~~Rename `IModel` → `Model`~~ → No. Keep `IModel`. Provide `Model = IModel` alias.
- ~~`Configuration` fluent setters~~ → No. Keep the legacy constructor signature exactly. Fluent setters are additive only.
- ~~`FoundryLocalManager.create()` / `instance()` method~~ → No. Keep `FoundryLocalManager(config)` + `initialize()` static + `instance` class attribute exactly as legacy.
- ~~`download_and_register_eps` returns `None`~~ → No. Returns `EpDownloadResult` as legacy does.
- ~~OpenAI methods as optional layer~~ → No. They're on `IModel` always; only the optional install extra pulls in the `openai` package.
- ~~Package/import names~~ → Distribution `foundry-local`, import `foundry_local`. Matches legacy.
