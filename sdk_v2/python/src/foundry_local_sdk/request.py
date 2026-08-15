# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import math
import threading
from collections.abc import Callable
from typing import TYPE_CHECKING, cast

from foundry_local_sdk.exception import FoundryLocalException

if TYPE_CHECKING:
    from foundry_local_sdk.items import Item
    from foundry_local_sdk.session_types import RequestOptions

_API_VERSION = 1  # FOUNDRY_LOCAL_API_VERSION
_MAX_TIMEOUT_MS = (1 << 63) - 1

# Matches the C# cancel-retry cadence while waiting for processing to drain.
_CANCEL_RETRY_INTERVAL_SECONDS = 0.05


class Request:
    """Inference request. Owns its native flRequest* handle.

    Supports fluent chaining — add_item() and set_options() return self.

    Items added via add_item() transfer native ownership to the request.
    Do not use the item after adding it.
    """

    def __init__(self) -> None:
        # Set cleanup fields before Request_Create can fail.
        self._closed = True
        self._ptr = None
        self._lock = threading.Condition()
        self._dispose_requested = False
        self._active_processes = 0
        # Avoid native API access from __del__ during interpreter shutdown.
        self._release_request: Callable[[object], object] | None = None
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        out = ffi.new("flRequest**")
        api.check_status(api.inference.Request_Create(out))
        self._release_request = cast(
            Callable[[object], object], getattr(api.inference, "Request_Release")
        )
        self._ptr = out[0]
        self._closed = False

    def _check_open(self) -> None:
        if self._dispose_requested or self._closed:
            raise FoundryLocalException(
                f"{type(self).__name__} has been closed and can no longer be used."
            )

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

        # Relinquish Python ownership only after the native call succeeds.
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
        """Signal cancellation for an in-flight request."""
        ptr = self._acquire_for_processing()
        try:
            from foundry_local_sdk._native.api import api

            api.check_status(api.inference.Request_Cancel(ptr))
        finally:
            self._release_after_processing()

    def set_timeout(self, timeout: "float | None") -> "Request":
        """Set the timeout for each execution of this request, in seconds.

        Args:
            timeout: Seconds. ``None`` or a non-positive value disables the timeout.
        """
        self._check_open()
        from foundry_local_sdk._native.api import api

        if timeout is None:
            timeout_ms = 0
        else:
            try:
                is_finite = math.isfinite(timeout)
            except OverflowError as exc:
                raise ValueError("timeout is too large") from exc
            if not is_finite:
                raise ValueError("timeout must be finite")
            if timeout <= 0:
                timeout_ms = 0
            else:
                milliseconds = timeout * 1000
                if not math.isfinite(milliseconds) or milliseconds > _MAX_TIMEOUT_MS:
                    raise ValueError("timeout is too large")
                timeout_ms = math.ceil(milliseconds)
        api.check_status(api.inference.Request_SetTimeoutMs(self._ptr, timeout_ms))
        return self

    def _acquire_for_processing(self) -> object:
        """Keep the native pointer alive until processing leaves native code."""
        with self._lock:
            if self._dispose_requested or self._ptr is None:
                raise FoundryLocalException(
                    f"{type(self).__name__} has been closed and can no longer be used."
                )
            self._active_processes += 1
            return self._ptr

    def _release_after_processing(self) -> None:
        with self._lock:
            self._active_processes -= 1
            if self._active_processes == 0:
                self._lock.notify_all()

    def _close(self) -> None:
        with self._lock:
            if self._closed:
                return
            if self._dispose_requested:
                # Another thread is already closing; wait for it to finish.
                while not self._closed:
                    self._lock.wait()
                return
            self._dispose_requested = True
            ptr = self._ptr
            release_request = self._release_request

        while True:
            with self._lock:
                if self._active_processes == 0:
                    self._ptr = None
                    break

            # Native cancellation may block; never hold the lifecycle condition.
            if ptr is not None:
                try:
                    from foundry_local_sdk._native.api import api

                    api.check_status(api.inference.Request_Cancel(ptr))
                except Exception:
                    pass

            with self._lock:
                if self._active_processes > 0:
                    self._lock.wait(timeout=_CANCEL_RETRY_INTERVAL_SECONDS)

        if ptr is not None:
            try:
                if release_request is not None:
                    release_request(ptr)
            except Exception:
                pass

        with self._lock:
            self._closed = True
            self._lock.notify_all()

    def __enter__(self) -> "Request":
        return self

    def __exit__(self, *_) -> None:
        self._close()

    def _finalize(self) -> None:
        """Release only if the condition is free and the request is open and idle."""
        condition = getattr(self, "_lock", None)
        if condition is None:
            return

        try:
            if not condition.acquire(blocking=False):
                return
            try:
                if self._closed or self._dispose_requested or self._active_processes:
                    return
                self._dispose_requested = True
                ptr = self._ptr
                release_request = self._release_request
                self._ptr = None
                self._closed = True
                condition.notify_all()
            finally:
                condition.release()

            try:
                if ptr is not None and release_request is not None:
                    release_request(ptr)
            except Exception:
                pass
        except Exception:
            pass

    def __del__(self) -> None:
        self._finalize()
