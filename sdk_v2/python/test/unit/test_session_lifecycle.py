from __future__ import annotations

import builtins
import sys
import threading
import types
from collections.abc import Callable
from typing import Any, cast

import pytest

from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.request import Request
from foundry_local_sdk.response import Response
from foundry_local_sdk.session import Session, _SessionState


class _FakeFfi:
    def new(self, declaration: str) -> list[object | None]:
        assert declaration == "flResponse**"
        return [None]


class _FakeRequest:
    def __init__(self) -> None:
        self._ptr = object()
        self.timeouts: list[float | None] = []

    def set_timeout(self, timeout: float | None) -> None:
        self.timeouts.append(timeout)

    def cancel(self) -> None:
        pass


class _RecordingCondition(threading.Condition):
    def __init__(self) -> None:
        super().__init__()
        self.wait_started = threading.Event()
        self.wait_timeouts: list[float | None] = []

    def wait(self, timeout: float | None = None) -> bool:
        self.wait_timeouts.append(timeout)
        self.wait_started.set()
        return super().wait(timeout)


class _NeverWaitCondition(threading.Condition):
    def wait(self, timeout: float | None = None) -> bool:
        raise AssertionError("finalizer must not wait")


@pytest.fixture
def fake_native(monkeypatch: pytest.MonkeyPatch) -> types.SimpleNamespace:
    inference = types.SimpleNamespace(
        Session_ProcessRequest=lambda *_: None,
        Response_Release=lambda *_: None,
        Request_SetTimeoutMs=lambda *_: None,
        Request_Release=lambda *_: None,
    )
    api = types.SimpleNamespace(inference=inference, check_status=lambda status: None)
    native_module = types.ModuleType("foundry_local_sdk._native")
    ffi = _FakeFfi()
    setattr(native_module, "ffi", ffi)
    setattr(native_module, "api", api)
    api_module = types.ModuleType("foundry_local_sdk._native.api")
    setattr(api_module, "api", api)
    setattr(api_module, "ffi", ffi)
    monkeypatch.setitem(sys.modules, "foundry_local_sdk._native", native_module)
    monkeypatch.setitem(sys.modules, "foundry_local_sdk._native.api", api_module)
    return types.SimpleNamespace(api=api, inference=inference)


def _make_session(
    *,
    condition: threading.Condition | None = None,
    release: Callable[[object], None] | None = None,
    cancel: Callable[[object], object] | None = None,
) -> Session:
    session = object.__new__(Session)
    session._closed = False
    session._ptr = object()
    session._stream_thread = None
    session._stream_request = None
    session._state_condition = condition or threading.Condition()
    session._session_state = _SessionState.OPEN
    session._active_native_calls = 0
    session._release_session = release or (lambda _: None)
    session._cancel_session = cancel or (lambda _: None)
    session._check_status = lambda status: None
    session._streaming_enabled = False
    session._streaming_callback = None
    session._stream_queue = None
    session._streaming_in_flight = threading.Lock()
    return session


def _join(thread: threading.Thread) -> None:
    thread.join(timeout=2)
    assert not thread.is_alive()


def test_close_waits_for_synchronous_request_without_timeout_and_rejects_new_calls(
    fake_native: types.SimpleNamespace,
) -> None:
    process_started = threading.Event()
    allow_process_to_finish = threading.Event()
    cancel_called = threading.Event()
    releases: list[object] = []
    errors: list[BaseException] = []
    condition = _RecordingCondition()

    def process(_session_ptr: object, _request_ptr: object, out: list[object | None]) -> None:
        process_started.set()
        assert allow_process_to_finish.wait(timeout=2)
        out[0] = object()

    fake_native.inference.Session_ProcessRequest = process
    session = _make_session(
        condition=condition,
        release=releases.append,
        cancel=lambda _: cancel_called.set(),
    )
    request = cast(Request, _FakeRequest())
    responses: list[Response] = []

    def run_request() -> None:
        try:
            responses.append(session.process_request(request))
        except BaseException as exc:
            errors.append(exc)

    request_thread = threading.Thread(target=run_request)
    request_thread.start()
    assert process_started.wait(timeout=2)

    close_thread = threading.Thread(target=session._close)
    close_thread.start()
    assert cancel_called.wait(timeout=2)
    assert condition.wait_started.wait(timeout=2)
    assert releases == []
    assert close_thread.is_alive()

    with pytest.raises(FoundryLocalException, match="closed"):
        session.process_request(request)
    with pytest.raises(FoundryLocalException, match="closed"):
        session.process_streaming_request(request)

    allow_process_to_finish.set()
    _join(request_thread)
    _join(close_thread)

    assert errors == []
    assert len(responses) == 1
    assert len(releases) == 1
    assert condition.wait_timeouts
    assert all(timeout is None for timeout in condition.wait_timeouts)
    responses[0]._close()


def test_close_waits_for_streaming_worker_before_release(
    fake_native: types.SimpleNamespace,
) -> None:
    process_started = threading.Event()
    allow_process_to_finish = threading.Event()
    cancel_called = threading.Event()
    releases: list[object] = []

    def process(_session_ptr: object, _request_ptr: object, out: list[object | None]) -> None:
        process_started.set()
        assert allow_process_to_finish.wait(timeout=2)
        out[0] = object()

    fake_native.inference.Session_ProcessRequest = process
    session = _make_session(release=releases.append, cancel=lambda _: cancel_called.set())
    session._streaming_enabled = True
    request = cast(Request, _FakeRequest())

    stream = session.process_streaming_request(request)
    assert process_started.wait(timeout=2)

    close_thread = threading.Thread(target=session._close)
    close_thread.start()
    assert cancel_called.wait(timeout=2)
    assert releases == []

    allow_process_to_finish.set()
    _join(close_thread)
    assert len(releases) == 1

    assert list(stream) == []
    stream.final_response._close()


def test_streaming_thread_start_failure_releases_call_lease(
    fake_native: types.SimpleNamespace,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    session = _make_session()
    session._streaming_enabled = True
    request = cast(Request, _FakeRequest())

    def fail_start(_thread: threading.Thread) -> None:
        raise RuntimeError("thread start failed")

    monkeypatch.setattr(threading.Thread, "start", fail_start)

    with pytest.raises(RuntimeError, match="thread start failed"):
        session.process_streaming_request(request)

    assert session._active_native_calls == 0
    assert session._stream_thread is None
    assert session._stream_request is None
    assert session._stream_queue is None
    assert session._streaming_in_flight.acquire(blocking=False)
    session._streaming_in_flight.release()
    session._close()


def test_concurrent_and_repeated_close_releases_exactly_once() -> None:
    callers = 4
    start = threading.Barrier(callers + 1)
    release_started = threading.Event()
    allow_release_to_finish = threading.Event()
    releases: list[object] = []
    cancellations: list[object] = []

    def release(ptr: object) -> None:
        releases.append(ptr)
        release_started.set()
        assert allow_release_to_finish.wait(timeout=2)

    session = _make_session(release=release, cancel=cancellations.append)

    def close_session() -> None:
        start.wait()
        session._close()

    threads = [threading.Thread(target=close_session) for _ in range(callers)]
    for thread in threads:
        thread.start()
    start.wait()

    assert release_started.wait(timeout=2)
    assert len(releases) == 1
    allow_release_to_finish.set()
    for thread in threads:
        _join(thread)

    session._close()
    assert len(cancellations) == 1
    assert len(releases) == 1


def test_finalizer_with_active_call_never_waits_or_releases() -> None:
    releases: list[object] = []
    cancellations: list[object] = []
    session = _make_session(
        condition=_NeverWaitCondition(),
        release=releases.append,
        cancel=cancellations.append,
    )
    session._active_native_calls = 1
    ptr = session._ptr

    session.__del__()

    assert session._ptr is ptr
    assert session._session_state is _SessionState.OPEN
    assert releases == []
    assert cancellations == []


def test_finalizer_does_not_block_on_contended_lifecycle_lock() -> None:
    releases: list[object] = []
    session = _make_session(release=releases.append)
    finished = threading.Event()

    def finalize() -> None:
        session.__del__()
        finished.set()

    with session._state_condition:
        thread = threading.Thread(target=finalize)
        thread.start()
        assert finished.wait(timeout=2)

    _join(thread)
    assert releases == []
    assert session._session_state is _SessionState.OPEN

    session._close()
    assert len(releases) == 1


def test_finalizer_releases_idle_session_without_native_import(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    releases: list[object] = []
    session = _make_session(release=releases.append)
    real_import = builtins.__import__

    def guarded_import(
        name: str,
        globals: dict[str, Any] | None = None,
        locals: dict[str, Any] | None = None,
        fromlist: tuple[str, ...] = (),
        level: int = 0,
    ) -> Any:
        if name.startswith("foundry_local_sdk._native"):
            raise AssertionError("finalizer must not import the native API")
        return real_import(name, globals, locals, fromlist, level)

    monkeypatch.setattr(builtins, "__import__", guarded_import)
    session.__del__()

    assert len(releases) == 1
    assert session._session_state is _SessionState.CLOSED
    assert session._ptr is None


@pytest.mark.parametrize(
    "timeout",
    [
        float("nan"),
        float("inf"),
        float("-inf"),
        (1 << 63) / 1000,
        float.fromhex("0x1.fffffffffffffp+1023"),
        1 << 4096,
    ],
)
def test_request_timeout_rejects_nonfinite_or_native_range_overflow(
    fake_native: types.SimpleNamespace,
    timeout: float,
) -> None:
    calls: list[int] = []
    fake_native.inference.Request_SetTimeoutMs = lambda _ptr, value: calls.append(value)
    request = object.__new__(Request)
    request._closed = False
    request._ptr = object()

    with pytest.raises(ValueError, match="timeout"):
        request.set_timeout(timeout)

    request._closed = True
    assert calls == []


@pytest.mark.parametrize(
    ("timeout", "expected_ms"),
    [(None, 0), (-1.0, 0), (0.0, 0), (0.0001, 1), (0.001, 1), (1.25, 1250)],
)
def test_request_timeout_marshals_valid_values(
    fake_native: types.SimpleNamespace,
    timeout: float | None,
    expected_ms: int,
) -> None:
    calls: list[int] = []
    fake_native.inference.Request_SetTimeoutMs = lambda _ptr, value: calls.append(value)
    request = object.__new__(Request)
    request._closed = False
    request._ptr = object()

    request.set_timeout(timeout)

    request._closed = True
    assert calls == [expected_ms]
