from __future__ import annotations

import sys
from threading import Event, Thread
from types import SimpleNamespace

import pytest

from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.imodel import _ModelImpl


class FakeFfi:
    NULL = None

    @staticmethod
    def new_handle(value):
        return value

    @staticmethod
    def from_handle(value):
        return value

    @staticmethod
    def callback(_signature):
        return lambda fn: fn


def make_model(monkeypatch, invoke_callback):
    def download(_ptr, callback, user_data):
        return invoke_callback(callback, user_data)

    def check_status(status):
        if status is not None:
            raise FoundryLocalException("download cancelled", error_code=5)

    fake_api = SimpleNamespace(model=SimpleNamespace(Download=download), check_status=check_status)
    monkeypatch.setitem(
        sys.modules,
        "foundry_local_sdk._native.api",
        SimpleNamespace(api=fake_api, ffi=FakeFfi()),
    )
    model = _ModelImpl.__new__(_ModelImpl)
    model._ptr = object()
    return model


def test_download_cancel_event_returns_nonzero(monkeypatch):
    cancel_event = Event()
    cancel_event.set()

    def invoke(callback, user_data):
        assert callback(25.0, user_data) == 1
        return object()

    model = make_model(monkeypatch, invoke)

    with pytest.raises(FoundryLocalException, match="download cancelled") as exc:
        model.download(cancel_event=cancel_event)

    assert exc.value.error_code == 5


def test_download_cancel_event_is_checked_after_progress(monkeypatch):
    cancel_event = Event()
    progress: list[float] = []

    def on_progress(value: float) -> None:
        progress.append(value)
        cancel_event.set()

    def invoke(callback, user_data):
        assert callback(50.0, user_data) == 1
        return object()

    model = make_model(monkeypatch, invoke)

    with pytest.raises(FoundryLocalException, match="download cancelled"):
        model.download(on_progress, cancel_event)

    assert progress == [50.0]


def test_download_preserves_progress_callback_exception(monkeypatch):
    expected = RuntimeError("progress failed")

    def on_progress(_value: float) -> None:
        raise expected

    def invoke(callback, user_data):
        assert callback(10.0, user_data) == 1
        return object()

    model = make_model(monkeypatch, invoke)

    with pytest.raises(RuntimeError, match="progress failed") as exc:
        model.download(on_progress)

    assert exc.value is expected


def test_download_preserves_first_exception_across_concurrent_callbacks(monkeypatch):
    expected = RuntimeError("first progress failed")
    first_entered = Event()
    second_native_started = Event()
    second_user_entered = Event()
    progress: list[float] = []

    def on_progress(value: float) -> None:
        progress.append(value)
        if value == 10.0:
            first_entered.set()
            second_native_started.wait(timeout=1)
            second_user_entered.wait(timeout=0.1)
            raise expected
        second_user_entered.set()

    def invoke(callback, user_data):
        results: dict[str, int] = {}

        first = Thread(target=lambda: results.setdefault("first", callback(10.0, user_data)))
        first.start()
        assert first_entered.wait(timeout=1)

        def call_second() -> None:
            second_native_started.set()
            results["second"] = callback(20.0, user_data)

        second = Thread(target=call_second)
        second.start()
        first.join(timeout=2)
        second.join(timeout=2)

        assert not first.is_alive()
        assert not second.is_alive()
        assert results == {"first": 1, "second": 1}
        return object()

    model = make_model(monkeypatch, invoke)

    with pytest.raises(RuntimeError, match="first progress failed") as exc:
        model.download(on_progress)

    assert exc.value is expected
    assert progress == [10.0]
