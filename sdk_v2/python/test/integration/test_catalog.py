# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Catalog flat-list semantics. Requires the manager singleton to be initialized."""
from __future__ import annotations

import pytest

from foundry_local_sdk import IModel
from foundry_local_sdk.imodel import _ModelImpl


class TestCatalogShape:
    def test_catalog_has_name(self, manager):
        assert isinstance(manager.catalog.name, str)

    def test_list_models_returns_list_of_imodel(self, manager):
        models = manager.catalog.list_models()
        assert isinstance(models, list)
        for m in models:
            assert isinstance(m, IModel)
            assert isinstance(m, _ModelImpl)

    def test_each_model_has_required_metadata(self, manager):
        models = manager.catalog.list_models()
        if not models:
            pytest.skip("Catalog is empty (no network or empty result).")
        for m in models[:5]:  # spot-check first few
            assert m.id
            assert m.alias
            info = m.info
            assert info.id == m.id
            assert info.alias == m.alias

    def test_get_model_by_alias_round_trip(self, manager):
        models = manager.catalog.list_models()
        if not models:
            pytest.skip("Catalog is empty.")
        m = models[0]
        looked_up = manager.catalog.get_model(m.alias)
        assert looked_up is not None
        assert looked_up.alias == m.alias

    def test_get_model_unknown_alias_returns_none(self, manager):
        result = manager.catalog.get_model("definitely-not-a-real-model-alias-xyz")
        assert result is None

    def test_get_model_variant_by_id_round_trip(self, manager):
        models = manager.catalog.list_models()
        if not models:
            pytest.skip("Catalog is empty.")
        m = models[0]
        variant = manager.catalog.get_model_variant(m.id)
        assert variant is not None
        assert variant.id == m.id

    def test_get_model_variant_unknown_id_returns_none(self, manager):
        assert manager.catalog.get_model_variant("not-a-real-id") is None

    def test_get_model_versions_returns_versions_for_alias(self, manager):
        models = manager.catalog.list_models()
        if not models:
            pytest.skip("Catalog is empty.")

        alias = next((m.alias for m in models if len(m.variants) > 1), models[0].alias)
        versions = manager.catalog.get_model_versions(alias)

        assert versions
        assert all(isinstance(v, IModel) for v in versions)
        assert all(v.alias == alias for v in versions)

    def test_get_model_versions_respects_max_versions_cap(self, manager):
        models = manager.catalog.list_models()
        if not models:
            pytest.skip("Catalog is empty.")

        alias = next((m.alias for m in models if len(m.variants) > 1), models[0].alias)
        versions = manager.catalog.get_model_versions(alias)
        capped = manager.catalog.get_model_versions(alias, max_versions=1)

        assert len(versions) >= 1
        assert len(capped) <= len(versions)

        # max_versions caps versions *per model name*, not the total result count. An alias
        # with several distinct model names can return one entry per name, so assert the cap
        # per name rather than on the overall length.
        counts_by_name: dict[str, int] = {}
        for v in capped:
            counts_by_name[v.info.name] = counts_by_name.get(v.info.name, 0) + 1
        assert all(count <= 1 for count in counts_by_name.values())

    def test_get_latest_version_returns_model_for_multi_variant_model(self, manager):
        models = manager.catalog.list_models()
        if not models:
            pytest.skip("Catalog is empty.")
        m = models[0]
        latest = manager.catalog.get_latest_version(m)
        assert latest is not None
        assert isinstance(latest, IModel)
        assert isinstance(latest, _ModelImpl)
        assert latest.alias == m.alias

    def test_get_latest_version_rejects_non_imodel(self, manager):
        from foundry_local_sdk.exception import FoundryLocalException

        with pytest.raises(FoundryLocalException):
            manager.catalog.get_latest_version("not-an-imodel")  # type: ignore[arg-type]


class TestSelectVariantMetadata:
    """The native model is the source of truth. After select_variant, all reported
    metadata (info, id, alias, ...) must reflect the selected variant even if info
    was read before selecting."""

    def _find_multi_variant_model(self, manager):
        for m in manager.catalog.list_models():
            if len(m.variants) > 1:
                return m
        return None

    def test_select_variant_refreshes_metadata(self, manager):
        model = self._find_multi_variant_model(manager)
        if model is None:
            pytest.skip("No multi-variant model in the catalog.")

        variants = model.variants

        # Read info BEFORE selecting — this is what used to prime a stale cache.
        # Derive the original variant from the currently-selected info rather than
        # assuming variants[0]: native selection prefers the first cached variant.
        info_before = model.info
        default_variant = next(v for v in variants if v.id == info_before.id)
        other_variant = next(v for v in variants if v.id != info_before.id)

        model.select_variant(other_variant)

        # Every read must reflect the selected variant.
        assert model.id == other_variant.id
        assert model.alias == other_variant.alias

        info_after = model.info
        assert info_after.id == other_variant.id
        assert info_after.name == other_variant.info.name
        assert info_after.version == other_variant.info.version

        # The earlier snapshot is an independent point-in-time value.
        assert info_before.id == default_variant.id

        # Selecting back refreshes again.
        model.select_variant(default_variant)
        assert model.info.id == default_variant.id
