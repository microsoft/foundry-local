# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Focused ABI and wrapper tests for request preflight."""
from __future__ import annotations

import dataclasses
import importlib
from types import SimpleNamespace

import pytest

from foundry_local_sdk import RequestPreflight
from foundry_local_sdk._native import ffi
from foundry_local_sdk._native.api import _Api, _FOUNDRY_LOCAL_API_VERSION
from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.session import ChatSession, _REQUEST_PREFLIGHT_VERSION


def test_request_preflight_layout_matches_v3_header() -> None:
    assert ffi.offsetof("flRequestPreflight", "version") == 0
    assert ffi.offsetof("flRequestPreflight", "prompt_tokens") == 8
    assert ffi.offsetof("flRequestPreflight", "deficit_tokens") == 48
    assert ffi.sizeof("flRequestPreflight") == 56


def test_inference_api_v3_appends_preflight_at_slot_23() -> None:
    pointer_size = ffi.sizeof("void *")
    assert ffi.offsetof("flInferenceApi", "Session_PreflightRequest") == 22 * pointer_size
    assert ffi.sizeof("flInferenceApi") == 23 * pointer_size


def test_preflight_versions_match_required_v3_api() -> None:
    assert _REQUEST_PREFLIGHT_VERSION == 3
    assert _FOUNDRY_LOCAL_API_VERSION == 3


def test_api_loader_requests_v3(monkeypatch: pytest.MonkeyPatch) -> None:
    native_api_module = importlib.import_module("foundry_local_sdk._native.api")
    requested_versions: list[int] = []
    root = SimpleNamespace(
        GetItemApi=lambda: object(),
        GetInferenceApi=lambda: object(),
        GetConfigurationApi=lambda: object(),
        GetCatalogApi=lambda: object(),
        GetModelApi=lambda: object(),
    )

    def get_api(version: int):
        requested_versions.append(version)
        return root

    monkeypatch.setattr(native_api_module, "_lib", SimpleNamespace(FoundryLocalGetApi=get_api))

    loaded = _Api()

    assert requested_versions == [3]
    assert loaded.root is root


def test_api_loader_rejects_runtime_without_v3(monkeypatch: pytest.MonkeyPatch) -> None:
    native_api_module = importlib.import_module("foundry_local_sdk._native.api")
    monkeypatch.setattr(
        native_api_module,
        "_lib",
        SimpleNamespace(FoundryLocalGetApi=lambda version: ffi.NULL),
    )

    with pytest.raises(FoundryLocalException, match=r"FoundryLocalGetApi\(3\) returned NULL"):
        _Api()


def test_request_preflight_is_frozen() -> None:
    result = RequestPreflight(10, 20, 30, 40, True, 0)
    with pytest.raises(dataclasses.FrozenInstanceError):
        result.prompt_tokens = 1  # type: ignore[misc]


def test_chat_session_preflight_maps_native_result(monkeypatch: pytest.MonkeyPatch) -> None:
    native_api_module = importlib.import_module("foundry_local_sdk._native.api")
    observed: dict[str, object] = {}

    def preflight_request(session_ptr, request_ptr, out_preflight):
        observed["session_ptr"] = session_ptr
        observed["request_ptr"] = request_ptr
        observed["version"] = int(out_preflight.version)
        out_preflight.prompt_tokens = 101
        out_preflight.output_reserve_tokens = 32
        out_preflight.required_tokens = 133
        out_preflight.context_limit_tokens = 128
        out_preflight.fits = 0
        out_preflight.deficit_tokens = 5
        return ffi.NULL

    fake_api = SimpleNamespace(
        inference=SimpleNamespace(Session_PreflightRequest=preflight_request),
        check_status=lambda status: observed.setdefault("status", status),
    )
    monkeypatch.setattr(native_api_module, "api", fake_api)

    session = ChatSession.__new__(ChatSession)
    session._closed = False
    session._ptr = ffi.cast("flSession *", 1)
    request = SimpleNamespace(_ptr=ffi.cast("flRequest *", 2))

    try:
        result = session.preflight_request(request)
    finally:
        session._ptr = None
        session._closed = True

    assert observed == {
        "session_ptr": ffi.cast("flSession *", 1),
        "request_ptr": ffi.cast("flRequest *", 2),
        "version": 3,
        "status": ffi.NULL,
    }
    assert result == RequestPreflight(
        prompt_tokens=101,
        output_reserve_tokens=32,
        required_tokens=133,
        context_limit_tokens=128,
        fits=False,
        deficit_tokens=5,
    )
