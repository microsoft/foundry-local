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
from foundry_local_sdk.session import ChatSession, Session, StreamingResponse, _SessionState


class _FakeFfi:
    NULL = None

    def new(self, declaration: str, initializer: object = None) -> object:
        match declaration:
            case "flResponse**" | "flKeyValuePairs**" | "flItem**":
                return [None]
            case "char[]":
                return initializer
            case "flToolDefinition*":
                return types.SimpleNamespace()
            case "bool*":
                return [False]
            case _:
                raise AssertionError(f"Unexpected declaration: {declaration}")

    def callback(self, declaration: str, callback: Callable[..., object]) -> Callable[..., object]:
        assert declaration == "flStreamingCallback"
        return callback

    def cast(self, declaration: str, value: object) -> object:
        assert declaration == "flStreamingCallback"
        return value


class _FakeRequest:
    def __init__(self) -> None:
        self._ptr = object()
        self.timeouts: list[float | None] = []
        self._active_processes = 0

    def set_timeout(self, timeout: float | None) -> None:
        self.timeouts.append(timeout)

    def cancel(self) -> None:
        pass

    def _acquire_for_processing(self) -> object:
        self._active_processes += 1
        return self._ptr

    def _release_after_processing(self) -> None:
        self._active_processes -= 1


def _make_request(*, release: Callable[[object], object] | None = None) -> Request:
    request = object.__new__(Request)
    request._closed = False
    request._ptr = object()
    request._lock = threading.Condition()
    request._dispose_requested = False
    request._active_processes = 0
    request._release_request = release or (lambda _: None)
    return request


class _FakeOptions:
    def to_native_options(self) -> dict[str, str]:
        return {"key": "value"}


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
        Session_AddToolDefinition=lambda *_: None,
        Session_GetTurnCount=lambda *_: 0,
        Session_ProcessRequest=lambda *_: None,
        Session_RemoveToolDefinition=lambda *_: None,
        Session_SetOptions=lambda *_: None,
        Session_SetStreamingCallback=lambda *_: None,
        Session_UndoTurns=lambda *_: None,
        Response_Release=lambda *_: None,
        Request_Cancel=lambda *_: None,
        Request_SetTimeoutMs=lambda *_: None,
        Request_Release=lambda *_: None,
    )
    root = types.SimpleNamespace(
        AddKeyValuePair=lambda *_: None,
        CreateKeyValuePairs=lambda out: out.__setitem__(0, object()),
        KeyValuePairs_Release=lambda *_: None,
    )
    item = types.SimpleNamespace(Item_Release=lambda *_: None, ItemQueue_TryPop=lambda *_: False)
    api = types.SimpleNamespace(inference=inference, item=item, root=root, check_status=lambda status: None)
    native_module = types.ModuleType("foundry_local_sdk._native")
    ffi = _FakeFfi()
    setattr(native_module, "ffi", ffi)
    setattr(native_module, "api", api)
    api_module = types.ModuleType("foundry_local_sdk._native.api")
    setattr(api_module, "api", api)
    setattr(api_module, "ffi", ffi)
    monkeypatch.setitem(sys.modules, "foundry_local_sdk._native", native_module)
    monkeypatch.setitem(sys.modules, "foundry_local_sdk._native.api", api_module)
    return types.SimpleNamespace(api=api, ffi=ffi, inference=inference)


def _make_session(
    *,
    condition: threading.Condition | None = None,
    release: Callable[[object], None] | None = None,
    cancel: Callable[[object], object] | None = None,
    session_type: type[Session] = Session,
) -> Session:
    session = object.__new__(session_type)
    session._closed = False
    session._ptr = object()
    session._stream_thread = None
    session._stream_request = None
    session._state_condition = condition or threading.Condition()
    session._session_state = _SessionState.OPEN
    session._active_native_calls = 0
    session._active_nonstream_requests = 0
    session._streaming_request_active = False
    session._release_session = release or (lambda _: None)
    session._cancel_session = cancel or (lambda _: None)
    session._check_status = lambda status: None
    session._streaming_enabled = False
    session._streaming_callback = None
    session._stream_queue = None
    return session


def _join(thread: threading.Thread) -> None:
    thread.join(timeout=2)
    assert not thread.is_alive()


def _assert_processing_idle(session: Session) -> None:
    assert session._active_native_calls == 0
    assert session._active_nonstream_requests == 0
    assert not session._streaming_request_active


def _invoke_session_operation(session: ChatSession, operation: str) -> object:
    match operation:
        case "set_options":
            return session.set_options(cast(Any, _FakeOptions()))
        case "enable_streaming":
            return session.set_streaming(True)
        case "disable_streaming":
            session._streaming_enabled = True
            session._streaming_callback = cast(Any, object())
            return session.set_streaming(False)
        case "add_tool":
            return session.add_tool_definition("tool", "description", "{}")
        case "remove_tool":
            return session.remove_tool_definition("tool")
        case "turn_count":
            return session.turn_count
        case "undo_turns":
            session.undo_turns(1)
            return None
        case _:
            raise AssertionError(f"Unexpected operation: {operation}")


@pytest.mark.parametrize(
    ("operation", "native_name"),
    [
        ("set_options", "Session_SetOptions"),
        ("enable_streaming", "Session_SetStreamingCallback"),
        ("disable_streaming", "Session_SetStreamingCallback"),
        ("add_tool", "Session_AddToolDefinition"),
        ("remove_tool", "Session_RemoveToolDefinition"),
        ("turn_count", "Session_GetTurnCount"),
        ("undo_turns", "Session_UndoTurns"),
    ],
)
def test_close_waits_for_leased_session_native_operations_and_releases_once(
    fake_native: types.SimpleNamespace,
    operation: str,
    native_name: str,
) -> None:
    native_started = threading.Event()
    allow_native_to_finish = threading.Event()
    releases: list[object] = []
    errors: list[BaseException] = []
    condition = _RecordingCondition()

    def blocking_native_call(*_: object) -> object:
        native_started.set()
        assert allow_native_to_finish.wait(timeout=2)
        return 3 if native_name == "Session_GetTurnCount" else None

    setattr(fake_native.inference, native_name, blocking_native_call)
    session = cast(
        ChatSession,
        _make_session(
            condition=condition,
            release=releases.append,
            session_type=ChatSession,
        ),
    )

    def run_operation() -> None:
        try:
            _invoke_session_operation(session, operation)
        except BaseException as exc:
            errors.append(exc)

    operation_thread = threading.Thread(target=run_operation)
    operation_thread.start()
    assert native_started.wait(timeout=2)

    close_thread = threading.Thread(target=session._close)
    close_thread.start()
    assert condition.wait_started.wait(timeout=2)
    assert close_thread.is_alive()
    assert releases == []

    allow_native_to_finish.set()
    _join(operation_thread)
    _join(close_thread)

    session._close()
    assert errors == []
    assert len(releases) == 1


def test_streaming_callback_without_active_stream_drains_and_releases_items(
    fake_native: types.SimpleNamespace,
) -> None:
    item_queue = object()
    native_items = [object(), object(), object()]
    queued_items = list(native_items)
    released_items: list[object] = []

    def try_pop(queue_ptr: object, out: list[object | None]) -> bool:
        assert queue_ptr is item_queue
        if not queued_items:
            return False
        out[0] = queued_items.pop(0)
        return True

    fake_native.api.item.ItemQueue_TryPop = try_pop
    fake_native.api.item.Item_Release = released_items.append
    session = _make_session()
    session.set_streaming(True)

    assert session._stream_queue is None
    callback = session._streaming_callback
    assert callback is not None
    result = callback(types.SimpleNamespace(item_queue=item_queue), None)

    assert result == 0
    assert queued_items == []
    assert released_items == native_items
    session._close()


def test_set_options_reentrant_close_during_conversion_does_not_deadlock(
    fake_native: types.SimpleNamespace,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    session_releases: list[object] = []
    kvp_releases: list[object] = []
    native_calls: list[object] = []
    errors: list[BaseException] = []
    session = _make_session(release=session_releases.append)

    class _ReentrantOptions:
        def to_native_options(self) -> dict[str, str]:
            assert session._active_native_calls == 0
            session._close()
            return {"key": "value"}

    monkeypatch.setattr(fake_native.api.root, "KeyValuePairs_Release", kvp_releases.append)
    monkeypatch.setattr(fake_native.inference, "Session_SetOptions", lambda *_: native_calls.append(object()))

    def set_options() -> None:
        try:
            session.set_options(cast(Any, _ReentrantOptions()))
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=set_options, daemon=True)
    thread.start()
    _join(thread)

    assert len(errors) == 1
    assert isinstance(errors[0], FoundryLocalException)
    assert "closed" in str(errors[0])
    assert native_calls == []
    assert len(kvp_releases) == 1
    assert len(session_releases) == 1


def test_set_options_marshalling_failure_occurs_before_lease_and_releases_kvp(
    fake_native: types.SimpleNamespace,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    kvp_releases: list[object] = []
    session = _make_session()

    def fail_add(*_: object) -> None:
        raise RuntimeError("marshalling failed")

    def fail_acquire() -> object:
        raise AssertionError("lease must not be acquired")

    monkeypatch.setattr(fake_native.api.root, "AddKeyValuePair", fail_add)
    monkeypatch.setattr(fake_native.api.root, "KeyValuePairs_Release", kvp_releases.append)
    monkeypatch.setattr(session, "_acquire_call", fail_acquire)

    with pytest.raises(RuntimeError, match="marshalling failed"):
        session.set_options(cast(Any, _FakeOptions()))

    assert len(kvp_releases) == 1
    assert session._active_native_calls == 0


def test_set_options_native_failure_releases_call_lease_and_kvp(
    fake_native: types.SimpleNamespace,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    kvp_releases: list[object] = []
    session = _make_session()

    def fail_set_options(*_: object) -> None:
        assert session._active_native_calls == 1
        raise RuntimeError("native call failed")

    monkeypatch.setattr(fake_native.inference, "Session_SetOptions", fail_set_options)
    monkeypatch.setattr(fake_native.api.root, "KeyValuePairs_Release", kvp_releases.append)

    with pytest.raises(RuntimeError, match="native call failed"):
        session.set_options(cast(Any, _FakeOptions()))

    assert session._active_native_calls == 0
    assert len(kvp_releases) == 1


def test_process_request_timeout_failure_releases_processing_lease(
    fake_native: types.SimpleNamespace,
) -> None:
    native_calls: list[object] = []
    session = _make_session()

    class _FailingTimeoutRequest(_FakeRequest):
        def set_timeout(self, timeout: float | None) -> None:
            assert session._active_native_calls == 1
            assert session._active_nonstream_requests == 1
            raise RuntimeError("timeout conversion failed")

    fake_native.inference.Session_ProcessRequest = lambda *_: native_calls.append(object())

    with pytest.raises(RuntimeError, match="timeout conversion failed"):
        session.process_request(cast(Request, _FailingTimeoutRequest()), timeout=1)

    _assert_processing_idle(session)
    assert native_calls == []


def test_active_nonstreaming_request_blocks_streaming_and_allows_another_nonstreaming_request(
    fake_native: types.SimpleNamespace,
) -> None:
    first_process_started = threading.Event()
    both_processes_started = threading.Event()
    allow_process_to_finish = threading.Event()
    count_lock = threading.Lock()
    started = 0
    errors: list[BaseException] = []
    responses: list[Response] = []

    def process(_session_ptr: object, _request_ptr: object, out: list[object | None]) -> None:
        nonlocal started
        with count_lock:
            started += 1
            if started == 1:
                first_process_started.set()
            elif started == 2:
                both_processes_started.set()
        assert allow_process_to_finish.wait(timeout=2)
        out[0] = object()

    fake_native.inference.Session_ProcessRequest = process
    session = _make_session()
    session._streaming_enabled = True
    active_request = cast(Request, _FakeRequest())
    rejected_request = cast(Request, _FakeRequest())
    second_request = cast(Request, _FakeRequest())

    def run_request(request: Request) -> None:
        try:
            responses.append(session.process_request(request))
        except BaseException as exc:
            errors.append(exc)

    request_threads = [
        threading.Thread(target=run_request, args=(active_request,)),
        threading.Thread(target=run_request, args=(second_request,)),
    ]
    request_threads[0].start()
    assert first_process_started.wait(timeout=2)

    with pytest.raises(FoundryLocalException, match="cannot overlap"):
        session.process_streaming_request(rejected_request)

    assert rejected_request._active_processes == 0
    assert session._active_nonstream_requests == 1
    assert not session._streaming_request_active

    request_threads[1].start()
    assert both_processes_started.wait(timeout=2)
    assert session._active_nonstream_requests == 2

    allow_process_to_finish.set()
    for thread in request_threads:
        _join(thread)
    assert errors == []
    assert len(responses) == 2
    _assert_processing_idle(session)
    for response in responses:
        response._close()
    session._close()


@pytest.mark.parametrize("rejected_mode", ["nonstreaming", "streaming"])
def test_streaming_request_rejects_other_processing_requests_while_native_work_is_active(
    fake_native: types.SimpleNamespace,
    rejected_mode: str,
) -> None:
    process_started = threading.Event()
    allow_process_to_finish = threading.Event()

    def process(_session_ptr: object, _request_ptr: object, out: list[object | None]) -> None:
        process_started.set()
        assert allow_process_to_finish.wait(timeout=2)
        out[0] = object()

    fake_native.inference.Session_ProcessRequest = process
    session = _make_session()
    session._streaming_enabled = True
    stream_request = cast(Request, _FakeRequest())
    rejected_request = cast(Request, _FakeRequest())

    stream = session.process_streaming_request(stream_request)
    assert process_started.wait(timeout=2)

    with pytest.raises(FoundryLocalException, match="cannot overlap"):
        if rejected_mode == "streaming":
            session.process_streaming_request(rejected_request)
        else:
            session.process_request(rejected_request)

    assert rejected_request._active_processes == 0
    assert session._active_nonstream_requests == 0
    assert session._streaming_request_active

    allow_process_to_finish.set()
    assert list(stream) == []
    stream.final_response._close()
    _assert_processing_idle(session)
    session._close()


@pytest.mark.parametrize("streaming", [False, True], ids=["nonstreaming", "streaming"])
def test_processing_mode_releases_after_native_error(
    fake_native: types.SimpleNamespace,
    streaming: bool,
) -> None:
    session = _make_session()
    request = cast(Request, _FakeRequest())
    error_status = object()

    def fail_process(
        _session_ptr: object,
        _request_ptr: object,
        out: list[object | None],
    ) -> object:
        out[0] = object()
        return error_status

    def check_status(status: object) -> None:
        if status is error_status:
            raise RuntimeError("native process failed")

    fake_native.inference.Session_ProcessRequest = fail_process
    fake_native.api.check_status = check_status

    with pytest.raises(RuntimeError, match="native process failed"):
        if streaming:
            session._streaming_enabled = True
            list(session.process_streaming_request(request))
        else:
            session.process_request(request)

    _assert_processing_idle(session)
    assert request._active_processes == 0
    session._close()


def test_streaming_validation_failure_releases_processing_mode(
    fake_native: types.SimpleNamespace,
) -> None:
    session = _make_session()
    request = cast(Request, _FakeRequest())

    with pytest.raises(FoundryLocalException, match="Streaming not enabled"):
        session.process_streaming_request(request)

    _assert_processing_idle(session)
    assert request._active_processes == 0
    session._close()


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

    _assert_failed_stream_setup_is_clean(session, request)
    session._close()


def _assert_failed_stream_setup_is_clean(session: Session, request: Request) -> None:
    _assert_processing_idle(session)
    assert session._stream_thread is None
    assert session._stream_request is None
    assert session._stream_queue is None
    assert request._active_processes == 0


def test_streaming_thread_setup_failure_releases_request_lease(
    fake_native: types.SimpleNamespace,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    session = _make_session()
    session._streaming_enabled = True
    request = cast(Request, _FakeRequest())

    def fail_thread(*_args: object, **_kwargs: object) -> threading.Thread:
        raise RuntimeError("thread setup failed")

    monkeypatch.setattr(threading, "Thread", fail_thread)

    with pytest.raises(RuntimeError, match="thread setup failed"):
        session.process_streaming_request(request)

    _assert_failed_stream_setup_is_clean(session, request)
    session._close()


@pytest.mark.parametrize("streaming", [False, True], ids=["synchronous", "streaming"])
def test_request_close_waits_for_processing_before_release(
    fake_native: types.SimpleNamespace,
    streaming: bool,
) -> None:
    process_started = threading.Event()
    allow_process_to_finish = threading.Event()
    cancel_called = threading.Event()
    releases: list[object] = []
    errors: list[BaseException] = []

    def process(_session_ptr: object, _request_ptr: object, out: list[object | None]) -> None:
        process_started.set()
        assert allow_process_to_finish.wait(timeout=2)
        out[0] = object()

    fake_native.inference.Session_ProcessRequest = process
    fake_native.inference.Request_Cancel = lambda _ptr: cancel_called.set()
    session = _make_session()
    request = _make_request(release=releases.append)

    if streaming:
        session._streaming_enabled = True
        result: StreamingResponse | list[Response] = session.process_streaming_request(request)
        request_thread = None
    else:
        responses: list[Response] = []
        result = responses

        def run_request() -> None:
            try:
                responses.append(session.process_request(request))
            except BaseException as exc:
                errors.append(exc)

        request_thread = threading.Thread(target=run_request)
        request_thread.start()

    assert process_started.wait(timeout=2)
    close_thread = threading.Thread(target=request._close)
    close_thread.start()
    assert cancel_called.wait(timeout=2)
    assert releases == []
    assert close_thread.is_alive()

    allow_process_to_finish.set()
    if request_thread is not None:
        _join(request_thread)
    else:
        assert list(cast(StreamingResponse, result)) == []
        cast(StreamingResponse, result).final_response._close()
    _join(close_thread)

    assert errors == []
    assert len(releases) == 1
    assert request._active_processes == 0
    assert request._closed
    if not streaming:
        assert len(cast(list[Response], result)) == 1
        cast(list[Response], result)[0]._close()
    session._close()


def test_request_concurrent_and_repeated_close_releases_exactly_once(
    fake_native: types.SimpleNamespace,
) -> None:
    callers = 4
    start = threading.Barrier(callers + 1)
    release_started = threading.Event()
    allow_release_to_finish = threading.Event()
    releases: list[object] = []

    def release(ptr: object) -> None:
        releases.append(ptr)
        release_started.set()
        assert allow_release_to_finish.wait(timeout=2)

    request = _make_request(release=release)

    def close_request() -> None:
        start.wait()
        request._close()

    threads = [threading.Thread(target=close_request) for _ in range(callers)]
    for thread in threads:
        thread.start()
    start.wait()

    assert release_started.wait(timeout=2)
    assert len(releases) == 1
    allow_release_to_finish.set()
    for thread in threads:
        _join(thread)

    request._close()
    assert len(releases) == 1


@pytest.mark.parametrize("streaming", [False, True], ids=["synchronous", "streaming"])
def test_processing_rejects_request_after_close(
    fake_native: types.SimpleNamespace,
    streaming: bool,
) -> None:
    request = _make_request()
    request._close()
    session = _make_session()
    session._streaming_enabled = streaming

    with pytest.raises(FoundryLocalException, match="closed"):
        if streaming:
            session.process_streaming_request(request)
        else:
            session.process_request(request)

    _assert_processing_idle(session)
    assert request._active_processes == 0
    assert session._stream_queue is None
    session._close()


def test_request_lease_allows_sequential_reuse(
    fake_native: types.SimpleNamespace,
) -> None:
    request = _make_request()
    session = _make_session()
    processed: list[object] = []

    def process(_session_ptr: object, request_ptr: object, out: list[object | None]) -> None:
        processed.append(request_ptr)
        out[0] = object()

    fake_native.inference.Session_ProcessRequest = process

    for _ in range(2):
        session.process_request(request)._close()

    assert processed == [request._ptr, request._ptr]
    assert request._active_processes == 0
    request._close()
    session._close()


def test_idle_request_cancel_does_not_retain_processing_lease(
    fake_native: types.SimpleNamespace,
) -> None:
    request = _make_request()
    cancellations: list[object] = []
    fake_native.inference.Request_Cancel = cancellations.append

    request.cancel()

    assert cancellations == [request._ptr]
    assert request._active_processes == 0
    request._close()


def test_closing_unstarted_stream_retries_cancellation_until_worker_settles(
    fake_native: types.SimpleNamespace,
) -> None:
    native_entry_allowed = threading.Event()
    native_entered = threading.Event()
    cancellation_attempted = threading.Event()
    cancellation_observed = threading.Event()
    cancellation_after_native_return = threading.Event()
    native_call_returned = threading.Event()
    cancellation_calls = 0

    class _PreEntryRequest(_FakeRequest):
        def cancel(self) -> None:
            nonlocal cancellation_calls
            cancellation_calls += 1
            cancellation_attempted.set()
            if native_entered.is_set():
                cancellation_observed.set()
            if native_call_returned.is_set():
                cancellation_after_native_return.set()

    def process(_session_ptr: object, _request_ptr: object, out: list[object | None]) -> None:
        assert native_entry_allowed.wait(timeout=2)
        native_entered.set()
        assert cancellation_observed.wait(timeout=2)
        out[0] = object()
        native_call_returned.set()

    fake_native.inference.Session_ProcessRequest = process
    session = _make_session()
    session._streaming_enabled = True
    request = cast(Request, _PreEntryRequest())
    stream = session.process_streaming_request(request)

    close_thread = threading.Thread(target=lambda: stream.__exit__(None, None, None))
    close_thread.start()
    assert cancellation_attempted.wait(timeout=2)
    assert not native_entered.is_set()
    assert not cancellation_observed.is_set()

    native_entry_allowed.set()
    _join(close_thread)

    assert cancellation_observed.is_set()
    assert cancellation_calls >= 2
    assert native_call_returned.is_set()
    assert not cancellation_after_native_return.is_set()
    _assert_processing_idle(session)
    assert request._active_processes == 0
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
    request._dispose_requested = False
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
    request._dispose_requested = False
    request._ptr = object()

    request.set_timeout(timeout)

    request._closed = True
    assert calls == [expected_ms]
