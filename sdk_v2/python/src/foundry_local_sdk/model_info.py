# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum

from typing_extensions import deprecated


class DeviceType(StrEnum):
    """Device types supported by model variants."""

    CPU = "CPU"
    GPU = "GPU"
    NPU = "NPU"


@deprecated(
    "PromptTemplate is an internal model implementation detail and will be removed in a future release. "
    "Templates are applied automatically by ChatSession."
)
@dataclass(frozen=True)
class PromptTemplate:
    """Prompt template strings for system, user, assistant, and raw prompt roles.

    .. deprecated::
        ``PromptTemplate`` is an internal model implementation detail and will be
        removed in a future release. Templates are applied automatically by
        ``ChatSession``; callers should not need to consume them directly.
    """

    system: str | None = None
    user: str | None = None
    assistant: str | None = None
    prompt: str | None = None


@dataclass(frozen=True)
class Runtime:
    """Runtime configuration specifying the device type and execution provider.

    ``device_type`` is ``None`` when the native side reports ``FOUNDRY_LOCAL_DEVICE_NOTSET`` (or any value the
    Python SDK does not recognise) — do not assume CPU as a default.
    """

    device_type: DeviceType | None
    execution_provider: str


@dataclass(frozen=True)
class Parameter:
    """A named parameter with an optional string value."""

    name: str
    value: str | None = None


@dataclass(frozen=True)
class ModelSettings:
    """Model-specific settings containing a list of parameters."""

    parameters: list[Parameter] | None = None


class ModelInfo:
    """Native model metadata used for inspection and local model registration.

    Default construction creates caller-owned, mutable metadata. Use
    :meth:`set_string_property` and :meth:`set_int_property` to populate it,
    then pass it to ``Catalog.register_model``. The object is a context manager
    and should be closed when it is no longer needed.

    ``IModel.info`` returns a read-only borrowed view instead. That view keeps
    its model and catalog alive and never releases the catalog-owned native
    metadata.
    """

    __slots__ = ("_ptr", "_owns", "_mutable", "_parent", "_closed")

    def __init__(self) -> None:
        from foundry_local_sdk._native.api import api, ffi

        out = ffi.new("flModelInfo**")
        api.check_status(api.model.CreateModelInfo(out))
        if out[0] == ffi.NULL:
            raise RuntimeError("CreateModelInfo returned a null pointer")

        self._ptr: object = out[0]
        self._owns = True
        self._mutable = True
        self._parent: object | None = None
        self._closed = False

    @classmethod
    def _from_native(cls, native_ptr: object, *, parent: object) -> ModelInfo:
        """Create a borrowed read-only view over catalog-owned metadata."""
        from foundry_local_sdk._native.api import ffi

        if native_ptr == ffi.NULL:
            raise ValueError("Cannot wrap a null flModelInfo pointer")

        instance = cls.__new__(cls)
        instance._ptr = native_ptr
        instance._owns = False
        instance._mutable = False
        # The native metadata belongs to the model, which belongs to its catalog.
        instance._parent = parent
        instance._closed = False
        return instance

    def _ensure_open(self) -> None:
        if self._closed:
            raise RuntimeError("ModelInfo is closed")

    @property
    def _native_ptr(self) -> object:
        """Native handle for internal binding calls."""
        self._ensure_open()
        return self._ptr

    def set_string_property(self, key: str, value: str) -> ModelInfo:
        """Set a string metadata property and return ``self``.

        Well-known keys are documented in the BYOM section of the SDK README;
        arbitrary keys are preserved for forward compatibility.
        """
        self._ensure_open()
        if not self._mutable:
            raise RuntimeError("ModelInfo returned by IModel.info is read-only")

        from foundry_local_sdk._native.api import api

        key_bytes = key.encode("utf-8")
        value_bytes = value.encode("utf-8")
        api.check_status(api.model.Info_SetStringProperty(self._ptr, key_bytes, value_bytes))
        return self

    def set_int_property(self, key: str, value: int) -> ModelInfo:
        """Set an integer metadata property and return ``self``."""
        self._ensure_open()
        if not self._mutable:
            raise RuntimeError("ModelInfo returned by IModel.info is read-only")

        from foundry_local_sdk._native.api import api

        key_bytes = key.encode("utf-8")
        api.check_status(api.model.Info_SetIntProperty(self._ptr, key_bytes, value))
        return self

    def get_string_property(self, key: str) -> str | None:
        """Get a string metadata property, or ``None`` when it is not set."""
        self._ensure_open()
        from foundry_local_sdk._native.api import api, ffi

        key_bytes = key.encode("utf-8")
        value = api.model.Info_GetStringProperty(self._ptr, key_bytes)
        return ffi.string(value).decode("utf-8") if value != ffi.NULL else None

    def get_int_property(self, key: str, default: int = 0) -> int:
        """Get an integer metadata property, or ``default`` when it is not set."""
        self._ensure_open()
        from foundry_local_sdk._native.api import api

        key_bytes = key.encode("utf-8")
        return int(api.model.Info_GetIntProperty(self._ptr, key_bytes, default))

    @property
    def id(self) -> str:
        """Unique model ID. Empty on mutable registration metadata."""
        self._ensure_open()
        from foundry_local_sdk._native.api import api, ffi

        value = api.model.Info_GetId(self._ptr)
        return ffi.string(value).decode("utf-8") if value != ffi.NULL else ""

    @property
    def name(self) -> str:
        """Model variant name. Empty on mutable registration metadata."""
        self._ensure_open()
        from foundry_local_sdk._native.api import api, ffi

        value = api.model.Info_GetName(self._ptr)
        return ffi.string(value).decode("utf-8") if value != ffi.NULL else ""

    @property
    def version(self) -> int:
        """Model version. Identity is derived from ``model_id`` during registration."""
        self._ensure_open()
        from foundry_local_sdk._native.api import api

        return int(api.model.Info_GetVersion(self._ptr))

    @property
    def alias(self) -> str:
        """Model alias. Empty until registration derives it from ``model_id``."""
        self._ensure_open()
        from foundry_local_sdk._native.api import api, ffi

        value = api.model.Info_GetAlias(self._ptr)
        return ffi.string(value).decode("utf-8") if value != ffi.NULL else ""

    @property
    def uri(self) -> str:
        """Model URI, or an empty string when absent."""
        self._ensure_open()
        from foundry_local_sdk._native.api import api, ffi

        value = api.model.Info_GetUri(self._ptr)
        return ffi.string(value).decode("utf-8") if value != ffi.NULL else ""

    @property
    def display_name(self) -> str | None:
        return self.get_string_property("display_name")

    @property
    def provider_type(self) -> str:
        return self.get_string_property("model_provider") or ""

    @property
    def model_type(self) -> str:
        return self.get_string_property("type") or ""

    @property
    @deprecated(
        "PromptTemplate is an internal model implementation detail and will be removed in a future release. "
        "Templates are applied automatically by ChatSession."
    )
    def prompt_template(self) -> PromptTemplate | None:
        """Deprecated model prompt template; always ``None``."""
        return None

    @property
    def publisher(self) -> str | None:
        return self.get_string_property("publisher")

    @property
    def model_settings(self) -> ModelSettings | None:
        # Complex model settings parsing remains intentionally deferred.
        return None

    @property
    def license(self) -> str | None:
        return self.get_string_property("license")

    @property
    def license_description(self) -> str | None:
        return self.get_string_property("license_description")

    @property
    def task(self) -> str | None:
        self._ensure_open()
        from foundry_local_sdk._native.api import api, ffi

        value = api.model.Info_GetTask(self._ptr)
        return ffi.string(value).decode("utf-8") if value != ffi.NULL else None

    @property
    def runtime(self) -> Runtime:
        self._ensure_open()
        from foundry_local_sdk._native.api import api, ffi

        device_type = {
            1: DeviceType.CPU,
            2: DeviceType.GPU,
            3: DeviceType.NPU,
        }.get(int(api.model.Info_GetDeviceType(self._ptr)))
        ep = api.model.Info_GetExecutionProvider(self._ptr)
        execution_provider = ffi.string(ep).decode("utf-8") if ep != ffi.NULL else ""
        return Runtime(device_type=device_type, execution_provider=execution_provider)

    @property
    def file_size_mb(self) -> int | None:
        value = self.get_int_property("filesize_mb", -1)
        return value if value >= 0 else None

    @property
    def supports_tool_calling(self) -> bool | None:
        value = self.get_int_property("supports_tool_calling", -1)
        return None if value < 0 else bool(value)

    @property
    def max_output_tokens(self) -> int | None:
        value = self.get_int_property("max_output_tokens", -1)
        return value if value >= 0 else None

    @property
    def min_fl_version(self) -> str | None:
        return self.get_string_property("min_fl_version")

    @property
    def created_at_unix(self) -> int:
        return max(self.get_int_property("created_at_unix", 0), 0)

    @property
    def context_length(self) -> int | None:
        value = self.get_int_property("context_length", -1)
        return value if value >= 0 else None

    @property
    def input_modalities(self) -> str | None:
        return self.get_string_property("input_modalities")

    @property
    def output_modalities(self) -> str | None:
        return self.get_string_property("output_modalities")

    @property
    def capabilities(self) -> str | None:
        return self.get_string_property("capabilities")

    def close(self) -> None:
        """Release caller-owned metadata exactly once and invalidate this wrapper."""
        if self._closed:
            return

        if self._owns:
            from foundry_local_sdk._native.api import api

            api.model.ReleaseModelInfo(self._ptr)

        self._closed = True
        self._parent = None

    def __enter__(self) -> ModelInfo:
        self._ensure_open()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
