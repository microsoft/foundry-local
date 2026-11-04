# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Deterministic unit tests for native builder, session, and manager lifetimes."""
from __future__ import annotations

import importlib
import threading
import weakref
from concurrent.futures import ThreadPoolExecutor
from types import SimpleNamespace
from typing import Self

import pytest

from foundry_local_sdk import CatalogType, ModelInfoBuilder
from foundry_local_sdk._native import ffi
from foundry_local_sdk.catalog import Catalog
from foundry_local_sdk.foundry_local_manager import FoundryLocalManager
from foundry_local_sdk.imodel import _ModelImpl
from foundry_local_sdk.session import Session


def _patch_api(monkeypatch: pytest.MonkeyPatch, fake_api: object) -> None:
    native_api_module = importlib.import_module("foundry_local_sdk._native.api")
    monkeypatch.setattr(native_api_module, "api", fake_api)


def _builder_api(*, register_model=None, release_model_info=None):
    metadata_ptr = ffi.cast("flModelInfo *", 1)

    def create_model_info(out):
        out[0] = metadata_ptr
        return ffi.NULL

    model = SimpleNamespace(
        CreateModelInfo=create_model_info,
        ReleaseModelInfo=release_model_info or (lambda _ptr: None),
    )
    catalog = SimpleNamespace(RegisterModel=register_model) if register_model is not None else None
    return SimpleNamespace(model=model, catalog=catalog, check_status=lambda status: assert_null(status))


def assert_null(status: object) -> None:
    assert status == ffi.NULL


def _new_manager() -> FoundryLocalManager:
    manager = FoundryLocalManager.__new__(FoundryLocalManager)
    manager._native_manager = object()
    manager._catalogs = {}
    manager._native_call_state = threading.local()
    manager._lifetime_changed = threading.Condition(FoundryLocalManager._lock)
    manager._active_native_calls = 0
    manager._sessions = weakref.WeakSet()
    manager._close_started = threading.Event()
    manager._close_started_lock = threading.Lock()
    return manager


def test_model_info_builder_concurrent_close_releases_once(monkeypatch: pytest.MonkeyPatch) -> None:
    releases: list[object] = []
    start = threading.Barrier(3)
    _patch_api(monkeypatch, _builder_api(release_model_info=releases.append))
    builder = ModelInfoBuilder()

    def close_after_barrier() -> None:
        start.wait()
        builder.close()

    with ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(close_after_barrier) for _ in range(2)]
        start.wait()
        for future in futures:
            future.result(timeout=5)

    assert len(releases) == 1


def test_model_info_builder_close_waits_for_register_model(monkeypatch: pytest.MonkeyPatch) -> None:
    register_entered = threading.Event()
    finish_register = threading.Event()
    close_started = threading.Event()
    close_finished = threading.Event()
    calls: list[str] = []

    def register_model(_catalog, _path, _model_id, _metadata, out):
        calls.append("register-enter")
        register_entered.set()
        assert finish_register.wait(timeout=5)
        out[0] = ffi.cast("flModel *", 2)
        calls.append("register-exit")
        return ffi.NULL

    def release_model_info(_ptr):
        calls.append("metadata-release")

    _patch_api(
        monkeypatch,
        _builder_api(register_model=register_model, release_model_info=release_model_info),
    )
    builder = ModelInfoBuilder()
    catalog = Catalog.__new__(Catalog)
    catalog._ptr = ffi.cast("flCatalog *", 3)
    catalog._catalog_type = CatalogType.LOCAL
    catalog._parent = None

    def close_builder() -> None:
        close_started.set()
        builder.close()
        close_finished.set()

    with ThreadPoolExecutor(max_workers=2) as executor:
        register_future = executor.submit(catalog.register_model, "model", "example:1", builder)
        assert register_entered.wait(timeout=5)
        close_future = executor.submit(close_builder)
        assert close_started.wait(timeout=5)
        assert not close_finished.is_set()
        finish_register.set()
        assert register_future.result(timeout=5) is not None
        close_future.result(timeout=5)

    assert calls == ["register-enter", "register-exit", "metadata-release"]


def test_manager_close_releases_live_session_before_manager(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[str] = []
    fake_api = SimpleNamespace(
        root=SimpleNamespace(
            Manager_Shutdown=lambda _manager: calls.append("shutdown") or ffi.NULL,
            Manager_Release=lambda _manager: calls.append("manager-release"),
        ),
        inference=SimpleNamespace(
            Session_Release=lambda _session: calls.append("session-release"),
        ),
        check_status=lambda status: assert_null(status),
    )
    _patch_api(monkeypatch, fake_api)

    manager = _new_manager()

    session = Session.__new__(Session)
    session._closed = False
    session._ptr = object()
    session._manager = manager
    session._operation_lock = threading.RLock()
    session._native_call_state = threading.local()
    session._stream_thread = None
    session._stream_request = None
    manager._register_session(session)

    manager.close()

    assert calls == ["session-release", "shutdown", "manager-release"]
    assert session._closed is True
    assert session._manager is None
    assert manager._native_manager is None


def test_manager_close_from_active_native_call_is_rejected(monkeypatch: pytest.MonkeyPatch) -> None:
    _patch_api(monkeypatch, SimpleNamespace())
    manager = _new_manager()

    with manager._native_call(), pytest.raises(RuntimeError, match="active native call"):
        manager.close()


def test_manager_native_calls_overlap_and_close_waits_for_all(monkeypatch: pytest.MonkeyPatch) -> None:
    both_admitted = threading.Barrier(3)
    release_calls = [threading.Event(), threading.Event()]
    shutdown_called = threading.Event()
    manager_released = threading.Event()
    fake_api = SimpleNamespace(
        root=SimpleNamespace(
            Manager_Shutdown=lambda _manager: shutdown_called.set() or ffi.NULL,
            Manager_Release=lambda _manager: manager_released.set(),
        ),
        check_status=lambda status: assert_null(status),
    )
    _patch_api(monkeypatch, fake_api)
    manager = _new_manager()

    def active_call(index: int) -> None:
        with manager._native_call():
            both_admitted.wait(timeout=5)
            assert release_calls[index].wait(timeout=5)

    with ThreadPoolExecutor(max_workers=3) as executor:
        calls = [executor.submit(active_call, index) for index in range(2)]
        both_admitted.wait(timeout=5)
        close_future = executor.submit(manager.close)
        assert manager._close_started.wait(timeout=5)
        assert not shutdown_called.is_set()
        assert not manager_released.is_set()
        release_calls[0].set()
        calls[0].result(timeout=5)
        assert not shutdown_called.is_set()
        assert not manager_released.is_set()
        release_calls[1].set()
        calls[1].result(timeout=5)
        close_future.result(timeout=5)

    assert shutdown_called.is_set()
    assert manager_released.is_set()


def test_model_call_blocks_close_and_borrowed_wrappers_reject_afterward(monkeypatch: pytest.MonkeyPatch) -> None:
    model_call_entered = threading.Event()
    finish_model_call = threading.Event()
    shutdown_called = threading.Event()

    def is_cached(_model, out):
        model_call_entered.set()
        assert finish_model_call.wait(timeout=5)
        out[0] = 1
        return ffi.NULL

    fake_api = SimpleNamespace(
        root=SimpleNamespace(
            Manager_Shutdown=lambda _manager: shutdown_called.set() or ffi.NULL,
            Manager_Release=lambda _manager: None,
        ),
        model=SimpleNamespace(IsCached=is_cached),
        catalog=SimpleNamespace(GetModels=lambda *_args: pytest.fail("catalog dispatched after manager close")),
        check_status=lambda status: assert_null(status),
    )
    _patch_api(monkeypatch, fake_api)
    manager = _new_manager()
    catalog = Catalog.__new__(Catalog)
    catalog._ptr = ffi.cast("flCatalog *", 2)
    catalog._parent = manager
    model = _ModelImpl(ffi.cast("flModel *", 1), parent=catalog)

    with ThreadPoolExecutor(max_workers=2) as executor:
        model_future = executor.submit(lambda: model.is_cached)
        assert model_call_entered.wait(timeout=5)
        close_future = executor.submit(manager.close)
        assert manager._close_started.wait(timeout=5)
        assert not shutdown_called.is_set()
        finish_model_call.set()
        assert model_future.result(timeout=5) is True
        close_future.result(timeout=5)

    with pytest.raises(RuntimeError, match="closed"):
        _ = model.is_cached
    with pytest.raises(RuntimeError, match="closed"):
        catalog.list_models()


def test_manager_close_waits_for_blocked_session_process_request_before_releasing_session(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    process_request_entered = threading.Barrier(2)
    finish_process_request = threading.Event()
    session_released = threading.Event()
    close_finished = threading.Event()
    releases: list[object] = []
    session_ptr = ffi.cast("flSession *", 1)
    request_ptr = ffi.cast("flRequest *", 2)
    response_ptr = ffi.cast("flResponse *", 3)

    def process_request(_session, _request, out):
        process_request_entered.wait(timeout=5)
        assert finish_process_request.wait(timeout=5)
        out[0] = response_ptr
        return ffi.NULL

    def release_session(ptr):
        releases.append(ptr)
        session_released.set()

    fake_api = SimpleNamespace(
        root=SimpleNamespace(
            Manager_Shutdown=lambda _manager: ffi.NULL,
            Manager_Release=lambda _manager: None,
        ),
        inference=SimpleNamespace(
            Session_ProcessRequest=process_request,
            Session_Release=release_session,
            Response_Release=lambda _response: None,
        ),
        check_status=lambda status: assert_null(status),
    )
    _patch_api(monkeypatch, fake_api)
    manager = _new_manager()
    session = Session.__new__(Session)
    session._closed = False
    session._ptr = session_ptr
    session._manager = manager
    session._operation_lock = threading.RLock()
    session._native_call_state = threading.local()
    session._stream_thread = None
    session._stream_request = None
    manager._register_session(session)
    request = SimpleNamespace(_ptr=request_ptr)

    def close_manager() -> None:
        manager.close()
        close_finished.set()

    with ThreadPoolExecutor(max_workers=2) as executor:
        process_future = executor.submit(session.process_request, request)
        process_request_entered.wait(timeout=5)
        close_future = executor.submit(close_manager)
        assert manager._close_started.wait(timeout=5)
        assert not session_released.is_set()
        assert not close_finished.is_set()
        finish_process_request.set()
        response = process_future.result(timeout=5)
        close_future.result(timeout=5)

    response._close()
    assert releases == [session_ptr]
    assert session_released.is_set()
    assert close_finished.is_set()


def test_session_construction_racing_manager_close_rolls_back_without_hanging(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[str] = []
    registration_entered = threading.Event()
    finish_registration = threading.Event()
    session_ptr = ffi.cast("flSession *", 2)

    def create_session(_model, out):
        out[0] = session_ptr
        return ffi.NULL

    fake_api = SimpleNamespace(
        root=SimpleNamespace(
            Manager_Shutdown=lambda _manager: calls.append("shutdown") or ffi.NULL,
            Manager_Release=lambda _manager: calls.append("manager-release"),
        ),
        inference=SimpleNamespace(
            Session_Create=create_session,
            Session_Release=lambda _session: calls.append("session-release"),
        ),
        check_status=lambda status: assert_null(status),
    )
    _patch_api(monkeypatch, fake_api)
    manager = _new_manager()
    model = _ModelImpl(ffi.cast("flModel *", 1), parent=SimpleNamespace(_parent=manager))
    register_session = manager._register_session

    def pause_registration(session: object) -> None:
        assert session._ptr == session_ptr
        assert session._manager is manager
        assert session._closed is False
        registration_entered.set()
        assert finish_registration.wait(timeout=5)
        register_session(session)

    manager._register_session = pause_registration

    with ThreadPoolExecutor(max_workers=2) as executor:
        construction_future = executor.submit(Session, model)
        assert registration_entered.wait(timeout=5)
        close_future = executor.submit(manager.close)
        assert manager._close_started.wait(timeout=5)
        finish_registration.set()
        with pytest.raises(RuntimeError, match="closed"):
            construction_future.result(timeout=5)
        close_future.result(timeout=5)

    assert calls == ["session-release", "shutdown", "manager-release"]


def test_failed_session_registration_and_finalizer_release_exactly_once(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    releases: list[object] = []
    session_ptr = ffi.cast("flSession *", 2)

    def create_session(_model, out):
        out[0] = session_ptr
        return ffi.NULL

    fake_api = SimpleNamespace(
        inference=SimpleNamespace(
            Session_Create=create_session,
            Session_Release=releases.append,
        ),
        check_status=lambda status: assert_null(status),
    )
    _patch_api(monkeypatch, fake_api)
    manager = _new_manager()
    manager._register_session = lambda _session: (_ for _ in ()).throw(RuntimeError("registration failed"))
    model = _ModelImpl(ffi.cast("flModel *", 1), parent=SimpleNamespace(_parent=manager))

    class CapturingSession(Session):
        instance: CapturingSession | None = None

        def __new__(cls, *_args: object, **_kwargs: object) -> Self:
            instance = super().__new__(cls)
            cls.instance = instance
            return instance

    with pytest.raises(RuntimeError, match="registration failed"):
        CapturingSession(model)

    assert CapturingSession.instance is not None
    CapturingSession.instance.__del__()
    assert releases == [session_ptr]
    manager._native_manager = None


def test_concurrent_manager_close_releases_once(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[str] = []
    shutdown_entered = threading.Event()
    finish_shutdown = threading.Event()

    def shutdown(_manager):
        calls.append("shutdown")
        shutdown_entered.set()
        assert finish_shutdown.wait(timeout=5)
        return ffi.NULL

    fake_api = SimpleNamespace(
        root=SimpleNamespace(
            Manager_Shutdown=shutdown,
            Manager_Release=lambda _manager: calls.append("manager-release"),
        ),
        check_status=lambda status: assert_null(status),
    )
    _patch_api(monkeypatch, fake_api)
    manager = _new_manager()

    with ThreadPoolExecutor(max_workers=2) as executor:
        first_close = executor.submit(manager.close)
        assert shutdown_entered.wait(timeout=5)
        second_close = executor.submit(manager.close)
        finish_shutdown.set()
        first_close.result(timeout=5)
        second_close.result(timeout=5)

    assert calls == ["shutdown", "manager-release"]


def test_deferred_model_info_reader_rejects_after_manager_close_started(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    manager = _new_manager()
    model = _ModelImpl(object(), parent=SimpleNamespace(_parent=manager))
    captured: dict[str, object] = {}

    def capture_reader(_ptr: object, *, manager_native_call=None):
        captured["manager_native_call"] = manager_native_call
        return object()

    imodel_module = importlib.import_module("foundry_local_sdk.imodel")
    monkeypatch.setattr(imodel_module, "_model_info_from_native", capture_reader)

    _ = model.info
    manager._close_started.set()
    native_call = captured["manager_native_call"]
    with pytest.raises(RuntimeError, match="closed"), native_call():
        pass
    manager._native_manager = None
