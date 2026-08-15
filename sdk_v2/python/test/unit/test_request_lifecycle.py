from __future__ import annotations

import builtins
import sys
import threading
import types
from collections.abc import Callable
from typing import Any

import pytest

from foundry_local_sdk.request import Request


class _FakeFfi:
    def new(self, declaration: str) -> list[object | None]:
        assert declaration == "flRequest**"
        return [None]


class _NeverWaitCondition(threading.Condition):
    def wait(self, timeout: float | None = None) -> bool:
        raise AssertionError("finalizer must not wait")


def _make_request(
    *,
    condition: threading.Condition | None = None,
    release: Callable[[object], object] | None = None,
) -> Request:
    request = object.__new__(Request)
    request._closed = False
    request._ptr = object()
    request._lock = condition or threading.Condition()
    request._dispose_requested = False
    request._active_processes = 0
    request._release_request = release or (lambda _: None)
    return request


def _install_native(
    monkeypatch: pytest.MonkeyPatch,
    *,
    create: Callable[[list[object | None]], object],
    release: Callable[[object], object],
) -> types.SimpleNamespace:
    inference = types.SimpleNamespace(
        Request_Create=create,
        Request_Release=release,
    )
    api = types.SimpleNamespace(inference=inference, check_status=lambda status: None)
    native_module = types.ModuleType("foundry_local_sdk._native")
    setattr(native_module, "ffi", _FakeFfi())
    setattr(native_module, "api", api)
    api_module = types.ModuleType("foundry_local_sdk._native.api")
    setattr(api_module, "api", api)
    monkeypatch.setitem(sys.modules, "foundry_local_sdk._native", native_module)
    monkeypatch.setitem(sys.modules, "foundry_local_sdk._native.api", api_module)
    return inference


def test_request_finalizer_does_not_block_on_contended_lock() -> None:
    releases: list[object] = []
    request = _make_request(release=releases.append)
    finished = threading.Event()

    def finalize() -> None:
        request.__del__()
        finished.set()

    with request._lock:
        thread = threading.Thread(target=finalize)
        thread.start()
        assert finished.wait(timeout=2)

    thread.join(timeout=2)
    assert not thread.is_alive()
    assert releases == []
    assert request._ptr is not None
    assert not request._closed

    request._close()
    assert len(releases) == 1


def test_request_finalizer_with_active_call_never_waits_or_releases() -> None:
    releases: list[object] = []
    request = _make_request(
        condition=_NeverWaitCondition(),
        release=releases.append,
    )
    request._active_processes = 1
    ptr = request._ptr

    request.__del__()

    assert request._ptr is ptr
    assert not request._closed
    assert not request._dispose_requested
    assert releases == []


def test_request_finalizer_releases_idle_request_without_native_access(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    releases: list[object] = []
    ptr = object()

    def create(out: list[object | None]) -> None:
        out[0] = ptr

    inference = _install_native(
        monkeypatch,
        create=create,
        release=releases.append,
    )
    request = Request()
    inference.Request_Release = lambda _: pytest.fail(
        "finalizer must not access the native API"
    )
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
    request.__del__()

    assert releases == [ptr]
    assert request._ptr is None
    assert request._closed


def test_request_finalizer_handles_partial_construction(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    releases: list[object] = []

    def fail_create(_out: list[object | None]) -> None:
        raise RuntimeError("creation failed")

    _install_native(
        monkeypatch,
        create=fail_create,
        release=releases.append,
    )
    request = object.__new__(Request)

    with pytest.raises(RuntimeError, match="creation failed"):
        Request.__init__(request)

    request.__del__()

    assert request._release_request is None
    assert request._ptr is None
    assert request._closed
    assert releases == []


def test_request_finalizer_and_close_release_exactly_once() -> None:
    releases: list[object] = []
    request = _make_request(release=releases.append)
    ptr = request._ptr

    request.__del__()
    request.__del__()
    request._close()

    assert releases == [ptr]
