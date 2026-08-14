# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import math
from typing import TYPE_CHECKING

from foundry_local_sdk.exception import FoundryLocalException

if TYPE_CHECKING:
    from foundry_local_sdk.items import Item
    from foundry_local_sdk.session_types import RequestOptions

_API_VERSION = 1  # FOUNDRY_LOCAL_API_VERSION
_MAX_TIMEOUT_MS = (1 << 63) - 1


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
        self._check_open()
        from foundry_local_sdk._native.api import api

        api.check_status(api.inference.Request_Cancel(self._ptr))

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

    def _close(self) -> None:
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
