# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Configuration → native plumbing tests.

We do not initialize a Manager (the singleton is owned by the
``manager`` fixture and only one can exist per process). Instead we
build native handles directly and assert they are accepted by the
config vtable without raising.
"""
from __future__ import annotations

import importlib
from contextlib import contextmanager
from types import SimpleNamespace

import pytest

from foundry_local_sdk import Configuration, FoundryLocalException, LogLevel


@pytest.fixture(autouse=True)
def _require_native(native_api):
    return native_api


@contextmanager
def _native_config(c: Configuration):
    """Build a native config handle and guarantee release.

    Without this guard a failed assertion between ``_build_native()`` and
    ``Configuration_Release`` would leak the native handle.
    """
    from foundry_local_sdk._native.api import api

    ptr = c._build_native()
    try:
        yield ptr
    finally:
        if ptr is not None:
            api.config.Configuration_Release(ptr)


class TestBuildNative:
    def test_minimal_config_builds(self):
        c = Configuration(app_name="BuildNativeTest")
        with _native_config(c) as ptr:
            assert ptr is not None

    def test_all_directories_accepted(self, tmp_path):
        c = Configuration(
            app_name="BuildNativeTest",
            app_data_dir=str(tmp_path / "app"),
            model_cache_dir=str(tmp_path / "cache"),
            logs_dir=str(tmp_path / "logs"),
            log_level=LogLevel.WARNING,
        )
        with _native_config(c) as ptr:
            assert ptr is not None

    def test_catalog_urls_accepted(self):
        c = Configuration(
            app_name="BuildNativeTest",
            catalog_urls=[
                ("https://example.invalid/asset-gallery/v1.0/models", None),
                ("https://other.invalid/asset-gallery/v1.0/models", "filter=cpu"),
            ],
        )
        with _native_config(c) as ptr:
            assert ptr is not None

    def test_additional_settings_accepted(self):
        c = Configuration(
            app_name="BuildNativeTest",
            additional_settings={"K1": "v1", "K2": "v2"},
        )
        with _native_config(c) as ptr:
            assert ptr is not None

    def test_disable_nonessential_telemetry_accepted(self):
        c = Configuration(
            app_name="BuildNativeTest",
            disable_nonessential_telemetry=True,
        )
        with _native_config(c) as ptr:
            assert ptr is not None

    def test_shared_host_setting_reaches_native_manager_validation(self):
        from foundry_local_sdk._native.api import api, ffi

        configuration = Configuration(
            app_name="HostedNativeTest",
            use_shared_host=True,
            web=Configuration.WebService(external_url="http://127.0.0.1:1234"),
        )
        with _native_config(configuration) as pointer:
            output = ffi.new("flManager**")
            with pytest.raises(FoundryLocalException, match="shared hosting cannot use an external service") as error:
                api.check_status(api.root.Manager_Create(pointer, output))
            assert error.value.error_code == 3
            assert output[0] == ffi.NULL

    def test_v2_runtime_negotiation_preserves_default_configuration(self, monkeypatch):
        native = importlib.import_module("foundry_local_sdk._native.api")
        library = native._lib
        requested = []

        def legacy_get_api(version):
            requested.append(version)
            return native.ffi.NULL if version > 2 else library.FoundryLocalGetApi(version)

        monkeypatch.setattr(native, "_lib", SimpleNamespace(FoundryLocalGetApi=legacy_get_api))
        api = native._Api()
        assert requested == [3, 2]
        assert api.api_version == 2
        output = native.ffi.new("flConfiguration**")
        api.check_status(api.config.Create(b"LegacyNativeTest", output))
        try:
            assert output[0] != native.ffi.NULL
            with pytest.raises(FoundryLocalException, match="requires an API v3"):
                configuration = Configuration(app_name="LegacyNativeTest", use_shared_host=True)
                configuration._apply_settings(output[0], api, native.ffi)
        finally:
            api.config.Configuration_Release(output[0])
