# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Catalog acquisition through the native string-map boundary."""
from __future__ import annotations

import importlib
import threading
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from foundry_local_sdk import CatalogType, FoundryLocalManager
from foundry_local_sdk._native import ffi
from foundry_local_sdk.exception import FoundryLocalException


@pytest.fixture
def mock_catalog_manager(monkeypatch):
    manager = FoundryLocalManager.__new__(FoundryLocalManager)
    manager._native_manager = object()
    manager._catalogs = {}
    manager._close_started = threading.Event()
    manager._active_native_calls = 0
    manager._native_call_state = threading.local()
    manager._lifetime_changed = threading.Condition(FoundryLocalManager._lock)
    name = ffi.new("char[]", b"Mock catalog")
    settings = {}
    sources = {}

    def create_options(out):
        out[0] = ffi.cast("flKeyValuePairs *", 1)

    def add_option(_pairs, key, value):
        settings[key.decode()] = value.decode()

    def get_catalog(_manager, _type, _pairs, out):
        api.last_options = settings.copy()
        identity = (settings.get("name", "").lower(), settings.get("credential", "azure_cli"))
        out[0] = sources.setdefault(identity, ffi.cast("flCatalog *", len(sources) + 10))
        return ffi.NULL

    def get_existing(_manager, catalog_type, out):
        out[0] = ffi.cast("flCatalog *", catalog_type + 1)
        return ffi.NULL

    def get_name(_catalog, out):
        out[0] = name
        return ffi.NULL

    api = SimpleNamespace(
        api_version=4,
        root=SimpleNamespace(
            CreateKeyValuePairs=Mock(side_effect=create_options),
            AddKeyValuePair=Mock(side_effect=add_option),
            KeyValuePairs_Release=Mock(side_effect=lambda _: settings.clear()),
            Manager_GetCatalogWithOptions=Mock(side_effect=get_catalog),
            Manager_GetCatalogByType=Mock(side_effect=get_existing),
        ),
        catalog=SimpleNamespace(GetName=get_name),
        check_status=Mock(),
    )
    monkeypatch.setattr(importlib.import_module("foundry_local_sdk._native.api"), "api", api)
    yield manager, api
    manager._native_manager = None
    manager._catalogs.clear()


def test_options_are_copied_forwarded_and_native_identity_reuses_wrapper(mock_catalog_manager):
    manager, api = mock_catalog_manager
    options = {"name": "Mock-Registry"}
    catalog = manager.get_catalog(CatalogType.AZURE_REGISTRY, options)
    assert catalog.catalog_type is CatalogType.AZURE_REGISTRY
    assert catalog._parent is manager
    assert api.last_options == options
    assert manager.get_catalog(CatalogType.AZURE_REGISTRY, options) is catalog
    api.root.Manager_GetCatalogWithOptions.assert_called_once()
    options["name"] = "mock-registry"
    options["credential"] = "azure_cli"
    assert manager.get_catalog(CatalogType.AZURE_REGISTRY, options) is catalog
    api.root.KeyValuePairs_Release.assert_called()


def test_effective_source_and_credentials_keep_catalogs_separate(mock_catalog_manager):
    manager, _ = mock_catalog_manager
    first = manager.get_catalog(CatalogType.AZURE_REGISTRY, {"name": "one"})
    assert manager.get_catalog(CatalogType.AZURE_REGISTRY, {"name": "two"}) is not first
    assert manager.get_catalog(
        CatalogType.AZURE_REGISTRY, {"name": "one", "credential": "workload_identity"}
    ) is not first


@pytest.mark.parametrize("catalog_type", [CatalogType.PUBLIC, CatalogType.LOCAL])
def test_existing_catalogs_keep_their_native_route(mock_catalog_manager, catalog_type):
    manager, api = mock_catalog_manager
    catalog = manager.get_catalog(catalog_type)
    assert manager.get_catalog(catalog_type, {}) is catalog
    api.root.Manager_GetCatalogByType.assert_called_once()
    api.root.Manager_GetCatalogWithOptions.assert_not_called()


@pytest.mark.parametrize("options", [{"": "x"}, {"name": "invalid\0registry"}, {"n\0ame": "x"}, {"name": 1}])
def test_invalid_abi_strings_never_reach_native(mock_catalog_manager, options):
    manager, api = mock_catalog_manager
    with pytest.raises((ValueError, TypeError)):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, options)
    api.root.Manager_GetCatalogWithOptions.assert_not_called()


@pytest.mark.parametrize("api_version", [2, 3])
def test_catalog_options_require_native_v4(mock_catalog_manager, api_version):
    manager, api = mock_catalog_manager
    api.api_version = api_version
    with pytest.raises(FoundryLocalException, match="version 4"):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, {"name": "one"})
    api.root.CreateKeyValuePairs.assert_not_called()


def test_native_failure_releases_options_and_is_not_cached(mock_catalog_manager):
    manager, api = mock_catalog_manager
    api.check_status.side_effect = FoundryLocalException("invalid catalog options")
    with pytest.raises(FoundryLocalException, match="invalid catalog options"):
        manager.get_catalog(CatalogType.PUBLIC, {"unsupported": "value"})
    assert api.last_options == {"unsupported": "value"}
    assert not manager._catalogs
    api.root.KeyValuePairs_Release.assert_called_once()
    api.check_status.side_effect = None
    assert manager.get_catalog(CatalogType.AZURE_REGISTRY, {"name": "one"}) is not None


def test_missing_native_catalog_is_not_cached(mock_catalog_manager):
    manager, api = mock_catalog_manager
    api.root.Manager_GetCatalogWithOptions.side_effect = None
    with pytest.raises(FoundryLocalException, match="no azure_registry catalog"):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, {"name": "one"})
    assert not manager._catalogs


def test_closed_manager_cannot_return_cached_catalog(mock_catalog_manager):
    manager, api = mock_catalog_manager
    manager.get_catalog(CatalogType.AZURE_REGISTRY, {"name": "one"})
    manager._close_started.set()
    with pytest.raises(RuntimeError, match="closed"):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, {"name": "one"})
    api.root.Manager_GetCatalogWithOptions.assert_called_once()
