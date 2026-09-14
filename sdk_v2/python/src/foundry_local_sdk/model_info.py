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


@dataclass(frozen=True)
class ModelInfo:
    """Point-in-time catalog metadata for a single model variant."""

    id: str
    name: str
    version: int
    alias: str
    display_name: str | None
    provider_type: str
    uri: str
    model_type: str
    prompt_template: PromptTemplate | None
    """.. deprecated::
        ``prompt_template`` is an internal model implementation detail and will
        be removed in a future release. It is no longer populated from native
        catalog data; ``ChatSession`` applies templates automatically.
    """
    publisher: str | None
    model_settings: ModelSettings | None
    license: str | None
    license_description: str | None
    task: str | None
    runtime: Runtime | None
    file_size_mb: int | None
    supports_tool_calling: bool | None
    max_output_tokens: int | None
    min_fl_version: str | None
    created_at_unix: int
    context_length: int | None
    input_modalities: str | None
    output_modalities: str | None
    capabilities: str | None

    def get_string_property(self, key: str) -> str | None:
        """Get a named property by key (for forward compatibility)."""
        properties = getattr(self, "_string_properties", {})
        if key in properties:
            return properties[key]
        field_name = {
            "model_provider": "provider_type",
            "type": "model_type",
        }.get(key, key.replace("-", "_"))
        value = getattr(self, field_name, None)
        return value if isinstance(value, str) else None

    def get_int_property(self, key: str, default: int = 0) -> int:
        """Get a named property as int (for forward compatibility)."""
        properties = getattr(self, "_int_properties", {})
        if key in properties:
            return properties[key]
        field_name = {
            "filesize_mb": "file_size_mb",
        }.get(key, key.replace("-", "_"))
        value = getattr(self, field_name, None)
        return int(value) if value is not None else default


class ModelInfoBuilder:
    """Caller-owned mutable native metadata for local model registration."""

    __slots__ = ("_ptr", "_closed", "_string_properties", "_int_properties")

    def __init__(self) -> None:
        from foundry_local_sdk._native.api import api, ffi

        out = ffi.new("flModelInfo**")
        api.check_status(api.model.CreateModelInfo(out))
        if out[0] == ffi.NULL:
            raise RuntimeError("CreateModelInfo returned a null pointer")

        self._ptr: object = out[0]
        self._closed = False
        self._string_properties: dict[str, str] = {}
        self._int_properties: dict[str, int] = {}

    def _ensure_open(self) -> None:
        if self._closed:
            raise RuntimeError("ModelInfoBuilder is closed")

    @property
    def _native_ptr(self) -> object:
        """Native handle for internal binding calls."""
        self._ensure_open()
        return self._ptr

    def set_string_property(self, key: str, value: str) -> ModelInfoBuilder:
        """Set a string metadata property and return ``self``.

        Well-known keys are documented in the BYOM section of the SDK README;
        arbitrary keys are preserved for forward compatibility.
        """
        self._ensure_open()
        from foundry_local_sdk._native.api import api

        key_bytes = key.encode("utf-8")
        value_bytes = value.encode("utf-8")
        api.check_status(api.model.Info_SetStringProperty(self._ptr, key_bytes, value_bytes))
        self._string_properties[key] = value
        return self

    def set_int_property(self, key: str, value: int) -> ModelInfoBuilder:
        """Set an integer metadata property and return ``self``."""
        self._ensure_open()
        from foundry_local_sdk._native.api import api

        key_bytes = key.encode("utf-8")
        api.check_status(api.model.Info_SetIntProperty(self._ptr, key_bytes, value))
        self._int_properties[key] = value
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

    def close(self) -> None:
        """Release caller-owned metadata exactly once and invalidate this wrapper."""
        if self._closed:
            return

        from foundry_local_sdk._native.api import api

        api.model.ReleaseModelInfo(self._ptr)

        self._closed = True

    def __enter__(self) -> ModelInfoBuilder:
        self._ensure_open()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
