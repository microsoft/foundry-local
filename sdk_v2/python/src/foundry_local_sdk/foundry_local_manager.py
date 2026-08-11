# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import threading
from typing import Callable

from foundry_local_sdk.catalog import Catalog
from foundry_local_sdk.configuration import Configuration
from foundry_local_sdk.ep_types import EpDownloadResult, EpInfo
from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.logging_helper import set_default_logger_severity


class FoundryLocalManager:
    """Singleton manager for Foundry Local SDK operations.

    Call ``FoundryLocalManager.initialize(config)`` once at startup, then
    access the singleton via ``FoundryLocalManager.instance``.

    Attributes:
        instance: The singleton ``FoundryLocalManager`` instance.
        catalog: The model ``Catalog`` for discovering and managing models.
        urls: Bound URL(s) after ``start_web_service()`` is called, or ``None``.
    """

    _lock: threading.Lock = threading.Lock()
    instance: FoundryLocalManager | None = None

    @staticmethod
    def initialize(config: Configuration) -> None:
        """Initialize the Foundry Local SDK with the given configuration.

        Must be called before using any other part of the SDK.

        Args:
            config: Configuration object for the SDK.
        """
        FoundryLocalManager(config)

    def __init__(self, config: Configuration) -> None:
        # Declared up front so close() / __del__ can run safely even if
        # _initialize() raises before the native handle is assigned.
        self._native_manager: object | None = None
        self.urls: list[str] | None = None

        with FoundryLocalManager._lock:
            if FoundryLocalManager.instance is not None:
                raise FoundryLocalException(
                    "FoundryLocalManager is a singleton and has already been initialized."
                )
            config.validate()
            self.config = config
            self._initialize()
            FoundryLocalManager.instance = self

        # Register an interpreter-shutdown hook to release the native Manager
        # before CPython tears down modules and dyld runs libfoundry_local's
        # C++ static destructors. Without this, the native dtor chain
        # (Manager → SpdlogLogger → spdlog::async_logger::flush) can fire
        # after spdlog's global thread pool has already been destroyed,
        # raising std::system_error("mutex lock failed") and aborting the
        # process. atexit guarantees we run before any of that.
        import atexit
        atexit.register(self.close)

    def _initialize(self) -> None:
        from foundry_local_sdk._native.api import api, ffi

        # Only push a log level into the native side if the caller actually picked one. ``None`` means "use
        # whatever default the native runtime decides" — forwarding ``None`` would only work by accident
        # through ``dict.get(None, default)``.
        if self.config.log_level is not None:
            set_default_logger_severity(self.config.log_level)

        native_config = self.config._build_native()
        try:
            mgr_out = ffi.new("flManager**")
            api.check_status(api.root.Manager_Create(native_config, mgr_out))
            self._native_manager = mgr_out[0]
        finally:
            # Manager_Create takes a const flConfiguration* — it copies what it needs.
            # We own the config handle and release it now.
            api.config.Configuration_Release(native_config)

        try:
            cat_out = ffi.new("flCatalog**")
            api.check_status(api.root.Manager_GetCatalog(self._native_manager, cat_out))
            self.catalog = Catalog(cat_out[0], parent=self)
        except BaseException:
            # Catalog fetch failed; release the manager handle to avoid leaking it.
            try:
                api.root.Manager_Release(self._native_manager)
            finally:
                self._native_manager = None
            raise

    # ------------------------------------------------------------------
    # EP discovery and registration
    # ------------------------------------------------------------------

    def discover_eps(self) -> list[EpInfo]:
        """Discover available execution providers and their registration status.

        Returns:
            List of ``EpInfo`` entries for all discoverable EPs.
        """
        from foundry_local_sdk._native.api import api, ffi

        eps_out = ffi.new("flEpInfo**")
        count_out = ffi.new("size_t*")
        api.check_status(
            api.root.Manager_GetDiscoverableEps(
                self._native_manager, eps_out, count_out
            )
        )

        count = int(count_out[0])
        result: list[EpInfo] = []
        for i in range(count):
            entry = eps_out[0][i]
            name = ffi.string(entry.name).decode("utf-8")
            is_reg = bool(entry.is_registered)
            result.append(EpInfo(name=name, is_registered=is_reg))
        return result

    def download_and_register_eps(
        self,
        names: list[str] | None = None,
        progress_callback: Callable[[str, float], None] | None = None,
    ) -> EpDownloadResult:
        """Download and register execution providers.

        Args:
            names: EP names to download. ``None`` or an empty list downloads
                all discoverable EPs.
            progress_callback: Optional callback ``(ep_name: str, percent: float)``
                invoked as each EP downloads.  ``percent`` is 0–100.

        Returns:
            ``EpDownloadResult`` describing operation status and per-EP outcomes.
        """
        # An empty list is treated as "download all" (same as None) for consistency across language bindings.
        if names is not None and len(names) == 0:
            names = None

        from foundry_local_sdk._native.api import api, ffi

        # Snapshot before-state to compute the delta of newly registered EPs.
        before_eps: dict[str, bool] = {ep.name: ep.is_registered for ep in self.discover_eps()}

        # Build the native EP-names array (or NULL to download all).
        if names is not None:
            # Keep encoded byte strings alive for the duration of the call.
            c_name_bufs = [ffi.new("char[]", n.encode("utf-8")) for n in names]
            c_names_arr = ffi.new("const char*[]", c_name_bufs)
            num_names = len(names)
        else:
            c_names_arr = ffi.NULL
            num_names = 0

        # Build the progress callback trampoline if requested.
        cb = ffi.NULL
        ud = ffi.NULL
        if progress_callback is not None:
            self._ep_cb_handle = ffi.new_handle(progress_callback)

            # Use the cdef typedef rather than an inline signature: the API-mode
            # runtime parser in _cffi_backend does not accept `const`, but the
            # cdef'd typedef preserves it. This keeps the const char* contract
            # the C ABI exposes (and matches the v1 SDK).
            @ffi.callback("flEpProgressCallback")
            def _ep_cb(ep_name_ptr: object, value: float, user_data: object) -> int:
                try:
                    fn = ffi.from_handle(user_data)
                    ep_name = (
                        ffi.string(ep_name_ptr).decode("utf-8")
                        if ep_name_ptr != ffi.NULL
                        else ""
                    )
                    fn(ep_name, float(value))
                    return 0
                except Exception:
                    return 1

            self._ep_cb = _ep_cb  # prevent GC
            cb = _ep_cb
            ud = self._ep_cb_handle

        api.check_status(
            api.root.Manager_DownloadAndRegisterEps(
                self._native_manager, c_names_arr, num_names, cb, ud
            )
        )

        # Determine which EPs were newly registered.
        after_eps = self.discover_eps()
        registered = [
            ep.name
            for ep in after_eps
            if ep.is_registered and not before_eps.get(ep.name, False)
        ]
        failed = [
            ep.name
            for ep in after_eps
            if not ep.is_registered and ep.name in (names or [])
        ]

        # Native owns catalog cache invalidation after EP registration; no Python-side action needed.
        return EpDownloadResult(
            success=len(failed) == 0,
            status="Completed",
            registered_eps=registered,
            failed_eps=failed,
        )

    # ------------------------------------------------------------------
    # Web service lifecycle
    # ------------------------------------------------------------------

    def start_web_service(self) -> None:
        """Start the optional built-in web service.

        The service binds to the URL(s) specified in ``Configuration.web.urls``,
        or ``http://127.0.0.1:0`` (a random ephemeral port) if not specified.
        ``FoundryLocalManager.urls`` is updated with the actual bound URL(s).
        """
        from foundry_local_sdk._native.api import api, ffi

        with FoundryLocalManager._lock:
            api.check_status(api.root.Manager_WebServiceStart(self._native_manager))

            urls_out = ffi.new("char***")
            count_out = ffi.new("size_t*")
            api.check_status(
                api.root.Manager_WebServiceUrls(self._native_manager, urls_out, count_out)
            )
            self.urls = [
                ffi.string(urls_out[0][i]).decode("utf-8") for i in range(int(count_out[0]))
            ]

    def stop_web_service(self) -> None:
        """Stop the optional built-in web service.

        Raises:
            FoundryLocalException: If the web service is not currently running.
        """
        from foundry_local_sdk._native.api import api

        with FoundryLocalManager._lock:
            if self.urls is None:
                raise FoundryLocalException("Web service is not running.")

            api.check_status(api.root.Manager_WebServiceStop(self._native_manager))
            self.urls = None

    # ------------------------------------------------------------------
    # Shutdown
    # ------------------------------------------------------------------

    def shutdown(self) -> None:
        """Initiate graceful shutdown of the native manager.

        Safe to call from any thread. Idempotent.
        """
        from foundry_local_sdk._native.api import api

        api.check_status(api.root.Manager_Shutdown(self._native_manager))

    def is_shutdown_requested(self) -> bool:
        """Whether ``shutdown()`` has been called on the native manager."""
        from foundry_local_sdk._native.api import api

        return bool(api.root.Manager_IsShutdownRequested(self._native_manager))

    def close(self) -> None:
        """Tear down the native manager and clear the singleton.

        After ``close()`` returns, ``FoundryLocalManager.instance`` is ``None`` and
        a fresh ``FoundryLocalManager(config)`` may be constructed. The native
        side enforces single-instance semantics via its own singleton; this method
        drives the native ``Manager_Shutdown`` (which drains sessions and unloads
        models) before releasing the handle.

        Idempotent. Safe to call multiple times.
        """
        import logging

        from foundry_local_sdk._native.api import api

        with FoundryLocalManager._lock:
            # Idempotent — close() called twice or after a failed __init__.
            if self._native_manager is None:
                if FoundryLocalManager.instance is self:
                    FoundryLocalManager.instance = None
                return

            # Drive the orchestrated drain on the native side. Log shutdown errors
            # rather than swallowing them silently — we still need to release the
            # handle, but the failure must surface somewhere.
            try:
                api.check_status(api.root.Manager_Shutdown(self._native_manager))
            except Exception as exc:
                logging.getLogger("foundry_local_sdk").warning(
                    "Manager_Shutdown failed during close(); releasing handle anyway: %s", exc
                )

            try:
                api.root.Manager_Release(self._native_manager)
            finally:
                self._native_manager = None
                if FoundryLocalManager.instance is self:
                    FoundryLocalManager.instance = None

    def __enter__(self) -> "FoundryLocalManager":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def __del__(self) -> None:
        # Best-effort safety net — production code should call close() explicitly.
        try:
            self.close()
        except Exception:
            pass
