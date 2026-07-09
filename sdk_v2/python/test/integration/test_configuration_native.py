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

from contextlib import contextmanager

import pytest

from foundry_local_sdk import Configuration, LogLevel


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
                ("https://example.invalid/catalog.json", None),
                ("https://example.invalid/other.json", "filter=cpu"),
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
