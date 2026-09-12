# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for the flToolDefinition binding layout and API version stamping.

The cffi extension is compiled against the real ``foundry_local_c.h``, so these assertions are the
Python-side half of the ABI contract: the struct this package fills in has to be the struct the
native library reads, and the version stamped on it has to be the version requested from
``FoundryLocalGetApi``. No model is loaded.
"""
from __future__ import annotations

import importlib
from types import SimpleNamespace

import pytest

from foundry_local_sdk._native import ffi
from foundry_local_sdk._native import _cffi_bindings
from foundry_local_sdk._native.api import _FOUNDRY_LOCAL_API_VERSION
from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.session import ChatSession
from foundry_local_sdk.session import _TOOL_KIND_CUSTOM, _TOOL_KIND_FUNCTION, _API_VERSION


class TestToolDefinitionAbi:
    def test_kind_is_appended_after_the_legacy_prefix(self):
        pointer_size = ffi.sizeof("void *")
        assert ffi.offsetof("flToolDefinition", "json_schema") == 3 * pointer_size
        assert ffi.offsetof("flToolDefinition", "kind") == 4 * pointer_size
        assert ffi.sizeof("flToolDefinition") == 5 * pointer_size

    def test_kind_is_a_fixed_width_32_bit_field(self):
        # flToolKind is a uint32_t typedef, not an enum, so its width is the same on every
        # toolchain and every binding lays the struct out identically.
        assert ffi.sizeof("flToolKind") == 4

    def test_tool_kind_values_match_the_header(self):
        # Read back through the compiled extension, so these are the header's values rather than a
        # second copy of them.
        assert _TOOL_KIND_FUNCTION == _cffi_bindings.lib.FOUNDRY_LOCAL_TOOL_KIND_FUNCTION
        assert _TOOL_KIND_CUSTOM == _cffi_bindings.lib.FOUNDRY_LOCAL_TOOL_KIND_CUSTOM
        assert _TOOL_KIND_FUNCTION == 0
        assert _TOOL_KIND_CUSTOM == 1

    def test_stamped_version_matches_the_requested_version(self):
        # Stamping a version the requested API table does not support would be silently wrong: the
        # native side reads `kind` only from a version 2 definition.
        assert _API_VERSION == _FOUNDRY_LOCAL_API_VERSION
        assert _API_VERSION == 2

    def test_public_custom_tool_api_encodes_definition_and_handles_duplicates_and_removal(
        self, monkeypatch
    ):
        native_api_module = importlib.import_module("foundry_local_sdk._native.api")
        duplicate_status = object()
        definitions: dict[bytes, dict[str, object]] = {}

        def add_tool_definition(_session, definition):
            encoded_name = ffi.string(definition.name)
            if encoded_name in definitions:
                return duplicate_status
            definitions[encoded_name] = {
                "version": int(definition.version),
                "name": encoded_name,
                "description": ffi.string(definition.description),
                "schema_is_null": definition.json_schema == ffi.NULL,
                "schema": ffi.string(definition.json_schema),
                "kind": int(definition.kind),
            }
            return ffi.NULL

        def remove_tool_definition(_session, name, out_removed):
            out_removed[0] = definitions.pop(ffi.string(name), None) is not None
            return ffi.NULL

        def check_status(status):
            if status is duplicate_status:
                raise FoundryLocalException("tool name is already registered", error_code=1)
            assert status == ffi.NULL

        fake_api = SimpleNamespace(
            inference=SimpleNamespace(
                Session_AddToolDefinition=add_tool_definition,
                Session_RemoveToolDefinition=remove_tool_definition,
            ),
            check_status=check_status,
        )
        monkeypatch.setattr(native_api_module, "api", fake_api)

        session = ChatSession.__new__(ChatSession)
        session._closed = False
        session._ptr = ffi.cast("flSession *", 1)
        session._stream_thread = None
        session._stream_request = None

        try:
            assert session.add_custom_tool_definition("réparer 🛠️", "Applique le correctif — 安全") is session
            assert definitions[b"r\xc3\xa9parer \xf0\x9f\x9b\xa0\xef\xb8\x8f"] == {
                "version": _API_VERSION,
                "name": b"r\xc3\xa9parer \xf0\x9f\x9b\xa0\xef\xb8\x8f",
                "description": b"Applique le correctif \xe2\x80\x94 \xe5\xae\x89\xe5\x85\xa8",
                "schema_is_null": False,
                "schema": b"",
                "kind": _TOOL_KIND_CUSTOM,
            }

            with pytest.raises(FoundryLocalException, match="already registered"):
                session.add_tool_definition("réparer 🛠️", "duplicate across kinds", "{}")

            assert session.remove_tool_definition("réparer 🛠️") is True
            assert session.remove_tool_definition("réparer 🛠️") is False
            assert session.add_custom_tool_definition("réparer 🛠️", "registered again") is session
        finally:
            # This fabricated unit-test handle is not owned by the native runtime.
            session._ptr = None
            session._closed = True

    @pytest.mark.parametrize(
        ("operation", "argument_name"),
        [
            (lambda session: session.add_tool_definition("bad\x00name", "description", "{}"), "name"),
            (lambda session: session.add_tool_definition("name", "bad\x00description", "{}"), "description"),
            (lambda session: session.add_tool_definition("name", "description", "{\x00}"), "json_schema"),
            (lambda session: session.add_custom_tool_definition("bad\x00name", "description"), "name"),
            (lambda session: session.add_custom_tool_definition("name", "bad\x00description"), "description"),
            (lambda session: session.remove_tool_definition("bad\x00name"), "name"),
        ],
    )
    def test_public_tool_apis_reject_embedded_nul_before_native_call(
        self, monkeypatch, operation, argument_name
    ):
        native_api_module = importlib.import_module("foundry_local_sdk._native.api")

        def unexpected_native_call(*_args):
            pytest.fail("native API must not be called for an argument containing NUL")

        fake_api = SimpleNamespace(
            inference=SimpleNamespace(
                Session_AddToolDefinition=unexpected_native_call,
                Session_RemoveToolDefinition=unexpected_native_call,
            )
        )
        monkeypatch.setattr(native_api_module, "api", fake_api)

        session = ChatSession.__new__(ChatSession)
        session._closed = False
        session._ptr = ffi.cast("flSession *", 1)
        session._stream_thread = None
        session._stream_request = None

        try:
            with pytest.raises(ValueError, match=rf"^{argument_name} must not contain an embedded NUL character$"):
                operation(session)
        finally:
            session._ptr = None
            session._closed = True
