# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

from abc import ABC, abstractmethod
from typing import TYPE_CHECKING, Callable

from typing_extensions import deprecated

from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.model_info import DeviceType, ModelInfo, Runtime

if TYPE_CHECKING:
    # These modules are implemented in Phase 6 (openai submodule).
    from foundry_local_sdk.openai.audio_client import AudioClient
    from foundry_local_sdk.openai.chat_client import ChatClient
    from foundry_local_sdk.openai.embedding_client import EmbeddingClient


# ---------------------------------------------------------------------------
# Public abstract interface
# ---------------------------------------------------------------------------


class IModel(ABC):
    """Abstract interface for a model that can be downloaded, loaded, and used for inference."""

    @property
    @abstractmethod
    def id(self) -> str:
        """Unique model id."""

    @property
    @abstractmethod
    def alias(self) -> str:
        """Model alias."""

    @property
    @abstractmethod
    def info(self) -> ModelInfo:
        """Full model metadata."""

    @property
    @abstractmethod
    def is_cached(self) -> bool:
        """True if the model is present in the local cache."""

    @property
    @abstractmethod
    def is_loaded(self) -> bool:
        """True if the model is loaded into memory."""

    @property
    @abstractmethod
    def context_length(self) -> int | None:
        """Maximum context length (in tokens) supported by the model, or ``None`` if unknown."""

    @property
    @abstractmethod
    def input_modalities(self) -> str | None:
        """Comma-separated input modalities (e.g. ``"text,image"``), or ``None`` if unknown."""

    @property
    @abstractmethod
    def output_modalities(self) -> str | None:
        """Comma-separated output modalities (e.g. ``"text"``), or ``None`` if unknown."""

    @property
    @abstractmethod
    def capabilities(self) -> str | None:
        """Comma-separated capability tags (e.g. ``"chat,completion"``), or ``None`` if unknown."""

    @property
    @abstractmethod
    def supports_tool_calling(self) -> bool | None:
        """Whether the model supports tool/function calling, or ``None`` if unknown."""

    @abstractmethod
    def download(self, progress_callback: Callable[[float], None] | None = None) -> None:
        """Download the model to the local cache if not already present.

        Args:
            progress_callback: Optional callback receiving download progress as
                a percentage (0.0–100.0).
        """

    @abstractmethod
    def get_path(self) -> str:
        """Return the local file-system path to the cached model.

        Returns:
            Path of the model directory.
        """

    @abstractmethod
    def load(self) -> None:
        """Load the model into memory if not already loaded."""

    @abstractmethod
    def remove_from_cache(self) -> None:
        """Remove the model from the local cache."""

    @abstractmethod
    def unload(self) -> None:
        """Unload the model if currently loaded."""

    @abstractmethod
    def get_chat_client(self) -> "ChatClient":
        """Get an OpenAI API-compatible ChatClient.

        .. deprecated::
            Use ``ChatSession`` instead; the OpenAI direct client will be removed at the end of 2026.
            OpenAI types remain supported for the web-server path.
        """

    @abstractmethod
    def get_audio_client(self) -> "AudioClient":
        """Get an OpenAI API-compatible AudioClient.

        .. deprecated::
            Use ``AudioSession`` instead; the OpenAI direct client will be removed at the end of 2026.
            OpenAI types remain supported for the web-server path.
        """

    @abstractmethod
    def get_embedding_client(self) -> "EmbeddingClient":
        """Get an OpenAI API-compatible EmbeddingClient.

        .. deprecated::
            Use ``EmbeddingsSession`` instead; the OpenAI direct client will be removed at the end of 2026.
            OpenAI types remain supported for the web-server path.
        """

    @property
    @abstractmethod
    def variants(self) -> list[IModel]:
        """Variants of the model optimised for different devices."""

    @abstractmethod
    def select_variant(self, variant: IModel) -> None:
        """Select a model variant from ``variants`` to use for IModel operations.

        Args:
            variant: Model variant to select.  Must be one of the items in ``variants``.

        Raises:
            FoundryLocalException: If variant is not valid for this model.
        """


# Public alias kept for backwards compatibility with callers who import ``Model``.
Model = IModel


# ---------------------------------------------------------------------------
# Private helper — build ModelInfo from a native flModel* pointer
# ---------------------------------------------------------------------------


def _model_info_from_native(
    native_model_ptr: object,
    *,
    string_properties: dict[str, str] | None = None,
    int_properties: dict[str, int] | None = None,
) -> ModelInfo:
    """Read native metadata into a safe point-in-time value snapshot."""
    from foundry_local_sdk._native.api import api, ffi  # local to avoid circular imports

    info_out = ffi.new("const flModelInfo**")
    api.check_status(api.model.GetInfo(native_model_ptr, info_out))
    if info_out[0] == ffi.NULL:
        raise FoundryLocalException("GetInfo returned no model metadata.")
    info = info_out[0]

    def get_str(key: str) -> str | None:
        ptr = api.model.Info_GetStringProperty(info, key.encode("utf-8"))
        return ffi.string(ptr).decode("utf-8") if ptr != ffi.NULL else None

    def get_int(key: str, default: int = -1) -> int:
        return int(api.model.Info_GetIntProperty(info, key.encode("utf-8"), default))

    uri_ptr = api.model.Info_GetUri(info)
    ep_ptr = api.model.Info_GetExecutionProvider(info)
    task_ptr = api.model.Info_GetTask(info)
    filesize = get_int("filesize_mb")
    max_tokens = get_int("max_output_tokens")
    context_length = get_int("context_length")
    supports_tool_calling = get_int("supports_tool_calling", -1)
    device_type = {1: DeviceType.CPU, 2: DeviceType.GPU, 3: DeviceType.NPU}.get(
        int(api.model.Info_GetDeviceType(info))
    )

    snapshot = ModelInfo(
        id=ffi.string(api.model.Info_GetId(info)).decode("utf-8"),
        name=ffi.string(api.model.Info_GetName(info)).decode("utf-8"),
        version=int(api.model.Info_GetVersion(info)),
        alias=ffi.string(api.model.Info_GetAlias(info)).decode("utf-8"),
        display_name=get_str("display_name"),
        provider_type=get_str("model_provider") or "",
        uri=ffi.string(uri_ptr).decode("utf-8") if uri_ptr != ffi.NULL else "",
        model_type=get_str("type") or "",
        prompt_template=None,
        publisher=get_str("publisher"),
        model_settings=None,
        license=get_str("license"),
        license_description=get_str("license_description"),
        task=ffi.string(task_ptr).decode("utf-8") if task_ptr != ffi.NULL else None,
        runtime=Runtime(
            device_type=device_type,
            execution_provider=ffi.string(ep_ptr).decode("utf-8") if ep_ptr != ffi.NULL else "",
        ),
        file_size_mb=filesize if filesize >= 0 else None,
        supports_tool_calling=None if supports_tool_calling < 0 else bool(supports_tool_calling),
        max_output_tokens=max_tokens if max_tokens >= 0 else None,
        min_fl_version=get_str("min_fl_version"),
        created_at_unix=max(get_int("created_at_unix", 0), 0),
        context_length=context_length if context_length >= 0 else None,
        input_modalities=get_str("input_modalities"),
        output_modalities=get_str("output_modalities"),
        capabilities=get_str("capabilities"),
    )
    object.__setattr__(snapshot, "_string_properties", dict(string_properties or {}))
    object.__setattr__(snapshot, "_int_properties", dict(int_properties or {}))
    return snapshot


# ---------------------------------------------------------------------------
# Private concrete implementations — not exported
# ---------------------------------------------------------------------------


class _ModelImpl(IModel):
    """Single native ``flModel*`` variant.  Does NOT own the pointer — Catalog does."""

    def __init__(
        self,
        native_ptr: object,
        *,
        parent: object | None = None,
        string_properties: dict[str, str] | None = None,
        int_properties: dict[str, int] | None = None,
    ) -> None:
        self._ptr = native_ptr
        # Keep the owning Catalog alive while this model exists. The native flModel*
        # is owned by the catalog; without this reference, GC could release the
        # catalog (and the manager behind it) first and dangle our pointer.
        self._parent = parent
        self._string_properties = dict(string_properties or {})
        self._int_properties = dict(int_properties or {})
        # Callback references — stored to prevent premature GC.
        self._progress_cb = None
        self._progress_cb_handle = None

    @property
    def _native_ptr(self) -> object:
        """Raw native ``flModel*`` pointer.

        Internal use only; keep an ``IModel`` reference alive while using it.
        """
        self._ensure_manager_open()
        return self._ptr

    def _ensure_manager_open(self) -> None:
        parent = self._parent
        manager = getattr(parent, "_parent", None)
        if manager is not None and getattr(manager, "_native_manager", None) is None:
            raise RuntimeError("FoundryLocalManager is closed")

    # ------------------------------------------------------------------
    # Identity properties — read from native ModelInfo
    # ------------------------------------------------------------------

    @property
    def id(self) -> str:
        return self.info.id

    @property
    def alias(self) -> str:
        return self.info.alias

    @property
    def info(self) -> ModelInfo:
        self._ensure_manager_open()
        return _model_info_from_native(
            self._ptr,
            string_properties=self._string_properties,
            int_properties=self._int_properties,
        )

    # ------------------------------------------------------------------
    # Live state properties — always go to native for fresh data
    # ------------------------------------------------------------------

    @property
    def is_cached(self) -> bool:
        self._ensure_manager_open()
        from foundry_local_sdk._native.api import api, ffi

        out = ffi.new("int*")
        api.check_status(api.model.IsCached(self._ptr, out))
        return bool(out[0])

    @property
    def is_loaded(self) -> bool:
        self._ensure_manager_open()
        from foundry_local_sdk._native.api import api, ffi

        out = ffi.new("int*")
        api.check_status(api.model.IsLoaded(self._ptr, out))
        return bool(out[0])

    # ------------------------------------------------------------------
    # Convenience pass-throughs from ModelInfo
    # ------------------------------------------------------------------

    @property
    def context_length(self) -> int | None:
        return self.info.context_length

    @property
    def input_modalities(self) -> str | None:
        return self.info.input_modalities

    @property
    def output_modalities(self) -> str | None:
        return self.info.output_modalities

    @property
    def capabilities(self) -> str | None:
        return self.info.capabilities

    @property
    def supports_tool_calling(self) -> bool | None:
        return self.info.supports_tool_calling

    # ------------------------------------------------------------------
    # Model lifecycle
    # ------------------------------------------------------------------

    def download(self, progress_callback: Callable[[float], None] | None = None) -> None:
        self._ensure_manager_open()
        from foundry_local_sdk._native.api import api, ffi

        cb = ffi.NULL
        user_data = ffi.NULL

        if progress_callback is not None:
            self._progress_cb_handle = ffi.new_handle(progress_callback)

            @ffi.callback("flProgressCallback")
            def _cb(value: float, ud: object) -> int:
                try:
                    fn = ffi.from_handle(ud)
                    fn(float(value))
                    return 0
                except Exception:
                    return 1

            self._progress_cb = _cb  # keep alive
            cb = _cb
            user_data = self._progress_cb_handle

        api.check_status(api.model.Download(self._ptr, cb, user_data))

    def get_path(self) -> str:
        self._ensure_manager_open()
        from foundry_local_sdk._native.api import api, ffi

        out = ffi.new("const char**")
        api.check_status(api.model.GetPath(self._ptr, out))
        return ffi.string(out[0]).decode("utf-8") if out[0] != ffi.NULL else ""

    def load(self) -> None:
        self._ensure_manager_open()
        from foundry_local_sdk._native.api import api

        api.check_status(api.model.Load(self._ptr))

    def unload(self) -> None:
        self._ensure_manager_open()
        from foundry_local_sdk._native.api import api

        api.check_status(api.model.Unload(self._ptr))

    def remove_from_cache(self) -> None:
        self._ensure_manager_open()
        from foundry_local_sdk._native.api import api

        api.check_status(api.model.RemoveFromCache(self._ptr))

    # ------------------------------------------------------------------
    # Variants — delegated to the native layer
    # ------------------------------------------------------------------

    @property
    def variants(self) -> list[IModel]:
        self._ensure_manager_open()
        """Return all device-optimised variants for this model.

        Calls the native ``GetVariants`` vtable function.  A model that is
        already a single variant returns a list containing only itself.
        """
        from foundry_local_sdk._native.api import api, ffi

        ml_out = ffi.new("flModelList**")
        api.check_status(api.model.GetVariants(self._ptr, ml_out))
        ml = ml_out[0]
        try:
            count = api.root.ModelList_Size(ml)
            # Variants share this model's catalog as their parent — chain to the
            # catalog, not to this model, so the reference graph stays flat.
            return [_ModelImpl(api.root.ModelList_GetAt(ml, i), parent=self._parent) for i in range(count)]
        finally:
            api.root.ModelList_Release(ml)

    def select_variant(self, variant: IModel) -> None:
        self._ensure_manager_open()
        """Select a specific variant.  Delegates to the native ``SelectVariant`` vtable.

        Args:
            variant: Must be one of the items returned by ``variants``.

        Raises:
            FoundryLocalException: If variant is not valid for this model.
        """
        if not isinstance(variant, _ModelImpl):
            raise FoundryLocalException("variant must be an IModel returned from this model's variants.")
        from foundry_local_sdk._native.api import api

        api.check_status(api.model.SelectVariant(self._ptr, variant._ptr))

    # ------------------------------------------------------------------
    # OpenAI client factories
    # ------------------------------------------------------------------

    @deprecated("The OpenAI direct client is deprecated and will be removed at the end of 2026; use ChatSession. "
                "OpenAI types remain supported for the web-server path.")
    def get_chat_client(self) -> "ChatClient":
        from foundry_local_sdk.openai.chat_client import ChatClient
        return ChatClient(self.info.id, self)

    @deprecated("The OpenAI direct client is deprecated and will be removed at the end of 2026; use AudioSession. "
                "OpenAI types remain supported for the web-server path.")
    def get_audio_client(self) -> "AudioClient":
        from foundry_local_sdk.openai.audio_client import AudioClient
        return AudioClient(self.info.id, self)

    @deprecated("The OpenAI direct client is deprecated and will be removed at the end of 2026; use EmbeddingsSession. "
                "OpenAI types remain supported for the web-server path.")
    def get_embedding_client(self) -> "EmbeddingClient":
        from foundry_local_sdk.openai.embedding_client import EmbeddingClient
        return EmbeddingClient(self.info.id, self)
