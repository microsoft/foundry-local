# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for the public Item construction APIs in foundry_local_sdk.items.

These tests do not load any model and do not touch FoundryLocalManager. They
exercise pure native struct round-trips through the cffi extension.
"""
from __future__ import annotations

import gc

import pytest

from foundry_local_sdk.items import (
    AudioItem,
    BytesItem,
    ImageItem,
    Item,
    ItemType,
    MessageItem,
    MessageRole,
    TensorItem,
    TextItem,
    TextItemType,
    ToolCallItem,
    ToolResultItem,
)


class TestTextItem:
    def test_default_type_construct_and_attributes(self):
        item = TextItem("hello world")
        assert item.text == "hello world"
        assert item.type == TextItemType.DEFAULT
        assert item.item_type == ItemType.TEXT

    def test_reasoning_type_construct_and_attributes(self):
        item = TextItem("because", type=TextItemType.REASONING)
        assert item.text == "because"
        assert item.type == TextItemType.REASONING

    def test_round_trip_default(self):
        item = TextItem("round trip")
        readback = TextItem._from_native(item._ptr, owns=False)
        assert readback.text == "round trip"
        assert readback.type == TextItemType.DEFAULT

    def test_round_trip_reasoning(self):
        item = TextItem("why", type=TextItemType.REASONING)
        readback = TextItem._from_native(item._ptr, owns=False)
        assert readback.text == "why"
        assert readback.type == TextItemType.REASONING

    def test_repr_contains_class_name(self):
        s = repr(TextItem("abc"))
        assert "TextItem" in s
        assert s != ""


class TestMessageItem:
    def test_string_content_wraps_in_text_item(self):
        msg = MessageItem(MessageRole.USER, "hi")
        assert msg.role == MessageRole.USER
        assert msg.name is None
        assert msg.item_type == ItemType.MESSAGE
        parts = msg.parts
        assert len(parts) == 1
        assert isinstance(parts[0], TextItem)
        assert parts[0].text == "hi"

    def test_list_content_preserves_identity(self):
        a = TextItem("first")
        b = TextItem("second")
        msg = MessageItem(MessageRole.ASSISTANT, [a, b])
        parts = msg.parts
        assert len(parts) == 2
        assert parts[0] is a
        assert parts[1] is b

    def test_classmethod_constructors_set_role(self):
        assert MessageItem.system("s").role == MessageRole.SYSTEM
        assert MessageItem.user("u").role == MessageRole.USER
        assert MessageItem.assistant("a").role == MessageRole.ASSISTANT
        assert MessageItem.developer("d").role == MessageRole.DEVELOPER

    def test_name_field_stored_and_round_trips(self):
        msg = MessageItem(MessageRole.USER, "hi", name="alice")
        assert msg.name == "alice"
        readback = MessageItem._from_native(msg._ptr, owns=False)
        assert readback.role == MessageRole.USER
        assert readback.name == "alice"
        assert len(readback.parts) == 1

    def test_round_trip_multi_part(self):
        a = TextItem("hello")
        b = TextItem("world")
        msg = MessageItem(MessageRole.ASSISTANT, [a, b])
        readback = MessageItem._from_native(msg._ptr, owns=False)
        assert readback.role == MessageRole.ASSISTANT
        assert len(readback.parts) == 2
        # Read-back parts are fresh Python wrappers over the borrowed natives,
        # so identity won't match — but the data should.
        assert isinstance(readback.parts[0], TextItem)
        assert readback.parts[0].text == "hello"
        assert readback.parts[1].text == "world"

    def test_empty_list_content_raises(self):
        with pytest.raises(ValueError):
            MessageItem(MessageRole.USER, [])

    def test_parts_survive_local_reference_gc(self):
        # Documented contract: MessageItem keeps content items alive via
        # self._parts, so callers don't have to retain local references.
        msg = MessageItem(MessageRole.USER, [TextItem("a"), TextItem("b")])
        gc.collect()
        readback = MessageItem._from_native(msg._ptr, owns=False)
        assert [p.text for p in readback.parts] == ["a", "b"]

    def test_repr_contains_class_name(self):
        s = repr(MessageItem(MessageRole.USER, "hi"))
        assert "MessageItem" in s

    def test_is_simple_text_true_for_single_text_part(self):
        msg = MessageItem(MessageRole.USER, "hi")
        assert msg.is_simple_text() is True
        assert msg.get_simple_text() == "hi"

    def test_is_simple_text_false_for_multi_part(self):
        msg = MessageItem(MessageRole.USER, [TextItem("a"), TextItem("b")])
        assert msg.is_simple_text() is False
        with pytest.raises(ValueError):
            msg.get_simple_text()


class TestBytesItem:
    def test_bytes_input(self):
        item = BytesItem(b"\x00\x01\x02\x03")
        assert item.data == b"\x00\x01\x02\x03"
        assert item.item_type == ItemType.BYTES

    def test_bytearray_input(self):
        item = BytesItem(bytearray(b"abc"))
        assert item.data == b"abc"

    def test_memoryview_input(self):
        item = BytesItem(memoryview(b"xyz"))
        assert item.data == b"xyz"

    def test_empty_bytes(self):
        item = BytesItem(b"")
        assert item.data == b""
        readback = BytesItem._from_native(item._ptr, owns=False)
        assert readback.data == b""

    def test_round_trip(self):
        payload = bytes(range(256))
        item = BytesItem(payload)
        readback = BytesItem._from_native(item._ptr, owns=False)
        assert readback.data == payload

    def test_repr_contains_class_name(self):
        assert "BytesItem" in repr(BytesItem(b"hi"))


class TestImageItem:
    def test_inline_data_construct(self):
        item = ImageItem("png", b"\x89PNG\r\n")
        assert item.format == "png"
        assert item.data == b"\x89PNG\r\n"
        assert item.uri is None
        assert item.item_type == ItemType.IMAGE

    def test_inline_data_round_trip(self):
        item = ImageItem("jpeg", b"\xff\xd8\xff")
        readback = ImageItem._from_native(item._ptr, owns=False)
        assert readback.format == "jpeg"
        assert readback.data == b"\xff\xd8\xff"
        assert readback.uri is None

    def test_from_uri_with_format(self):
        item = ImageItem.from_uri("https://example.com/x.png", format="png")
        assert item.uri == "https://example.com/x.png"
        assert item.format == "png"
        assert item.data == b""
        readback = ImageItem._from_native(item._ptr, owns=False)
        assert readback.uri == "https://example.com/x.png"
        assert readback.format == "png"
        assert readback.data == b""

    def test_from_uri_without_format(self):
        item = ImageItem.from_uri("file:///tmp/x")
        assert item.uri == "file:///tmp/x"
        assert item.format is None
        readback = ImageItem._from_native(item._ptr, owns=False)
        assert readback.uri == "file:///tmp/x"
        assert readback.format is None

    def test_repr_contains_class_name(self):
        assert "ImageItem" in repr(ImageItem("png", b"x"))
        assert "ImageItem" in repr(ImageItem.from_uri("u", format="png"))


class TestAudioItem:
    def test_inline_data_construct(self):
        item = AudioItem("pcm16", b"\x00\x01\x02\x03", sample_rate=16000, channels=1)
        assert item.format == "pcm16"
        assert item.data == b"\x00\x01\x02\x03"
        assert item.sample_rate == 16000
        assert item.channels == 1
        assert item.uri is None
        assert item.item_type == ItemType.AUDIO

    def test_inline_data_round_trip(self):
        item = AudioItem("pcm16", b"\xaa\xbb", sample_rate=44100, channels=2)
        readback = AudioItem._from_native(item._ptr, owns=False)
        assert readback.format == "pcm16"
        assert readback.data == b"\xaa\xbb"
        assert readback.sample_rate == 44100
        assert readback.channels == 2
        assert readback.uri is None

    def test_create_format_descriptor(self):
        item = AudioItem.create_format_descriptor("pcm16", 16000, 1)
        assert item.data == b""
        assert item.sample_rate == 16000
        assert item.channels == 1
        assert item.format == "pcm16"
        readback = AudioItem._from_native(item._ptr, owns=False)
        assert readback.data == b""
        assert readback.sample_rate == 16000
        assert readback.channels == 1
        assert readback.format == "pcm16"

    def test_from_uri_with_format(self):
        item = AudioItem.from_uri("https://example.com/clip.wav", format="wav")
        assert item.uri == "https://example.com/clip.wav"
        assert item.format == "wav"
        assert item.data == b""
        readback = AudioItem._from_native(item._ptr, owns=False)
        assert readback.uri == "https://example.com/clip.wav"
        assert readback.format == "wav"

    def test_repr_contains_class_name(self):
        assert "AudioItem" in repr(AudioItem("pcm16", b"x", 16000, 1))
        assert "AudioItem" in repr(AudioItem.from_uri("u", format="wav"))


class TestToolCallItem:
    def test_construct_and_attributes(self):
        item = ToolCallItem("call-1", "get_weather", '{"city":"Seattle"}')
        assert item.call_id == "call-1"
        assert item.name == "get_weather"
        assert item.arguments == '{"city":"Seattle"}'
        assert item.item_type == ItemType.TOOL_CALL

    def test_round_trip(self):
        item = ToolCallItem("id42", "do_thing", "{}")
        readback = ToolCallItem._from_native(item._ptr, owns=False)
        assert readback.call_id == "id42"
        assert readback.name == "do_thing"
        assert readback.arguments == "{}"

    def test_repr_contains_class_name(self):
        assert "ToolCallItem" in repr(ToolCallItem("a", "b", "c"))


class TestToolResultItem:
    def test_construct_and_attributes(self):
        item = ToolResultItem("call-1", "sunny")
        assert item.call_id == "call-1"
        assert item.result == "sunny"
        assert item.item_type == ItemType.TOOL_RESULT

    def test_round_trip(self):
        item = ToolResultItem("id42", '{"ok":true}')
        readback = ToolResultItem._from_native(item._ptr, owns=False)
        assert readback.call_id == "id42"
        assert readback.result == '{"ok":true}'

    def test_repr_contains_class_name(self):
        assert "ToolResultItem" in repr(ToolResultItem("a", "b"))


class TestTensorItemNegative:
    def test_direct_construction_raises(self):
        with pytest.raises(TypeError):
            TensorItem()


class TestDispatchByType:
    def test_from_native_dispatches_to_text(self):
        original = TextItem("dispatch me")
        readback = Item.from_native(original._ptr, owns=False)
        assert isinstance(readback, TextItem)
        assert readback.text == "dispatch me"

    def test_from_native_dispatches_to_bytes(self):
        original = BytesItem(b"abc")
        readback = Item.from_native(original._ptr, owns=False)
        assert isinstance(readback, BytesItem)
        assert readback.data == b"abc"
