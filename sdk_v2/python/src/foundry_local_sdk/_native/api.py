"""_Api singleton: loads the flApi vtable and provides status-to-exception conversion.

Usage (internal only — public code never imports from _native)::

    from foundry_local_sdk._native.api import api

    api.check_status(api.root.Manager_Create(cfg_ptr, out_mgr))
    version = api.version_string()
"""

from __future__ import annotations

import importlib
import os

from foundry_local_sdk._native.lib_loader import find_library, prepare_native_dependencies
from foundry_local_sdk.exception import FoundryLocalException

# FOUNDRY_LOCAL_API_VERSION = 2 (from foundry_local_c.h)
_FOUNDRY_LOCAL_API_VERSION: int = 2

_lib_path = find_library()

# Preload ORT and GenAI BEFORE the cffi extension imports libfoundry_local.
# Once they're resident in the process, libfoundry_local's NEEDED entries
# (Linux/macOS) and IAT references (Windows) for onnxruntime / onnxruntime-genai
# resolve from the already-loaded module table by name — no filesystem search,
# no RPATH involved. This matches the load pattern used by the C# SDK; see
# lib_loader.prepare_native_dependencies for the why.
#
# The returned handles MUST be kept alive at module scope. We assign to a
# module-level name so the GC never collects them (which would unload the DLLs
# mid-process and crash any in-flight ORT call).
_preloaded_native_deps: list = []
if _lib_path is not None:
    _preloaded_native_deps = prepare_native_dependencies(_lib_path.parent)

# Make foundry_local available to the dynamic loader before importing the cffi extension. The extension was
# linked against foundry_local at build time and lists it as a NEEDED dependency; without help, the loader
# won't find it (Windows: not on PATH; Linux/macOS: not on LD_LIBRARY_PATH/DYLD_LIBRARY_PATH).
# RPATH ($ORIGIN / @loader_path) baked in by CMake handles the case where the wheel layout puts deps next
# to libfoundry_local; the explicit add_dll_directory / CDLL below handles the case where it doesn't.
if _lib_path is not None:
    if hasattr(os, "add_dll_directory"):
        # Windows: extend the loader's DLL search path so the cffi extension's import-table reference to
        # foundry_local.dll resolves. ORT/GenAI were already preloaded by absolute path above.
        _dll_parent = _lib_path.parent.resolve()
        if _dll_parent.is_dir():
            os.add_dll_directory(str(_dll_parent))
    else:
        # Linux/macOS: preload libfoundry_local with RTLD_GLOBAL so its exported symbols satisfy the cffi
        # extension's NEEDED entry by symbol resolution rather than by file lookup. ORT/GenAI are already
        # loaded (above), so libfoundry_local's own NEEDED entries for them resolve to those modules.
        import ctypes

        _preloaded_native_deps.append(ctypes.CDLL(str(_lib_path), mode=ctypes.RTLD_GLOBAL))

# Import the compiled cffi extension (API mode: struct layouts verified at
# build time, function calls compiled-in rather than interpreted).
_cffi_mod = importlib.import_module("foundry_local_sdk._native._cffi_bindings")
ffi = _cffi_mod.ffi
_lib = _cffi_mod.lib


class _Api:
    """Thin wrapper around the flApi vtable returned by FoundryLocalGetApi.

    Acquires all sub-API pointers at construction time so callers never have
    to call the accessor functions themselves.

    This object is a process-level singleton (``api`` below).  Do not create
    additional instances.
    """

    def __init__(self) -> None:
        root_ptr = _lib.FoundryLocalGetApi(_FOUNDRY_LOCAL_API_VERSION)
        if root_ptr == ffi.NULL:
            raise FoundryLocalException(
                f"FoundryLocalGetApi({_FOUNDRY_LOCAL_API_VERSION}) returned NULL — "
                "library version mismatch or corrupt binary.",
                error_code=0,
            )
        # root is const flApi* — access function pointer members directly.
        self.root = root_ptr
        self.item = self.root.GetItemApi()
        self.inference = self.root.GetInferenceApi()
        self.config = self.root.GetConfigurationApi()
        self.catalog = self.root.GetCatalogApi()
        self.model = self.root.GetModelApi()

    def check_status(self, status: object) -> None:
        """Raise ``FoundryLocalException`` if *status* is non-NULL.

        Always releases *status* so callers never need to call
        ``Status_Release`` explicitly.
        """
        if status == ffi.NULL:
            return
        try:
            code = self.root.Status_GetErrorCode(status)
            msg_ptr = self.root.Status_GetErrorMessage(status)
            msg = ffi.string(msg_ptr).decode("utf-8") if msg_ptr != ffi.NULL else "Unknown error"
        finally:
            self.root.Status_Release(status)
        raise FoundryLocalException(msg, error_code=int(code))

    @staticmethod
    def version_string() -> str:
        """Return the library version string from ``FoundryLocalGetVersionString``."""
        ptr = _lib.FoundryLocalGetVersionString()
        if ptr == ffi.NULL:
            return ""
        return ffi.string(ptr).decode("utf-8")


class _LazyApi:
    """Lazy proxy that constructs the ``_Api`` singleton on first attribute access.

    Deferring construction keeps module import cheap and — more importantly — makes a failed
    ``FoundryLocalGetApi`` call recoverable. If construction were eager at import time, Python would cache the
    ``ImportError`` permanently and no later ``import`` could retry, even after the underlying problem (missing
    DLL on PATH, bad version) was fixed in the same process.

    Forwards every attribute (including the ``version_string`` ``@staticmethod``) through normal attribute
    lookup on the underlying singleton.
    """

    __slots__ = ("_instance",)

    def __init__(self) -> None:
        self._instance: _Api | None = None

    def _get(self) -> _Api:
        inst = self._instance
        if inst is None:
            inst = _Api()
            self._instance = inst
        return inst

    def __getattr__(self, name: str) -> object:
        return getattr(self._get(), name)


# Process-level singleton (lazily constructed on first attribute access).
api = _LazyApi()
