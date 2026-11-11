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
    assert CatalogType.AZURE_REGISTRY == 2


def test_get_catalog_defaults_to_public_catalog() -> None:
    default = inspect.signature(FoundryLocalManager.get_catalog).parameters["catalog_type"].default
    assert default is CatalogType.PUBLIC


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

    def get_azure_registry_catalog(_manager, _registry, out):
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
            Manager_GetAzureRegistryCatalog=Mock(side_effect=get_azure_registry_catalog),
            Manager_GetCatalogByType=Mock(side_effect=get_catalog_by_type),
        ),
        catalog=SimpleNamespace(GetName=get_name),
        check_status=Mock(),
    )
    monkeypatch.setattr(importlib.import_module("foundry_local_sdk._native.api"), "api", api)
    yield manager, api
    manager._native_manager = None
    manager._catalogs.clear()


def test_azure_registry_catalog_routes_registry_and_reuses_case_insensitive_wrapper(mock_catalog_manager):
    manager, api = mock_catalog_manager
    catalog = manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="Mock-Registry")
    assert catalog.catalog_type is CatalogType.AZURE_REGISTRY
    assert catalog._parent is manager
    assert manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="mock-registry") is catalog
    api.root.Manager_GetAzureRegistryCatalog.assert_called_once()
    assert api.root.Manager_GetAzureRegistryCatalog.call_args.args[:2] == (manager._native_manager, b"Mock-Registry")
    api.root.Manager_GetCatalogByType.assert_not_called()


def test_azure_registry_catalog_requires_registry_name(mock_catalog_manager):
    manager, api = mock_catalog_manager
    with pytest.raises(ValueError, match="registry_name is required"):
        manager.get_catalog(CatalogType.AZURE_REGISTRY)
    with pytest.raises(ValueError, match="registry_name must not be empty"):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="")
    api.root.Manager_GetAzureRegistryCatalog.assert_not_called()


def test_azure_registry_catalog_keeps_registries_separate(mock_catalog_manager):
    manager, api = mock_catalog_manager
    first = manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="registry-one")
    second = manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="registry-two")
    assert first is not second
    assert [call.args[1] for call in api.root.Manager_GetAzureRegistryCatalog.call_args_list] == [
        b"registry-one", b"registry-two",
    ]


@pytest.mark.parametrize("catalog_type", [CatalogType.PUBLIC, CatalogType.LOCAL])
def test_existing_catalogs_keep_their_native_route(mock_catalog_manager, catalog_type):
    manager, api = mock_catalog_manager
    catalog = manager.get_catalog(catalog_type)
    assert catalog.catalog_type is catalog_type
    assert manager.get_catalog(catalog_type) is catalog
    api.root.Manager_GetCatalogByType.assert_called_once()
    assert api.root.Manager_GetCatalogByType.call_args.args[:2] == (manager._native_manager, int(catalog_type))
    api.root.Manager_GetAzureRegistryCatalog.assert_not_called()


@pytest.mark.parametrize("catalog_type", [CatalogType.PUBLIC, CatalogType.LOCAL])
def test_registry_name_is_rejected_for_non_azure_registry_catalogs(mock_catalog_manager, catalog_type):
    manager, api = mock_catalog_manager
    with pytest.raises(ValueError, match="only supported for CatalogType.AZURE_REGISTRY"):
        manager.get_catalog(catalog_type, registry_name="mock-registry")
    api.root.Manager_GetAzureRegistryCatalog.assert_not_called()
    api.root.Manager_GetCatalogByType.assert_not_called()


@pytest.mark.parametrize("registry_name", ["", "invalid\0registry"])
def test_invalid_registry_strings_never_reach_native(mock_catalog_manager, registry_name):
    manager, api = mock_catalog_manager
    with pytest.raises(ValueError):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name=registry_name)
    api.root.Manager_GetAzureRegistryCatalog.assert_not_called()


@pytest.mark.parametrize("api_version", [2, 3])
def test_azure_registry_catalog_requires_native_v4(mock_catalog_manager, api_version):
    manager, api = mock_catalog_manager
    api.api_version = api_version
    with pytest.raises(FoundryLocalException, match="version 4"):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="mock-registry")
    api.root.Manager_GetAzureRegistryCatalog.assert_not_called()


def test_native_failure_is_propagated_and_not_cached(mock_catalog_manager):
    manager, api = mock_catalog_manager
    api.check_status.side_effect = FoundryLocalException("mock registry failure")
    with pytest.raises(FoundryLocalException, match="mock registry failure"):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="mock-registry")
    assert not manager._catalogs
    api.check_status.side_effect = None
    assert manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="mock-registry") is not None
    assert api.root.Manager_GetAzureRegistryCatalog.call_count == 2


def test_missing_native_azure_registry_catalog_is_not_cached(mock_catalog_manager):
    manager, api = mock_catalog_manager
    api.root.Manager_GetAzureRegistryCatalog.side_effect = None
    with pytest.raises(FoundryLocalException, match="no azure_registry catalog"):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="mock-registry")
    assert not manager._catalogs


def test_closed_manager_cannot_return_a_cached_azure_registry_catalog(mock_catalog_manager):
    manager, api = mock_catalog_manager
    manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="mock-registry")
    manager._close_started.set()
    with pytest.raises(RuntimeError, match="closed"):
        manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="mock-registry")
    api.root.Manager_GetAzureRegistryCatalog.assert_called_once()


def test_missing_azure_registry_model_variant_returns_none(mock_catalog_manager):
    manager, api = mock_catalog_manager
    catalog = manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="mock-registry")

    def get_variant(_catalog, _model_id, out):
        out[0] = ffi.NULL
        return ffi.NULL

    api.catalog.GetModelVariant = Mock(side_effect=get_variant)
    assert catalog.get_model_variant("tiny-generic-cpu:999") is None
    api.catalog.GetModelVariant.assert_called_once()
    assert api.catalog.GetModelVariant.call_args.args[1] == b"tiny-generic-cpu:999"


@pytest.mark.parametrize("message", ["HTTP 401", "HTTP 403", "authentication failed", "transport failed"])
def test_azure_registry_model_lookup_errors_are_not_missing_variants(mock_catalog_manager, message):
    manager, api = mock_catalog_manager
    catalog = manager.get_catalog(CatalogType.AZURE_REGISTRY, registry_name="mock-registry")
    api.catalog.GetModelVariant = Mock(return_value=ffi.NULL)
    api.check_status.side_effect = FoundryLocalException(message)
    with pytest.raises(FoundryLocalException, match=message):
        catalog.get_model_variant("tiny-generic-cpu:999")