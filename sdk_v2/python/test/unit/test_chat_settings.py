# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for ChatClientSettings serialization and validation.

These exercise the Python layer in isolation — no native session is created,
so the dev openai dependency is the only requirement.
"""
from __future__ import annotations
from foundry_local_sdk.openai.chat_client import ChatClientSettings

import pytest

pytest.importorskip("openai")


class TestSerialization:
    def test_empty_settings_serialize_to_empty_dict(self):
        assert ChatClientSettings()._serialize() == {}

    def test_top_level_fields_round_trip(self):
        s = ChatClientSettings(
            frequency_penalty=0.0,
            max_tokens=128,
            n=2,
            temperature=0.7,
            presence_penalty=0.0,
            top_p=0.9,
        )
        d = s._serialize()
        assert d["frequency_penalty"] == 0.0
        assert d["max_tokens"] == 128
        assert d["n"] == 2
        assert d["temperature"] == 0.7
        assert d["presence_penalty"] == 0.0
        assert d["top_p"] == 0.9
        assert "metadata" not in d

    def test_foundry_specific_fields_serialise_into_metadata(self):
        s = ChatClientSettings(top_k=40, random_seed=123)
        d = s._serialize()
        assert d["metadata"] == {"top_k": "40", "random_seed": "123"}

    def test_none_fields_omitted_from_payload(self):
        s = ChatClientSettings(temperature=0.5)
        d = s._serialize()
        assert d == {"temperature": 0.5}


class TestResponseFormatValidation:
    def test_text_format_accepted(self):
        fmt = {"type": "text"}
        d = ChatClientSettings(response_format=fmt)._serialize()
        assert d == {"response_format": {"type": "text"}}

    def test_json_object_format_accepted(self):
        fmt = {"type": "json_object"}
        d = ChatClientSettings(response_format=fmt)._serialize()
        assert d == {"response_format": {"type": "json_object"}}

    def test_invalid_type_rejected(self):
        with pytest.raises(ValueError, match="ResponseFormat type"):
            ChatClientSettings(response_format={"type": "yaml"})._serialize()

    def test_json_schema_requires_schema_string(self):
        with pytest.raises(ValueError, match="json_schema"):
            ChatClientSettings(response_format={"type": "json_schema"})._serialize()

    def test_json_schema_with_schema_accepted(self):
        schema = '{"type":"object"}'
        d = ChatClientSettings(
            response_format={"type": "json_schema", "json_schema": schema}
        )._serialize()
        assert d == {"response_format": {"type": "json_schema", "json_schema": schema}}

    def test_text_with_schema_rejected(self):
        with pytest.raises(ValueError):
            ChatClientSettings(
                response_format={"type": "text", "json_schema": "..."}
            )._serialize()


class TestToolChoiceValidation:
    @pytest.mark.parametrize("kind", ["none", "auto", "required"])
    def test_basic_kinds_accepted(self, kind):
        d = ChatClientSettings(tool_choice={"type": kind})._serialize()
        assert d == {"tool_choice": {"type": kind}}

    def test_function_requires_name(self):
        with pytest.raises(ValueError, match="name"):
            ChatClientSettings(tool_choice={"type": "function"})._serialize()

    def test_function_with_name_accepted(self):
        d = ChatClientSettings(tool_choice={"type": "function", "name": "f"})._serialize()
        assert d == {"tool_choice": {"type": "function", "name": "f"}}

    def test_non_function_with_name_rejected(self):
        with pytest.raises(ValueError):
            ChatClientSettings(tool_choice={"type": "auto", "name": "x"})._serialize()

    def test_invalid_type_rejected(self):
        with pytest.raises(ValueError, match="ToolChoice type"):
            ChatClientSettings(tool_choice={"type": "bogus"})._serialize()
