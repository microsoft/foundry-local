# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Focused tests for synchronous request preflight."""

from __future__ import annotations

import importlib
import threading
from concurrent.futures import ThreadPoolExecutor
from types import SimpleNamespace

import pytest

from foundry_local_sdk import RequestPreflightResult
from foundry_local_sdk._native import ffi
from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.request import Request
from foundry_local_sdk.session import ChatSession, _API_VERSION

_SESSION_PTR = ffi.cast("flSession *", 1)
_REQUEST_PTR = ffi.cast("flRequest *", 2)
_PREFLIGHT_PTR = ffi.cast("flRequestPreflight *", 3)


def _fake_session() -> ChatSession:
    session = ChatSession.__new__(ChatSession)
    session._closed = False
    session._ptr = _SESSION_PTR
    session._stream_thread = None
    session._stream_request = None
    session._operation_lock = threading.RLock()
    session._native_call_state = threading.local()
    session._manager = None
    return session


def _fake_request() -> Request:
    request = Request.__new__(Request)
    request._ptr = _REQUEST_PTR
    request._closed = False
    request._lifetime_lock = threading.RLock()
    return request


def _install_native(monkeypatch: pytest.MonkeyPatch, api: object) -> None:
    native_api_module = importlib.import_module("foundry_local_sdk._native.api")
    monkeypatch.setattr(native_api_module, "api", api)


def test_preflight_request_maps_result_and_releases_operation(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[str] = []

    def create(session_ptr, request_ptr, out):
        assert session_ptr == _SESSION_PTR
        assert request_ptr == _REQUEST_PTR
        out[0] = _PREFLIGHT_PTR
        calls.append("create")
        return ffi.NULL

    def execute(operation, result):
        assert operation == _PREFLIGHT_PTR
        assert result.version == _API_VERSION
        result.prompt_tokens = 11
        result.output_reserve_tokens = 7
        result.required_tokens = 18
        result.context_limit_tokens = 16
        result.fits = False
        result.deficit_tokens = 2
        calls.append("execute")
        return ffi.NULL

    fake_api = SimpleNamespace(
        inference=SimpleNamespace(
            Session_CreateRequestPreflight=create,
            RequestPreflight_Execute=execute,
            RequestPreflight_Release=lambda _operation: calls.append("release"),
        ),
        check_status=lambda status: assert_null(status),
    )
    _install_native(monkeypatch, fake_api)

    result = _fake_session().preflight_request(_fake_request())

    assert result == RequestPreflightResult(11, 7, 18, 16, False, 2)
    assert calls == ["create", "execute", "release"]


def test_preflight_request_execute_error_releases_operation(monkeypatch: pytest.MonkeyPatch) -> None:
    execute_status = object()
    releases = []

    def create(_session_ptr, _request_ptr, out):
        out[0] = _PREFLIGHT_PTR
        return ffi.NULL

    def check_status(status):
        if status is execute_status:
            raise FoundryLocalException("preflight failed")
        assert_null(status)

    fake_api = SimpleNamespace(
        inference=SimpleNamespace(
            Session_CreateRequestPreflight=create,
            RequestPreflight_Execute=lambda _operation, _result: execute_status,
            RequestPreflight_Release=releases.append,
        ),
        check_status=check_status,
    )
    _install_native(monkeypatch, fake_api)

    with pytest.raises(FoundryLocalException, match="preflight failed"):
        _fake_session().preflight_request(_fake_request())

    assert releases == [_PREFLIGHT_PTR]


def test_preflight_capture_keeps_session_and_request_alive_during_concurrent_close(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    entered = threading.Event()
    continue_capture = threading.Event()
    releases: list[str] = []

    def create(session_ptr, request_ptr, out):
        assert session_ptr == _SESSION_PTR
        assert request_ptr == _REQUEST_PTR
        entered.set()
        assert continue_capture.wait(timeout=5)
        assert releases == []
        out[0] = _PREFLIGHT_PTR
        return ffi.NULL

    def execute(_operation, result):
        result.context_limit_tokens = 100
        result.fits = True
        return ffi.NULL

    fake_api = SimpleNamespace(
        inference=SimpleNamespace(
            Session_CreateRequestPreflight=create,
            RequestPreflight_Execute=execute,
            RequestPreflight_Release=lambda _ptr: releases.append("preflight"),
            Request_Release=lambda _ptr: releases.append("request"),
            Session_Release=lambda _ptr: releases.append("session"),
        ),
        check_status=assert_null,
    )
    _install_native(monkeypatch, fake_api)
    session, request = _fake_session(), _fake_request()
    with ThreadPoolExecutor(max_workers=3) as pool:
        result = pool.submit(session.preflight_request, request)
        assert entered.wait(timeout=5)
        request_close = pool.submit(request._close)
        session_close = pool.submit(session._close)
        continue_capture.set()
        assert result.result(timeout=5).fits
        request_close.result(timeout=5)
        session_close.result(timeout=5)

    assert set(releases) == {"preflight", "request", "session"}


def assert_null(status) -> None:
    assert status == ffi.NULL
