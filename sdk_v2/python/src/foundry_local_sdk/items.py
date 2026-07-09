from __future__ import annotations

# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------

import math
import struct
from dataclasses import dataclass
from enum import IntEnum


_API_VERSION = 1  # FOUNDRY_LOCAL_API_VERSION
# Sentinel for absent time fields in flSpeech* structs. INT64_MIN, matches
# C ABI FOUNDRY_LOCAL_DURATION_UNSET. cffi cdef does not process #define.
_DURATION_UNSET: int = -(2**63)
# Sentinel for an absent confidence value in flSpeechWord. -FLT_MAX (the most-negative finite
# 32-bit float), matching C ABI FOUNDRY_LOCAL_CONFIDENCE_UNSET. Derived from the IEEE-754
# single-precision bit pattern (0x7f7fffff) so it equals the exact double cffi widens the
# C float to. cffi cdef does not process #define.
_CONFIDENCE_UNSET: float = -struct.unpack("<f", b"\xff\xff\x7f\x7f")[0]


class ItemType(IntEnum):
    UNKNOWN = 0
    BYTES = 1
    TENSOR = 10
    TEXT = 20
    MESSAGE = 21
    IMAGE = 25
    AUDIO = 30
    SPEECH_SEGMENT = 31
    SPEECH_RESULT = 32
    TOOL_CALL = 100
    TOOL_RESULT = 101
    QUEUE = 200


class TextItemType(IntEnum):
    DEFAULT = 0
    REASONING = 1
    OPENAI_JSON = 2


class SpeechSegmentKind(IntEnum):
    """Discriminator for a :class:`SpeechSegmentItem`.

    PARTIAL/FINAL describe the state of the current segment hypothesis in a
    streaming callback. PARTIAL is the evolving guess for the in-progress
    segment; FINAL closes it. They do not describe the overall response.
    NONE is used for segments embedded in :class:`SpeechResultItem`, where
    the streaming distinction no longer applies.
    """
    NONE = 0
    PARTIAL = 1
    FINAL = 2


class MessageRole(IntEnum):
    NONE = 0
    SYSTEM = 1
    USER = 2
    ASSISTANT = 3
    TOOL = 4
    DEVELOPER = 5


class TensorDataType(IntEnum):
    UNDEFINED = 0
    FLOAT = 1
    UINT8 = 2
    INT8 = 3
    UINT16 = 4
    INT16 = 5
    INT32 = 6
    INT64 = 7
    STRING = 8
    BOOL = 9
    FLOAT16 = 10
    DOUBLE = 11
    UINT32 = 12
    UINT64 = 13
    COMPLEX64 = 14
    COMPLEX128 = 15
    BFLOAT16 = 16
    FLOAT8E4M3FN = 17
    FLOAT8E4M3FNUZ = 18
    FLOAT8E5M2 = 19
    FLOAT8E5M2FNUZ = 20
    UINT4 = 21
    INT4 = 22
    FLOAT4E2M1 = 23
    FLOAT8E8M0 = 24


# Byte size per tensor element, indexed by TensorDataType value.
# A size of 0 means "variable / sub-byte" — STRING (variable-length) and the 4-bit packed dtypes
# (UINT4, INT4, FLOAT4E2M1) cannot be sized by a simple element-count multiplication and must be handled
# specially by callers.
_TENSOR_ELEMENT_BYTES: dict[int, int] = {
    TensorDataType.FLOAT: 4,
    TensorDataType.UINT8: 1,
    TensorDataType.INT8: 1,
    TensorDataType.UINT16: 2,
    TensorDataType.INT16: 2,
    TensorDataType.INT32: 4,
    TensorDataType.INT64: 8,
    TensorDataType.STRING: 0,
    TensorDataType.BOOL: 1,
    TensorDataType.FLOAT16: 2,
    TensorDataType.DOUBLE: 8,
    TensorDataType.UINT32: 4,
    TensorDataType.UINT64: 8,
    TensorDataType.COMPLEX64: 8,
    TensorDataType.COMPLEX128: 16,
    TensorDataType.BFLOAT16: 2,
    TensorDataType.FLOAT8E4M3FN: 1,
    TensorDataType.FLOAT8E4M3FNUZ: 1,
    TensorDataType.FLOAT8E5M2: 1,
    TensorDataType.FLOAT8E5M2FNUZ: 1,
    TensorDataType.UINT4: 0,
    TensorDataType.INT4: 0,
    TensorDataType.FLOAT4E2M1: 0,
    TensorDataType.FLOAT8E8M0: 1,
}


def _utf8(c_str) -> str | None:
    from foundry_local_sdk._native import ffi

    if c_str == ffi.NULL:
        return None

    return ffi.string(c_str).decode("utf-8")


class Item:
    # Native handle and ownership flag. ``_ptr`` is a cffi cdata of type ``flItem*`` and is set to ``None``
    # after ``_close()`` releases it. cffi cdata has no Python type, so the annotation is intentionally loose.
    _ptr: object | None
    _owns: bool

    def __init__(self, ptr, owns: bool = True) -> None:
        self._ptr = ptr
        self._owns = owns

    @classmethod
    def from_native(cls, ptr, owns: bool = False) -> "Item":
        """Dispatch to the correct subclass based on GetType."""
        from foundry_local_sdk._native.api import api

        item_type = ItemType(int(api.item.GetType(ptr)))
        subclass = _TYPE_MAP.get(item_type)

        if subclass is None:
            # Unknown or unrepresented type — wrap as a bare Item without data access.
            if item_type == ItemType.QUEUE:
                # Tripwire: queues from native flow back through Item.from_native
                # is not currently a supported path. ItemQueue is created by the
                # Python side and handed in; we don't expect to receive one back.
                import logging
                logging.getLogger(__name__).warning(
                    "ItemQueue received from native — this path is not yet "
                    "supported, returning bare Item"
                )
            obj = cls.__new__(cls)
            obj._ptr = ptr
            obj._owns = owns
            return obj

        return subclass._from_native(ptr, owns)

    @property
    def item_type(self) -> ItemType:
        from foundry_local_sdk._native.api import api

        return ItemType(int(api.item.GetType(self._ptr)))

    def _release_ownership(self):
        """Transfer native ownership to caller (e.g. before adding to a Request)."""
        self._owns = False
        return self._ptr

    def _close(self) -> None:
        """Release native handle. Safe to call multiple times."""
        # Use getattr to guard against incomplete initialisation (e.g. if a
        # subclass __init__ raises before super().__init__() is reached).
        if getattr(self, "_owns", False) and getattr(self, "_ptr", None) is not None:
            try:
                from foundry_local_sdk._native.api import api

                api.item.Item_Release(self._ptr)
            except Exception:
                pass

            self._ptr = None
            self._owns = False

    def __enter__(self) -> "Item":
        return self

    def __exit__(self, *_) -> None:
        self._close()

    def __del__(self) -> None:
        self._close()


class TextItem(Item):
    text: str
    type: TextItemType

    def __init__(self, text: str, type: TextItemType = TextItemType.DEFAULT) -> None:
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.TEXT, out))
        super().__init__(out[0], owns=True)

        # Keep c_text alive until after SetText returns — cffi does not copy the string.
        c_text = ffi.new("char[]", text.encode("utf-8") + b"\x00")
        text_data = ffi.new("flTextData*")
        text_data.version = _API_VERSION
        text_data.text = c_text
        text_data.type = int(type)
        api.check_status(api.item.SetText(self._ptr, text_data))

        self.text = text
        self.type = type

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "TextItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flTextData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetText(ptr, data))

        obj.text = _utf8(data.text) or ""
        obj.type = TextItemType(int(data.type))

        return obj

    def __repr__(self) -> str:
        return f"TextItem({self.text!r}, type={self.type.name})"


class MessageItem(Item):
    role: MessageRole
    name: str | None
    # Borrowed (not owned) child items. The MessageItem holds native pointers into these parts but does not
    # release them; ``_parts`` keeps the Python wrappers alive so their underlying handles stay valid for the
    # lifetime of this message. See the constructor's ``.. warning::`` block.
    _parts: list[Item]

    def __init__(
        self,
        role: MessageRole,
        content: str | list[Item],
        name: str | None = None,
    ) -> None:
        """Construct a MESSAGE item from a role and content.

        ``content`` may be a string (wrapped in an internally-owned ``TextItem``) or a list of pre-built ``Item``
        objects. When a list is supplied, the constructed ``MessageItem`` **borrows** the native pointers of
        those parts — it does not take ownership. The caller-supplied parts are kept alive by ``self._parts``
        for the lifetime of this message.

        .. warning::
            Do not call ``_close()`` (or otherwise release) any item in ``content`` while this ``MessageItem``
            is still in use. Doing so leaves the message holding dangling native pointers, which the native
            layer will dereference and corrupt or crash. Treat parts as owned by the ``MessageItem`` once
            handed in.
        """
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.MESSAGE, out))
        super().__init__(out[0], owns=True)

        self.role = role
        self.name = name

        if isinstance(content, str):
            text_part = TextItem(content)
            self._parts = [text_part]
        else:
            if not content:
                raise ValueError("content list must not be empty")

            self._parts = list(content)

        # All cffi temporaries must remain alive until after SetMessage returns.
        c_ptrs = ffi.new("flItem*[]", [p._ptr for p in self._parts])
        c_name = ffi.new("char[]", name.encode("utf-8") + b"\x00") if name is not None else ffi.NULL

        msg_data = ffi.new("flMessageData*")
        msg_data.version = _API_VERSION
        msg_data.role = int(role)
        msg_data.content_items = c_ptrs
        msg_data.content_items_count = len(self._parts)
        msg_data.name = c_name

        api.check_status(api.item.SetMessage(self._ptr, msg_data))

    @classmethod
    def system(cls, content: str | list[Item], name: str | None = None) -> "MessageItem":
        return cls(MessageRole.SYSTEM, content, name)

    @classmethod
    def user(cls, content: str | list[Item], name: str | None = None) -> "MessageItem":
        return cls(MessageRole.USER, content, name)

    @classmethod
    def assistant(cls, content: str | list[Item], name: str | None = None) -> "MessageItem":
        return cls(MessageRole.ASSISTANT, content, name)

    @classmethod
    def developer(cls, content: str | list[Item], name: str | None = None) -> "MessageItem":
        return cls(MessageRole.DEVELOPER, content, name)

    @property
    def parts(self) -> list[Item]:
        return list(self._parts)

    def is_simple_text(self) -> bool:
        """True when the message has exactly one part and it is a ``TextItem``."""
        return len(self._parts) == 1 and isinstance(self._parts[0], TextItem)

    def get_simple_text(self) -> str:
        """Text of the single ``TextItem`` part. Raises if :meth:`is_simple_text` is false."""
        if not self.is_simple_text():
            raise ValueError("MessageItem is not a single TextItem")

        return self._parts[0].text  # type: ignore[attr-defined]

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "MessageItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flMessageData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetMessage(ptr, data))

        obj.role = MessageRole(int(data.role))
        obj.name = _utf8(data.name)
        obj._parts = [
            Item.from_native(data.content_items[i], owns=False)
            for i in range(int(data.content_items_count))
        ]

        return obj

    def __repr__(self) -> str:
        return f"MessageItem(role={self.role.name}, parts={len(self._parts)})"


class BytesItem(Item):
    data: bytes

    def __init__(self, data: bytes | bytearray | memoryview) -> None:
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.BYTES, out))
        super().__init__(out[0], owns=True)

        raw = bytes(data)
        self.data = raw

        # Keep both raw and buf alive until after SetBytes returns — the native
        # call reads the buffer pointer synchronously so no heap copy is needed.
        buf = ffi.from_buffer(raw)
        bytes_data = ffi.new("flBytesData*")
        bytes_data.version = _API_VERSION
        bytes_data.item_type = int(ItemType.BYTES)
        bytes_data.data = ffi.cast("void *", buf)
        bytes_data.data_size = len(raw)
        api.check_status(api.item.SetBytes(self._ptr, bytes_data))

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "BytesItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flBytesData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetBytes(ptr, data))

        if int(data.data_size) > 0:
            obj.data = bytes(ffi.buffer(ffi.cast("char*", data.data), int(data.data_size)))
        else:
            obj.data = b""

        return obj

    def __repr__(self) -> str:
        return f"BytesItem({len(self.data)} bytes)"


class ImageItem(Item):
    # ``format`` is the IANA-style codec hint (e.g. ``"png"``); ``None`` when the native side did not supply
    # one. ``uri`` is set when the image was constructed from a URI rather than inline bytes; mutually
    # exclusive with non-empty ``data`` in practice.
    format: str | None
    uri: str | None
    data: bytes

    def __init__(self, format: str, data: bytes | bytearray) -> None:
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.IMAGE, out))
        super().__init__(out[0], owns=True)

        self.format = format
        self.uri = None
        raw = bytes(data)
        self.data = raw

        # Keep all cffi temporaries alive until after SetImage returns.
        buf = ffi.from_buffer(raw)
        c_fmt = ffi.new("char[]", format.encode("utf-8") + b"\x00")
        image_data = ffi.new("flImageData*")
        image_data.version = _API_VERSION
        image_data.data = ffi.cast("void *", buf)
        image_data.data_size = len(raw)
        image_data.format = c_fmt
        image_data.uri = ffi.NULL
        api.check_status(api.item.SetImage(self._ptr, image_data))

    @classmethod
    def from_uri(cls, uri: str, format: str | None = None) -> "ImageItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.IMAGE, out))

        obj = cls.__new__(cls)
        obj._ptr = out[0]
        obj._owns = True
        obj.uri = uri
        obj.format = format
        obj.data = b""

        # Keep cffi temporaries alive until after SetImage returns.
        c_uri = ffi.new("char[]", uri.encode("utf-8") + b"\x00")
        c_fmt = ffi.new("char[]", format.encode("utf-8") + b"\x00") if format is not None else ffi.NULL

        image_data = ffi.new("flImageData*")
        image_data.version = _API_VERSION
        image_data.data = ffi.NULL
        image_data.data_size = 0
        image_data.uri = c_uri
        image_data.format = c_fmt
        api.check_status(api.item.SetImage(obj._ptr, image_data))

        return obj

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "ImageItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flImageData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetImage(ptr, data))

        obj.format = _utf8(data.format)
        obj.uri = _utf8(data.uri)

        if int(data.data_size) > 0:
            obj.data = bytes(ffi.buffer(ffi.cast("char*", data.data), int(data.data_size)))
        else:
            obj.data = b""

        return obj

    def __repr__(self) -> str:
        if self.uri:
            return f"ImageItem(uri={self.uri!r}, format={self.format!r})"

        return f"ImageItem({len(self.data)} bytes, format={self.format!r})"


class AudioItem(Item):
    format: str | None
    uri: str | None
    data: bytes
    sample_rate: int
    channels: int

    def __init__(
        self,
        format: str,
        data: bytes | bytearray,
        sample_rate: int = 0,
        channels: int = 0,
    ) -> None:
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.AUDIO, out))
        super().__init__(out[0], owns=True)

        self.format = format
        self.uri = None
        raw = bytes(data)
        self.data = raw
        self.sample_rate = sample_rate
        self.channels = channels

        # Keep all cffi temporaries alive until after SetAudio returns.
        buf = ffi.from_buffer(raw)
        c_fmt = ffi.new("char[]", format.encode("utf-8") + b"\x00")
        audio_data = ffi.new("flAudioData*")
        audio_data.version = _API_VERSION
        audio_data.data = ffi.cast("void *", buf)
        audio_data.data_size = len(raw)
        audio_data.format = c_fmt
        audio_data.uri = ffi.NULL
        audio_data.sample_rate = sample_rate
        audio_data.channels = channels
        api.check_status(api.item.SetAudio(self._ptr, audio_data))

    @classmethod
    def create_format_descriptor(cls, format: str, sample_rate: int, channels: int) -> "AudioItem":
        """Create an ``AudioItem`` describing audio format only (no data).

        Used as the format hint for live streaming sessions where audio
        data arrives later via an ``ItemQueue``.
        """
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.AUDIO, out))

        obj = cls.__new__(cls)
        obj._ptr = out[0]
        obj._owns = True
        obj.uri = None
        obj.format = format
        obj.data = b""
        obj.sample_rate = sample_rate
        obj.channels = channels

        # Keep cffi temporaries alive until after SetAudio returns.
        c_fmt = ffi.new("char[]", format.encode("utf-8") + b"\x00")

        audio_data = ffi.new("flAudioData*")
        audio_data.version = _API_VERSION
        audio_data.data = ffi.NULL
        audio_data.data_size = 0
        audio_data.uri = ffi.NULL
        audio_data.format = c_fmt
        audio_data.sample_rate = sample_rate
        audio_data.channels = channels
        api.check_status(api.item.SetAudio(obj._ptr, audio_data))

        return obj

    @classmethod
    def from_uri(cls, uri: str, format: str | None = None) -> "AudioItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.AUDIO, out))

        obj = cls.__new__(cls)
        obj._ptr = out[0]
        obj._owns = True
        obj.uri = uri
        obj.format = format
        obj.data = b""
        obj.sample_rate = 0
        obj.channels = 0

        # Keep cffi temporaries alive until after SetAudio returns.
        c_uri = ffi.new("char[]", uri.encode("utf-8") + b"\x00")
        c_fmt = ffi.new("char[]", format.encode("utf-8") + b"\x00") if format is not None else ffi.NULL

        audio_data = ffi.new("flAudioData*")
        audio_data.version = _API_VERSION
        audio_data.data = ffi.NULL
        audio_data.data_size = 0
        audio_data.uri = c_uri
        audio_data.format = c_fmt
        audio_data.sample_rate = 0
        audio_data.channels = 0
        api.check_status(api.item.SetAudio(obj._ptr, audio_data))

        return obj

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "AudioItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flAudioData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetAudio(ptr, data))

        obj.format = _utf8(data.format)
        obj.uri = _utf8(data.uri)
        obj.sample_rate = int(data.sample_rate)
        obj.channels = int(data.channels)

        if int(data.data_size) > 0:
            obj.data = bytes(ffi.buffer(ffi.cast("char*", data.data), int(data.data_size)))
        else:
            obj.data = b""

        return obj

    def __repr__(self) -> str:
        if self.uri:
            return f"AudioItem(uri={self.uri!r}, format={self.format!r})"

        return f"AudioItem({len(self.data)} bytes, format={self.format!r})"


class ToolCallItem(Item):
    call_id: str
    name: str
    arguments: str

    def __init__(self, call_id: str, name: str, arguments: str) -> None:
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.TOOL_CALL, out))
        super().__init__(out[0], owns=True)

        self.call_id = call_id
        self.name = name
        self.arguments = arguments

        # Keep cffi temporaries alive until after SetToolCall returns.
        c_call_id = ffi.new("char[]", call_id.encode("utf-8") + b"\x00")
        c_name = ffi.new("char[]", name.encode("utf-8") + b"\x00")
        c_args = ffi.new("char[]", arguments.encode("utf-8") + b"\x00")

        tool_call_data = ffi.new("flToolCallData*")
        tool_call_data.version = _API_VERSION
        tool_call_data.call_id = c_call_id
        tool_call_data.name = c_name
        tool_call_data.arguments = c_args
        api.check_status(api.item.SetToolCall(self._ptr, tool_call_data))

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "ToolCallItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flToolCallData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetToolCall(ptr, data))

        obj.call_id = _utf8(data.call_id) or ""
        obj.name = _utf8(data.name) or ""
        obj.arguments = _utf8(data.arguments) or ""

        return obj

    def __repr__(self) -> str:
        return f"ToolCallItem(id={self.call_id!r}, name={self.name!r})"


class ToolResultItem(Item):
    call_id: str
    result: str

    def __init__(self, call_id: str, result: str) -> None:
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flItem**")
        api.check_status(api.item.Create(ItemType.TOOL_RESULT, out))
        super().__init__(out[0], owns=True)

        self.call_id = call_id
        self.result = result

        # Keep cffi temporaries alive until after SetToolResult returns.
        c_call_id = ffi.new("char[]", call_id.encode("utf-8") + b"\x00")
        c_result = ffi.new("char[]", result.encode("utf-8") + b"\x00")

        tool_result_data = ffi.new("flToolResultData*")
        tool_result_data.version = _API_VERSION
        tool_result_data.call_id = c_call_id
        tool_result_data.result = c_result
        api.check_status(api.item.SetToolResult(self._ptr, tool_result_data))

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "ToolResultItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flToolResultData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetToolResult(ptr, data))

        obj.call_id = _utf8(data.call_id) or ""
        obj.result = _utf8(data.result) or ""

        return obj

    def __repr__(self) -> str:
        return f"ToolResultItem(id={self.call_id!r})"


class TensorItem(Item):
    data_type: TensorDataType
    shape: list[int]
    data: bytes

    def __init__(self) -> None:
        raise TypeError("TensorItem cannot be created directly; use Item.from_native()")

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "TensorItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flTensorData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetTensor(ptr, data))

        obj.data_type = TensorDataType(int(data.data_type))
        rank = int(data.rank)
        shape_ptr = ffi.cast("int64_t *", data.shape)
        obj.shape = [int(shape_ptr[i]) for i in range(rank)]

        elem_bytes = _TENSOR_ELEMENT_BYTES.get(int(obj.data_type), 0)
        # When elem_bytes == 0 (e.g. STRING) the byte count is indeterminate; yield empty bytes.
        total = math.prod(obj.shape) * elem_bytes if (obj.shape and elem_bytes) else 0

        if total > 0:
            obj.data = bytes(ffi.buffer(ffi.cast("char*", data.data), total))
        else:
            obj.data = b""

        return obj

    def __repr__(self) -> str:
        return f"TensorItem(dtype={self.data_type.name}, shape={self.shape})"


@dataclass(frozen=True)
class SpeechWord:
    """One word within a :class:`SpeechSegmentItem`. Output-only."""
    text: str
    start_time_ms: int | None
    end_time_ms: int | None
    confidence: float | None
    speaker_id: str | None


class SpeechSegmentItem(Item):
    """Output-only item produced by ``AudioSession``.

    A streaming callback receives zero-or-more PARTIAL segments followed by
    exactly one FINAL closing the current utterance. Entries of
    :attr:`SpeechResultItem.segments` use NONE or FINAL.
    """

    kind: SpeechSegmentKind
    text: str
    start_time_ms: int | None
    end_time_ms: int | None
    utterance_start: bool
    words: list[SpeechWord]
    language: str | None

    def __init__(self) -> None:
        raise TypeError("SpeechSegmentItem cannot be created directly; use Item.from_native()")

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "SpeechSegmentItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flSpeechSegmentData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetSpeechSegment(ptr, data))

        obj.kind = SpeechSegmentKind(int(data.kind))
        obj.text = _utf8(data.text) or ""
        start = int(data.start_time_ms)
        end = int(data.end_time_ms)
        obj.start_time_ms = None if start == _DURATION_UNSET else start
        obj.end_time_ms = None if end == _DURATION_UNSET else end
        obj.utterance_start = bool(data.utterance_start)
        obj.language = _utf8(data.language)

        word_count = int(data.words_count)
        words: list[SpeechWord] = []
        for i in range(word_count):
            w = data.words[i]
            w_start = int(w.start_time_ms)
            w_end = int(w.end_time_ms)
            # Read confidence only when set; the sentinel marks an absent value.
            conf = float(w.confidence)
            confidence = None if conf == _CONFIDENCE_UNSET else conf
            words.append(SpeechWord(
                text=_utf8(w.text) or "",
                start_time_ms=None if w_start == _DURATION_UNSET else w_start,
                end_time_ms=None if w_end == _DURATION_UNSET else w_end,
                confidence=confidence,
                speaker_id=_utf8(w.speaker_id),
            ))
        obj.words = words

        return obj

    def __repr__(self) -> str:
        preview = self.text if len(self.text) <= 40 else self.text[:37] + "..."
        return f"SpeechSegmentItem(kind={self.kind.name}, text={preview!r})"


class SpeechResultItem(Item):
    """Output-only aggregate produced by ``AudioSession`` at the end of a transcription.

    Holds the full transcript and per-segment detail. The :attr:`segments`
    entries are borrowed handles owned by this result; do not close them
    individually — they remain valid only while this item is alive.
    """

    text: str
    language: str | None
    duration_ms: int | None
    segments: list[SpeechSegmentItem]

    def __init__(self) -> None:
        raise TypeError("SpeechResultItem cannot be created directly; use Item.from_native()")

    @classmethod
    def _from_native(cls, ptr, owns: bool) -> "SpeechResultItem":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        obj = cls.__new__(cls)
        obj._ptr = ptr
        obj._owns = owns

        data = ffi.new("flSpeechResultData*")
        data.version = _API_VERSION
        api.check_status(api.item.GetSpeechResult(ptr, data))

        obj.text = _utf8(data.text) or ""
        obj.language = _utf8(data.language)
        duration = int(data.duration_ms)
        obj.duration_ms = None if duration == _DURATION_UNSET else duration

        seg_count = int(data.segments_count)
        # Borrowed segment handles are owned by the result item; their lifetime
        # is tied to obj via the native parent, so owns=False on the wrappers.
        obj.segments = [
            SpeechSegmentItem._from_native(data.segments[i], owns=False)
            for i in range(seg_count)
        ]

        return obj

    def __repr__(self) -> str:
        dur = self.duration_ms if self.duration_ms is not None else "?"
        return f"SpeechResultItem(duration_ms={dur}, segments={len(self.segments)})"


# Dispatch map for Item.from_native — must be defined after all subclasses
# so the names resolve correctly.
_TYPE_MAP: dict[ItemType, type[Item]] = {
    ItemType.TEXT: TextItem,
    ItemType.MESSAGE: MessageItem,
    ItemType.BYTES: BytesItem,
    ItemType.TENSOR: TensorItem,
    ItemType.IMAGE: ImageItem,
    ItemType.AUDIO: AudioItem,
    ItemType.SPEECH_SEGMENT: SpeechSegmentItem,
    ItemType.SPEECH_RESULT: SpeechResultItem,
    ItemType.TOOL_CALL: ToolCallItem,
    ItemType.TOOL_RESULT: ToolResultItem,
}
