# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Singleton lifecycle: close() clears the slot so a new manager can be built.

Named ``test_zz_...`` so it sorts after other integration tests; tearing
down and rebuilding the singleton mid-session would invalidate any cached
fixture references.

.. warning::
   **Fragile ordering invariant.** This file's ``restore_singleton`` fixture
   closes and rebuilds the global ``FoundryLocalManager`` singleton. The
   session-scoped ``manager`` fixture in ``conftest.py`` caches a reference
   to the *original* singleton, so any test that uses it **after** this file
   would get a stale reference.

   It works today because:

   1. pytest collects integration tests in alphabetical filename order, so
      ``test_zz_*.py`` runs after every other ``test_*.py``.
   2. The conftest teardown guards ``mgr.close()`` with
      ``FoundryLocalManager.instance is mgr`` so the post-rebuild reference
      mismatch is tolerated.

   If a future test file is added with a name that sorts after ``test_zz_``
   (or uses ``pytest-randomly`` / ``pytest-ordering``), this invariant breaks
   silently. The principled fix is subprocess isolation (``pytest-forked``
   with ``@pytest.mark.forked``) or a separate CI invocation for singleton-
   lifecycle tests. Defer until a third lifecycle test exists or the
   collection order changes.
"""
from __future__ import annotations

import json
import threading
import uuid
from concurrent.futures import ThreadPoolExecutor

import pytest

from foundry_local_sdk import (
    CatalogType,
    Configuration,
    FoundryLocalManager,
    LogLevel,
    ModelInfoBuilder,
)


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

    def test_custom_metadata_survives_manager_recreation(self, restore_singleton, tmp_path):
        config = restore_singleton
        model_path = tmp_path / "model"
        model_path.mkdir()
        (model_path / "genai_config.json").write_text(
            json.dumps({"model": {"type": "phi3"}}), encoding="utf-8"
        )
        model_id = f"python-byom-recreate-{uuid.uuid4().hex}:1"

        assert FoundryLocalManager.instance is not None
        FoundryLocalManager.instance.close()
        first = FoundryLocalManager(config)
        first_catalog = first.get_catalog(CatalogType.LOCAL)
        with ModelInfoBuilder() as metadata:
            metadata.set_string_property("task", "chat-completion")
            metadata.set_string_property("custom_marker", "persist-me")
            metadata.set_int_property("custom_count", 42)
            registered = first_catalog.register_model(model_path, model_id, metadata)

        registered_info = registered.info
        assert registered_info.get_string_property("custom_marker") == "persist-me"
        same_manager_lookup = first_catalog.get_model_variant(model_id)
        assert same_manager_lookup is not None
        assert same_manager_lookup.info.get_int_property("custom_count", -1) == 42

        first.close()
        with pytest.raises(RuntimeError, match="closed"):
            registered_info.get_string_property("custom_marker")
        with pytest.raises(RuntimeError, match="closed"):
            registered_info.get_int_property("custom_count", -1)

        second = FoundryLocalManager(config)
        second_catalog = second.get_catalog(CatalogType.LOCAL)
        recreated_lookup = second_catalog.get_model_variant(model_id)
        assert recreated_lookup is not None
        assert recreated_lookup.info.get_string_property("custom_marker") == "persist-me"
        assert recreated_lookup.info.get_int_property("custom_count", -1) == 42
        second_catalog.unregister_model(model_id)

    def test_close_waits_for_model_call_and_rejects_later_borrowed_calls(self, restore_singleton, tmp_path):
        config = restore_singleton
        model_path = tmp_path / "model"
        model_path.mkdir()
        (model_path / "genai_config.json").write_text(
            json.dumps({"model": {"type": "phi3"}}), encoding="utf-8"
        )
        model_id = f"python-close-race-{uuid.uuid4().hex}:1"

        assert FoundryLocalManager.instance is not None
        FoundryLocalManager.instance.close()
        first = FoundryLocalManager(config)
        first_catalog = first.get_catalog(CatalogType.LOCAL)
        with ModelInfoBuilder() as metadata:
            metadata.set_string_property("task", "chat-completion")
            registered = first_catalog.register_model(model_path, model_id, metadata)

        registered._before_native_call_for_test = first.close  # type: ignore[attr-defined]
        with pytest.raises(RuntimeError, match="active native call"):
            _ = registered.is_cached
        assert FoundryLocalManager.instance is first
        registered._before_native_call_for_test = None  # type: ignore[attr-defined]

        operation_entered = threading.Event()
        close_attempting_lock = threading.Event()

        def before_native_call() -> None:
            operation_entered.set()
            assert close_attempting_lock.wait(timeout=10)

        registered._before_native_call_for_test = before_native_call  # type: ignore[attr-defined]
        first._before_close_lock_for_test = close_attempting_lock.set
        with ThreadPoolExecutor(max_workers=2) as executor:
            operation_future = executor.submit(lambda: registered.is_cached)
            assert operation_entered.wait(timeout=10)
            close_future = executor.submit(first.close)
            assert operation_future.result(timeout=10) is True
            close_future.result(timeout=10)
        with pytest.raises(RuntimeError, match="closed"):
            _ = registered.is_cached
        with pytest.raises(RuntimeError, match="closed"):
            first_catalog.list_models()

        second = FoundryLocalManager(config)
        second_catalog = second.get_catalog(CatalogType.LOCAL)
        recreated = second_catalog.get_model_variant(model_id)
        assert recreated is not None
        second_catalog.unregister_model(model_id)
