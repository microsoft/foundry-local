# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import threading
from collections.abc import Iterator
from contextlib import contextmanager
from typing import TYPE_CHECKING

from foundry_local_sdk.exception import FoundryLocalException

if TYPE_CHECKING:
    from foundry_local_sdk.items import Item
    from foundry_local_sdk.session_types import RequestOptions

_API_VERSION = 1  # FOUNDRY_LOCAL_API_VERSION


@contextmanager
def _cancellable_lock(lock: threading.RLock, cancel_event: threading.Event) -> Iterator[None]:
    while True:
        if cancel_event.is_set():
            raise FoundryLocalException("Request canceled before native processing.", error_code=5)
        if lock.acquire(timeout=0.01):
            break
    try:
        if cancel_event.is_set():
            raise FoundryLocalException("Request canceled before native processing.", error_code=5)
        yield
    finally:
        lock.release()


class _CancellationState:
    def __init__(self, request: Request, event: threading.Event) -> None:
        self.request = request
        self.event = event
        self.lock = threading.Lock()
        self.finished = False
        self.error: Exception | None = None

    def retry(self) -> None:
        # Unset events cannot touch native state, so they need not contend with request cleanup.
        try:
            requested = self.event.is_set()
        except Exception as exc:
            with self.lock:
                self.error = exc
            return
        if not requested:
            return
        with self.lock:
            if self.finished or self.error is not None:
                return
            try:
                self.request.cancel()
            except Exception as exc:
                self.error = exc


class _CancellationDispatcher:
    def __init__(self) -> None:
        self._condition = threading.Condition()
        self._pending: set[_CancellationState] = set()
        self._thread: threading.Thread | None = None

    def register(self, state: _CancellationState) -> None:
        with self._condition:
            self._pending.add(state)
            if self._thread is None:
                worker = threading.Thread(target=self._run, name="FoundryLocalCancellation", daemon=True)
                try:
                    worker.start()
                except BaseException:
                    self._pending.remove(state)
                    raise
                self._thread = worker
            self._condition.notify()

    def unregister(self, state: _CancellationState) -> None:
        # Join an active cancellation attempt before the enclosing native lifetime lease is released.
        with state.lock:
            state.finished = True
        with self._condition:
            self._pending.remove(state)
            self._condition.notify()
        if state.error is not None:
            raise state.error

    def _run(self) -> None:
        while True:
            with self._condition:
                while not self._pending:
                    self._condition.wait()
                self._condition.wait_for(lambda: not self._pending, timeout=0.01)
                if not self._pending:
                    continue
                pending = tuple(self._pending)
            for state in pending:
                state.retry()
            del state, pending


_cancellation_dispatcher = _CancellationDispatcher()


class Request:
    """Inference request. Owns its native flRequest* handle.

    Supports fluent chaining — add_item() and set_options() return self.

    Items added via add_item() transfer native ownership to the request.
    Do not use the item after adding it.
    """

    def __init__(self) -> None:
        self._lifetime_lock = threading.RLock()
        # Initialise lifecycle flags FIRST so that if Request_Create raises,
        # __del__ sees a fully-constructed (but already-closed) object and
        # cleanly no-ops instead of AttributeError'ing inside the GC.
        self._closed = True
        self._ptr = None
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flRequest**")
        api.check_status(api.inference.Request_Create(out))
        self._ptr = out[0]
        self._closed = False

    def _check_open(self) -> None:
        if self._closed:
            raise FoundryLocalException(
                f"{type(self).__name__} has been closed and can no longer be used."
            )

    @contextmanager
    def _native_lifetime(self, cancel_event: threading.Event | None = None) -> Iterator[object]:
        with self._lifetime_lock if cancel_event is None else _cancellable_lock(self._lifetime_lock, cancel_event):
            self._check_open()
            yield self._ptr

    @contextmanager
    def _cancellation(self, cancel_event: threading.Event | None) -> Iterator[None]:
        if cancel_event is None:
            yield
            return
        if cancel_event.is_set():
            raise FoundryLocalException("Request canceled before native processing.", error_code=5)

        state = _CancellationState(self, cancel_event)
        _cancellation_dispatcher.register(state)
        try:
            yield
        finally:
            _cancellation_dispatcher.unregister(state)

    def add_item(self, item: "Item", transfer_ownership: bool = True) -> "Request":
        """Add item to request.

        Args:
            item: The item to add. Any ``Item`` subclass, including ``ItemQueue`` (which is itself an ``Item``
                — see C++ ``struct ItemQueue : Item``).
            transfer_ownership: When True (default), the item's native handle is transferred to the request and
                the Python wrapper becomes inert. Set False when the caller needs to keep using the item —
                typically an ``ItemQueue`` you continue pushing into for live-streaming sessions.
        """
        self._check_open()
        from foundry_local_sdk._native.api import api

        # Pass the raw native pointer first; ``_release_ownership`` only flips the Python-side ``_owns`` flag
        # (it does NOT zero ``_ptr``), so calling it after the native call succeeds is safe and ensures we
        # don't leak the native handle if ``check_status`` raises.
        api.check_status(
            api.inference.Request_AddItem(self._ptr, item._ptr, transfer_ownership)
        )
        if transfer_ownership:
            item._release_ownership()
        return self

    @property
    def item_count(self) -> int:
        self._check_open()
        from foundry_local_sdk._native.api import api

        return int(api.inference.Request_GetItemCount(self._ptr))

    def get_item(self, index: int) -> "Item":
        self._check_open()
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api
        from foundry_local_sdk.items import Item

        out = ffi.new("flItem**")
        api.check_status(api.inference.Request_GetItem(self._ptr, index, out))
        return Item.from_native(out[0], owns=False)

    def set_options(self, options: "RequestOptions") -> "Request":
        """Set per-request inference options. Overrides session-level options for this request."""
        self._check_open()
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        native_options = options.to_native_options()
        kvp_out = ffi.new("flKeyValuePairs**")
        api.root.CreateKeyValuePairs(kvp_out)
        kvp = kvp_out[0]
        try:
            for key, value in native_options.items():
                api.root.AddKeyValuePair(kvp, key.encode("utf-8"), value.encode("utf-8"))
            api.check_status(api.inference.Request_SetOptions(self._ptr, kvp))
        finally:
            api.root.KeyValuePairs_Release(kvp)
        return self

    def cancel(self) -> None:
        """Cancel this request.

        Only an invocation currently being processed is affected. Calling this
        while the request is idle or after completion has no effect.
        """
        self._check_open()
        from foundry_local_sdk._native.api import api

        api.check_status(api.inference.Request_Cancel(self._ptr))

    def _close(self) -> None:
        with self._lifetime_lock:
            if self._closed:
                return
            if self._ptr is not None:
                try:
                    from foundry_local_sdk._native.api import api

                    api.inference.Request_Release(self._ptr)
                except Exception:
                    pass
            self._ptr = None
            self._closed = True

    def __enter__(self) -> "Request":
        return self

    def __exit__(self, *_) -> None:
        self._close()

    def __del__(self) -> None:
        self._close()
