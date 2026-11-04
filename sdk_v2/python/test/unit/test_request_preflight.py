# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Focused tests for synchronous request preflight and cancellation."""

from __future__ import annotations

import importlib
import threading
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from types import SimpleNamespace

import pytest

from foundry_local_sdk import RequestPreflightResult
from foundry_local_sdk._native import ffi
from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.request import Request, _CancellationState
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
    session._stream_queue = None
    session._operation_lock = threading.RLock()
    session._native_call_state = threading.local()
    session._manager = None
    session._streaming_enabled = True
    session._streaming_in_flight = threading.Lock()
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


@pytest.mark.parametrize("streaming", [False, True])
def test_cancellation_event_retries_across_native_admission_and_stops_before_reuse(
    monkeypatch: pytest.MonkeyPatch,
    streaming: bool,
) -> None:
    entered = threading.Event()
    first_cancel = threading.Event()
    admitted = threading.Event()
    canceled = threading.Event()
    cancel_event = threading.Event()
    cancel_calls: list[object] = []
    canceled_status = object()
    response = object()

    def process(_session, _request, _out):
        entered.set()
        assert first_cancel.wait(timeout=2)
        admitted.set()
        assert canceled.wait(timeout=2)
        return canceled_status

    def cancel(request):
        cancel_calls.append(request)
        first_cancel.set()
        if admitted.is_set():
            canceled.set()
        return ffi.NULL

    def check_status(status):
        if status is canceled_status:
            raise FoundryLocalException("Request canceled.", error_code=5)
        assert_null(status)

    fake_api = SimpleNamespace(
        inference=SimpleNamespace(
            Session_ProcessRequest=process,
            Request_Cancel=cancel,
            Request_Release=lambda _ptr: None,
            Session_Release=lambda _ptr: None,
        ),
        check_status=check_status,
    )
    _install_native(monkeypatch, fake_api)
    response_module = importlib.import_module("foundry_local_sdk.response")
    monkeypatch.setattr(response_module, "Response", lambda _ptr: response)
    with _fake_session() as session, _fake_request() as request, ThreadPoolExecutor(max_workers=1) as pool:
        def run():
            if streaming:
                with session.process_streaming_request(request, cancel_event=cancel_event) as stream:
                    list(stream)
            else:
                session.process_request(request, cancel_event=cancel_event)

        pending = pool.submit(run)
        assert entered.wait(timeout=2)
        cancel_event.set()
        with pytest.raises(FoundryLocalException) as error:
            pending.result(timeout=2)
        assert error.value.error_code == 5
        assert len(cancel_calls) >= 2

        attempts = len(cancel_calls)
        fake_api.inference.Session_ProcessRequest = lambda *_args: ffi.NULL
        assert session.process_request(request) is response
        assert len(cancel_calls) == attempts


def test_pre_set_cancellation_event_never_submits_inference(monkeypatch: pytest.MonkeyPatch) -> None:
    cancel_event = threading.Event()
    cancel_event.set()
    fake_api = SimpleNamespace(
        inference=SimpleNamespace(
            Session_ProcessRequest=lambda *_args: pytest.fail("Canceled request reached inference"),
            Request_Release=lambda _ptr: None,
            Session_Release=lambda _ptr: None,
        ),
    )
    _install_native(monkeypatch, fake_api)
    with _fake_session() as session, _fake_request() as request:
        with pytest.raises(FoundryLocalException) as error:
            session.process_request(request, cancel_event=cancel_event)
        assert error.value.error_code == 5


@pytest.mark.parametrize("streaming", [False, True])
@pytest.mark.parametrize("shared_request", [False, True])
def test_queued_cancellation_leaves_owner_running_and_allows_reuse(
    monkeypatch: pytest.MonkeyPatch, streaming: bool, shared_request: bool
) -> None:
    entered = threading.Event()
    release = threading.Event()
    queued = threading.Event()

    class ObservedCancelEvent(threading.Event):
        def is_set(self) -> bool:
            queued.set()
            return super().is_set()

    cancel_event = ObservedCancelEvent()
    process_calls = []
    streamed_items = []
    response = object()

    def process(_session, _request, _out):
        process_calls.append(_request)
        entered.set()
        assert release.wait(timeout=5)
        return ffi.NULL

    fake_api = SimpleNamespace(
        inference=SimpleNamespace(
            Session_ProcessRequest=process,
            Request_Cancel=lambda _ptr: pytest.fail("Queued cancellation must not cancel the owner"),
            Request_Release=lambda _ptr: None,
            Session_Release=lambda _ptr: None,
        ),
        check_status=assert_null,
    )
    _install_native(monkeypatch, fake_api)
    response_module = importlib.import_module("foundry_local_sdk.response")
    monkeypatch.setattr(response_module, "Response", lambda _ptr: response)

    with (
        _fake_session() as owner,
        _fake_session() as other_session,
        _fake_request() as request,
        _fake_request() as other_request,
        ThreadPoolExecutor(max_workers=2) as pool,
    ):
        contender_session = other_session if shared_request else owner
        contender_request = request if shared_request else other_request

        def run_contender():
            if streaming:
                with contender_session.process_streaming_request(contender_request, cancel_event=cancel_event) as stream:
                    for item in stream:
                        streamed_items.append(item)
            else:
                contender_session.process_request(contender_request, cancel_event=cancel_event)

        active = pool.submit(owner.process_request, request)
        pending = None
        try:
            assert entered.wait(timeout=2)
            pending = pool.submit(run_contender)
            assert queued.wait(timeout=2)
            if streaming and not shared_request and owner._stream_queue is not None:
                # Simulate the active owner's callback while the other request is still queued.
                owner._stream_queue.put(object())
            cancel_event.set()
            with pytest.raises(FoundryLocalException) as error:
                pending.result(timeout=2)
            assert error.value.error_code == 5
            assert not active.done()
            assert len(process_calls) == 1
            assert streamed_items == []
        finally:
            release.set()
            assert active.result(timeout=2) is response
            if pending is not None:
                try:
                    pending.result(timeout=2)
                except FoundryLocalException:
                    pass

        assert contender_session.process_request(contender_request) is response
        assert len(process_calls) == 2


def test_unset_event_does_not_wait_for_cancellation_cleanup_lock() -> None:
    request = _fake_request()
    request._closed = True
    state = _CancellationState(request, threading.Event())
    with ThreadPoolExecutor(max_workers=1) as pool:
        state.lock.acquire()
        try:
            pool.submit(state.retry).result(timeout=2)
        finally:
            state.lock.release()


def test_completed_request_wakes_cancellation_cleanup_without_waiting_for_cancel_event() -> None:
    helper_started = threading.Event()
    release_cancel_wait = threading.Event()
    processing_thread: threading.Thread | None = None

    class UnsetCancelEvent(threading.Event):
        def is_set(self) -> bool:
            if threading.current_thread() is not processing_thread:
                helper_started.set()
            return False

        def wait(self, timeout: float | None = None) -> bool:
            helper_started.set()
            release_cancel_wait.wait(timeout=2)
            return False

    request = _fake_request()
    request._closed = True

    def complete_request() -> None:
        nonlocal processing_thread
        processing_thread = threading.current_thread()
        with request._cancellation(UnsetCancelEvent()):
            assert helper_started.wait(timeout=1)

    with ThreadPoolExecutor(max_workers=1) as pool:
        completed = pool.submit(complete_request)
        try:
            completed.result(timeout=1)
        finally:
            release_cancel_wait.set()


def test_concurrent_cancellation_uses_one_dispatcher_and_stops_before_reuse(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    requests = [_fake_request() for _ in range(16)]
    for request in requests:
        request._closed = True
    cancel_event = threading.Event()
    canceled = threading.Event()
    threads: set[int] = set()
    seen: set[int] = set()
    attempts: list[int] = []
    lock = threading.Lock()

    def cancel(request: Request) -> None:
        with lock:
            threads.add(threading.get_ident())
            seen.add(id(request))
            attempts.append(id(request))
            if len(seen) == len(requests):
                canceled.set()

    monkeypatch.setattr(Request, "cancel", cancel)
    with ExitStack() as stack:
        for request in requests:
            stack.enter_context(request._cancellation(cancel_event))
        cancel_event.set()
        assert canceled.wait(timeout=2)
        assert len(threads) == 1

    attempts_at_release = len(attempts)
    with requests[0]._cancellation(threading.Event()):
        assert not threading.Event().wait(timeout=0.03)
    assert len(attempts) == attempts_at_release


def test_cancellation_cleanup_joins_active_dispatcher_attempt(monkeypatch: pytest.MonkeyPatch) -> None:
    request = _fake_request()
    request._closed = True
    cancel_event = threading.Event()
    entered = threading.Event()
    release = threading.Event()
    closing = threading.Event()

    def cancel(_request: Request) -> None:
        entered.set()
        assert release.wait(timeout=2)

    monkeypatch.setattr(Request, "cancel", cancel)

    def process() -> None:
        with request._cancellation(cancel_event):
            cancel_event.set()
            assert entered.wait(timeout=2)
            closing.set()

    with ThreadPoolExecutor(max_workers=1) as pool:
        pending = pool.submit(process)
        try:
            assert closing.wait(timeout=2)
            assert not pending.done()
        finally:
            release.set()
        pending.result(timeout=2)
