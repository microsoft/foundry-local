# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Native-layer smoke tests — assert the cffi extension and vtable are wired up.

These tests do not require any model. They only verify that the bindings
were built, can be loaded, and expose the expected vtable surface.
"""
from __future__ import annotations

import pytest


def test_native_module_loads(native_api):
    api, ffi = native_api
    assert api is not None
    assert ffi is not None


def test_root_vtable_populated(native_api):
    api, _ = native_api
    # Functions touched by every test path — if any are missing the cffi
    # build is broken or out of sync with foundry_local_c.h.
    for name in (
        "Manager_Create",
        "Manager_GetCatalog",
        "CreateKeyValuePairs",
        "AddKeyValuePair",
        "KeyValuePairs_Release",
    ):
        assert hasattr(api.root, name), f"api.root missing {name}"


@pytest.mark.parametrize("version", (2, 3, 4))
def test_root_vtable_reads_legacy_fields_across_api_versions(native_api, version):
    from foundry_local_sdk._native import _cffi_bindings

    _, ffi = native_api
    root = _cffi_bindings.lib.FoundryLocalGetApi(version)
    assert root != ffi.NULL
    assert root.GetItemApi() != ffi.NULL
    assert root.GetConfigurationApi() != ffi.NULL


def test_subsystem_vtables_populated(native_api):
    api, _ = native_api
    for vt_name in ("config", "catalog", "model", "item", "inference"):
        vt = getattr(api, vt_name)
        assert vt is not None, f"api.{vt_name} is None"


def test_check_status_null_is_noop(native_api):
    """A NULL status pointer indicates success and must return without raising."""
    api, ffi = native_api
    api.check_status(ffi.NULL)  # must be a silent no-op


def test_check_status_failure_raises_with_native_error(native_api):
    """Provoke a real native error and assert ``check_status`` raises.

    ``GetModel`` is documented to return a non-NULL status (or NULL model)
    on a malformed request. We use ``Manager_Create`` with a NULL
    configuration which the native layer rejects with an error status —
    a stable, side-effect-free way to obtain a real ``flStatus*``.
    """
    from foundry_local_sdk.exception import FoundryLocalException

    api, ffi = native_api

    mgr_out = ffi.new("flManager**")
    status = api.root.Manager_Create(ffi.NULL, mgr_out)
    if status == ffi.NULL:
        # The native layer accepted a NULL configuration on this build —
        # nothing to assert. Release the manager (if one was created)
        # and skip rather than fabricating a failure.
        if mgr_out[0] != ffi.NULL:
            api.root.Manager_Release(mgr_out[0])
        pytest.skip("Manager_Create accepted NULL configuration on this build")

    with pytest.raises(FoundryLocalException):
        api.check_status(status)
