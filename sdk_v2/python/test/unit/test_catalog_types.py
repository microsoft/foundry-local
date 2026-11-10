# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Mocked tests for catalog selection and registry-bound TEST wrappers."""
from __future__ import annotations

import inspect
import importlib
import threading
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from foundry_local_sdk import CatalogType, FoundryLocalManager
from foundry_local_sdk._native import ffi
from foundry_local_sdk.exception import FoundryLocalException


def test_catalog_type_values_match_native_c_enum() -> None:
    assert CatalogType.PUBLIC == 0
    assert CatalogType.LOCAL == 1


def test_get_catalog_defaults_to_public_catalog() -> None:
    default = inspect.signature(FoundryLocalManager.get_catalog).parameters["catalog_type"].default
    assert default is CatalogType.PUBLIC


@pytest.fixture
def mock_catalog_manager(monkeypatch):
    manager = FoundryLocalManager.__new__(FoundryLocalManager)
    manager._native_manager = object()
    manager._catalogs = {}
    manager._close_started = threading.Event()
    name = ffi.new("char[]", b"Mock catalog")

    def get_test_catalog(_manager, _registry, out):
        out[0] = ffi.cast("flCatalog *", 1)
        return ffi.NULL

    def get_catalog_by_type(_manager, _catalog_type, out):
        out[0] = ffi.cast("flCatalog *", 2)
        return ffi.NULL

    def get_name(_catalog, out):
        out[0] = name
        return ffi.NULL

    api = SimpleNamespace(
        api_version=4,
        root=SimpleNamespace(
            Manager_GetTestCatalog=Mock(side_effect=get_test_catalog),
            Manager_GetCatalogByType=Mock(side_effect=get_catalog_by_type),
        ),
        catalog=SimpleNamespace(GetName=get_name),
        check_status=Mock(),
    )
    monkeypatch.setattr(importlib.import_module("foundry_local_sdk._native.api"), "api", api)
    yield manager, api
    manager._native_manager = None
    manager._catalogs.clear()


def test_test_catalog_routes_registry_and_reuses_case_insensitive_wrapper(mock_catalog_manager):
    manager, api = mock_catalog_manager
    catalog = manager.get_catalog(CatalogType.TEST, registry_name="Mock-Registry")
    assert catalog.catalog_type is CatalogType.TEST
    assert catalog._parent is manager
    assert manager.get_catalog(CatalogType.TEST, registry_name="mock-registry") is catalog
    api.root.Manager_GetTestCatalog.assert_called_once()
    assert api.root.Manager_GetTestCatalog.call_args.args[:2] == (manager._native_manager, b"Mock-Registry")
    api.root.Manager_GetCatalogByType.assert_not_called()


def test_test_catalog_keeps_registries_and_native_default_separate(mock_catalog_manager):
    manager, api = mock_catalog_manager
    first = manager.get_catalog(CatalogType.TEST, registry_name="registry-one")
    second = manager.get_catalog(CatalogType.TEST, registry_name="registry-two")
    default = manager.get_catalog(CatalogType.TEST)
    assert first is not second
    assert default is not first and default is not second
    assert [call.args[1] for call in api.root.Manager_GetTestCatalog.call_args_list] == [
        b"registry-one", b"registry-two", ffi.NULL,
    ]


@pytest.mark.parametrize("catalog_type", [CatalogType.PUBLIC, CatalogType.LOCAL])
def test_existing_catalogs_keep_their_native_route(mock_catalog_manager, catalog_type):
    manager, api = mock_catalog_manager
    catalog = manager.get_catalog(catalog_type)
    assert catalog.catalog_type is catalog_type
    assert manager.get_catalog(catalog_type) is catalog
    api.root.Manager_GetCatalogByType.assert_called_once()
    assert api.root.Manager_GetCatalogByType.call_args.args[:2] == (manager._native_manager, int(catalog_type))
    api.root.Manager_GetTestCatalog.assert_not_called()


@pytest.mark.parametrize("catalog_type", [CatalogType.PUBLIC, CatalogType.LOCAL])
def test_registry_name_is_rejected_for_non_test_catalogs(mock_catalog_manager, catalog_type):
    manager, api = mock_catalog_manager
    with pytest.raises(ValueError, match="only supported for CatalogType.TEST"):
        manager.get_catalog(catalog_type, registry_name="mock-registry")
    api.root.Manager_GetTestCatalog.assert_not_called()
    api.root.Manager_GetCatalogByType.assert_not_called()


@pytest.mark.parametrize("registry_name", ["", "invalid\0registry"])
def test_invalid_registry_strings_never_reach_native(mock_catalog_manager, registry_name):
    manager, api = mock_catalog_manager
    with pytest.raises(ValueError):
        manager.get_catalog(CatalogType.TEST, registry_name=registry_name)
    api.root.Manager_GetTestCatalog.assert_not_called()


@pytest.mark.parametrize("api_version", [2, 3])
def test_test_catalog_requires_native_v4(mock_catalog_manager, api_version):
    manager, api = mock_catalog_manager
    api.api_version = api_version
    with pytest.raises(FoundryLocalException, match="version 4"):
        manager.get_catalog(CatalogType.TEST, registry_name="mock-registry")
    api.root.Manager_GetTestCatalog.assert_not_called()


def test_native_failure_is_propagated_and_not_cached(mock_catalog_manager):
    manager, api = mock_catalog_manager
    api.check_status.side_effect = FoundryLocalException("mock registry failure")
    with pytest.raises(FoundryLocalException, match="mock registry failure"):
        manager.get_catalog(CatalogType.TEST, registry_name="mock-registry")
    assert not manager._catalogs
    api.check_status.side_effect = None
    assert manager.get_catalog(CatalogType.TEST, registry_name="mock-registry") is not None
    assert api.root.Manager_GetTestCatalog.call_count == 2


def test_missing_native_test_catalog_is_not_cached(mock_catalog_manager):
    manager, api = mock_catalog_manager
    api.root.Manager_GetTestCatalog.side_effect = None
    with pytest.raises(FoundryLocalException, match="no test catalog"):
        manager.get_catalog(CatalogType.TEST)
    assert not manager._catalogs


def test_closed_manager_cannot_return_a_cached_test_catalog(mock_catalog_manager):
    manager, api = mock_catalog_manager
    manager.get_catalog(CatalogType.TEST, registry_name="mock-registry")
    manager._close_started.set()
    with pytest.raises(RuntimeError, match="closed"):
        manager.get_catalog(CatalogType.TEST, registry_name="mock-registry")
    api.root.Manager_GetTestCatalog.assert_called_once()