# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Singleton lifecycle: close() clears the slot so a new manager can be built.

Named ``test_zz_...`` so it sorts after other integration tests; tearing
down and rebuilding the singleton mid-session would invalidate any cached
fixture references.
"""
from __future__ import annotations

import pytest

from foundry_local_sdk import Configuration, FoundryLocalManager, LogLevel


def _make_config(manager) -> Configuration:
    """Mirror the conftest manager config so the rebuilt singleton uses the same cache."""
    src = manager.config
    kwargs: dict[str, object] = {
        "app_name": src.app_name,
        "log_level": src.log_level or LogLevel.WARNING,
    }
    if src.model_cache_dir:
        kwargs["model_cache_dir"] = src.model_cache_dir
    if src.app_data_dir:
        kwargs["app_data_dir"] = src.app_data_dir
    if src.logs_dir:
        kwargs["logs_dir"] = src.logs_dir
    return Configuration(**kwargs)


@pytest.fixture
def restore_singleton(manager):
    """Save the current config, run the test, then leave a working singleton in place."""
    saved_config = _make_config(manager)
    yield saved_config
    if FoundryLocalManager.instance is None:
        FoundryLocalManager(saved_config)


class TestSingletonRecreate:
    def test_close_clears_singleton(self, restore_singleton):
        config = restore_singleton
        # Close whatever singleton currently exists.
        assert FoundryLocalManager.instance is not None
        FoundryLocalManager.instance.close()
        assert FoundryLocalManager.instance is None

        # A fresh manager can now be constructed and registers as the singleton.
        new_mgr = FoundryLocalManager(config)
        assert FoundryLocalManager.instance is new_mgr

    def test_close_is_idempotent(self, restore_singleton):
        mgr = FoundryLocalManager.instance
        assert mgr is not None
        mgr.close()
        # Second close must not raise.
        mgr.close()
        assert FoundryLocalManager.instance is None

    def test_context_manager_clears_singleton(self, restore_singleton):
        config = restore_singleton
        # Tear down the existing singleton so the with-block can build a new one.
        if FoundryLocalManager.instance is not None:
            FoundryLocalManager.instance.close()

        with FoundryLocalManager(config) as m:
            assert FoundryLocalManager.instance is m
        assert FoundryLocalManager.instance is None
