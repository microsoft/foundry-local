# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""BYOM binding tests that do not download, load, or run model inference."""
from __future__ import annotations

import json
import uuid
from dataclasses import FrozenInstanceError, asdict, is_dataclass
from pathlib import Path

import pytest

from foundry_local_sdk import CatalogType, FoundryLocalException, IModel, ModelInfoBuilder


def test_manager_get_catalog_selects_public_and_local(manager) -> None:
    public_catalog = manager.get_catalog()
    local_catalog = manager.get_catalog(CatalogType.LOCAL)

    assert public_catalog is manager.catalog
    assert public_catalog is manager.get_catalog(CatalogType.PUBLIC)
    assert local_catalog is manager.get_catalog(CatalogType.LOCAL)
    assert public_catalog.catalog_type is CatalogType.PUBLIC
    assert local_catalog.catalog_type is CatalogType.LOCAL
    assert public_catalog is not local_catalog


def test_model_info_builder_properties_round_trip_and_close() -> None:
    metadata = ModelInfoBuilder()
    assert metadata.set_string_property("task", "chat-completion") is metadata
    assert metadata.set_string_property("custom-string", "custom-value") is metadata
    assert metadata.set_int_property("context_length", 4096) is metadata
    assert metadata.set_int_property("custom-int", 17) is metadata

    assert metadata.get_string_property("task") == "chat-completion"
    assert metadata.get_string_property("custom-string") == "custom-value"
    assert metadata.get_int_property("context_length", -1) == 4096
    assert metadata.get_int_property("custom-int", -1) == 17
    assert metadata.get_string_property("missing") is None
    assert metadata.get_int_property("missing", 23) == 23

    metadata.close()
    metadata.close()
    with pytest.raises(RuntimeError, match="closed"):
        metadata.get_string_property("task")


def test_local_catalog_registers_and_unregisters_without_deleting_assets(manager, tmp_path) -> None:
    model_path = tmp_path / "model"
    model_path.mkdir()
    config_path = model_path / "genai_config.json"
    config_path.write_text(json.dumps({"model": {"type": "phi3"}}), encoding="utf-8")

    model_id = f"python-byom-{uuid.uuid4().hex}:1"
    local_catalog = manager.get_catalog(CatalogType.LOCAL)
    registered: IModel | None = None
    try:
        with ModelInfoBuilder() as metadata:
            metadata.set_string_property("task", "chat-completion")
            metadata.set_string_property("display_name", "Python BYOM")
            metadata.set_string_property("custom_marker", "python-binding")
            metadata.set_int_property("context_length", 2048)
            metadata.set_int_property("custom_count", 42)
            registered = local_catalog.register_model(model_path, model_id, metadata)

        # RegisterModel copies metadata, so closing the builder cannot invalidate
        # the returned point-in-time metadata value.
        info = registered.info
        assert info.id == model_id
        assert info.name == model_id.rsplit(":", 1)[0]
        assert info.version == 1
        assert info.task == "chat-completion"
        assert info.context_length == 2048
        assert info.display_name == "Python BYOM"
        assert info.get_string_property("custom_marker") == "python-binding"
        assert info.get_string_property("model_provider") == "LocalRegistration"
        assert info.get_int_property("custom_count", -1) == 42
        assert is_dataclass(info)
        assert asdict(info)["id"] == model_id
        assert Path(registered.get_path()).resolve() == model_path.resolve()

        local_catalog.unregister_model(model_id)
        assert local_catalog.get_model_variant(model_id) is None
        assert config_path.is_file()

        # Outstanding model handles and immutable metadata remain valid after
        # unregistration for the lifetime of the owning catalog/manager.
        assert registered.info.id == model_id
        assert info.id == model_id
        registered = None
    finally:
        if registered is not None and local_catalog.get_model_variant(model_id) is not None:
            local_catalog.unregister_model(model_id)


def test_public_catalog_rejects_registration_before_native_call(manager, tmp_path) -> None:
    with ModelInfoBuilder() as metadata:
        metadata.set_string_property("task", "chat-completion")
        with pytest.raises(FoundryLocalException, match="local catalog"):
            manager.catalog.register_model(tmp_path, "not-local:1", metadata)


def test_model_info_is_a_frozen_value_snapshot(manager) -> None:
    models = manager.catalog.list_models()
    if not models:
        pytest.skip("Public catalog is empty.")

    info = models[0].info
    with pytest.raises(FrozenInstanceError):
        info.task = "embeddings"  # type: ignore[misc]